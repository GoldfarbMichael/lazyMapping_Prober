// fingerprint_orchestrator.c
// -----------------------------------------------------------------------------
// Stage 3: real-browser stress-ng fingerprinting.
//
// This process (the ORCHESTRATOR) reproduces the C fingerprinting battery
// (runStressNG_batches in mastikElite.c) but moves the memorygram SAMPLING into a
// real Chrome browser running the JS lazy mapping (JavaScript/main.js, ?mode=fingerprint).
// It does NO cache probing itself -- all measurement is in JS -- so it needs no Mastik
// mapping, no hugepages, and no root.
//
// Cores:  Chrome (the sampler) -> core 0,  stress-ng -> core 1  (orchestrator -> core 2).
//
// Flow (per sample), coordinated via the Flask /fp/* endpoints:
//   1. launch Chrome on the ?mode=fingerprint page; it builds the lazy mapping ONCE and
//      POSTs /fp/ready. We block on /fp/state until ready.
//   2. fork+exec a stress-ng stressor pinned to core 1; let it reach steady state.
//   3. POST /fp/cmd {sample} -> server bumps seq; the browser samples the memorygram with
//      NO network in the loop, POSTs the CSV to /collect, then acks /fp/done {seq}.
//   4. block on /fp/state until done_seq == seq, then SIGKILL the stressor + the cooldown.
//   5. next stressor.
//
// Sampling is stressor-by-stressor (round-robin: outer loop = iteration, inner = stressor)
// so each stressor's samples are spread across the whole run -- this avoids
// fingerprinting a drifting machine state.
//
// Parameters come in as the SAME config label the native sampler uses for its output dir --
// {NoC}C_{TST}TST_{K}K_{CYCLES}cycles, e.g. "16C_2TST_0K_4576cycles" -- parsed with mastikElite.c's
// parse_*_from_dirname(). run_fingerprint_sweep.sh composes one label per NoC exactly the way
// run_all_configs.sh does for batch_runner.sh, so an experiment's parameter tuple travels unchanged
// from the sweep script -> this process -> the JS URL -> the CSV tree -> the .h5 filename.
// A bare NoC is still accepted (defaults for the rest) for quick manual runs.
// One lazy mapping per invocation.
// -----------------------------------------------------------------------------

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "mastikElite.h"   // stress_battery, NUM_STRESSORS, pin_to_core, cleanup_handler
#include "fp_ctl.h"        // raw-socket HTTP + JSON scalars (shared with website_orchestrator.c)

#define BROWSER_CORE   0           // Chrome (the JS sampler)
#define STRESSOR_CORE  1           // stress-ng
#define ORCH_CORE      2           // this orchestrator (keep cores 0/1 clean)

// DEFAULTS ONLY. The sampling tuple is normally supplied as a config label (argv[1]); these apply
// to the bare-NoC form and to an unset FP_SAMPLES / FP_COOLDOWN_US. They match the JS defaults /
// the main stress-ng eval config. Every one of them is a RUNTIME value below -- do not reintroduce
// compile-time uses, or the label would stop describing the data.
#define DEF_NUM_SAMPLES 50         // iterations (samples) per stressor
#define DEF_FP_TST      2          // total sampling time (s)
#define DEF_FP_K        0          // accesses between timer polls (JS side); 0 = dynamic K
#define DEF_FP_CYCLES   4576       // est. CPU cycles per address (JS Q sizing)
#define DEF_COOLDOWN_US 500000     // cooldown between samples, MICROSECONDS (0.5 s):
                                   // lets the L3 return to baseline before the next trace
#define STEADY_US      50000       // grace for the stressor to reach its loop before sampling
#define READY_TIMEOUT_S 120        // max wait for Chrome to load + build the mapping
                                   // (cold Chrome-as-root startup alone can take ~20-30s)
#define CHROME_PROFILE "/tmp/chrome-fingerprint"

// True iff x is a power of two in [1, 64] (lazy-mapping regime).
static int is_pow2_le64(int x) {
    return x >= 1 && x <= 64 && (x & (x - 1)) == 0;
}

// True iff s is a non-empty run of digits -- i.e. the legacy bare-NoC argv[1] rather than a label.
static int is_all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

// Read a non-negative int from the environment. These are the FALLBACK for the optional
// positional args: sudo strips the environment, and reaching the env through `sudo env FP_SAMPLES=..`
// would need /usr/bin/env in sudoers -- which is equivalent to NOPASSWD: ALL, since `sudo env` can
// exec anything as root. Positional args keep the existing per-binary NOPASSWD rule sufficient, so
// the sweep runs unattended without weakening sudo. Unset/empty/malformed -> dflt.
static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end;
    long x = strtol(v, &end, 10);
    if (*end != '\0' || x < 0 || x > INT_MAX) {
        fprintf(stderr, "[fp] WARNING: ignoring malformed %s='%s'; using %d\n", name, v, dflt);
        return dflt;
    }
    return (int)x;
}

// Parse a non-negative int argument with range checking. atoi() would silently overflow on a long
// digit run -- more plausible now that the cooldown is expressed in microseconds. -1 on error.
static int parse_nonneg_arg(const char *s, const char *what) {
    if (!is_all_digits(s)) {
        fprintf(stderr, "[fp] ERROR: %s must be a non-negative integer, got '%s'\n", what, s);
        return -1;
    }
    errno = 0;
    char *end;
    long x = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || x > INT_MAX) {
        fprintf(stderr, "[fp] ERROR: %s out of range: '%s' (max %d)\n", what, s, INT_MAX);
        return -1;
    }
    return (int)x;
}

// usleep() is specified only for values below 1e6 (POSIX allows EINVAL at or above that; glibc
// happens to tolerate more). Now that the cooldown knob is in microseconds, a >= 1 s value must
// not risk becoming a silent no-op, so split it into whole seconds + remainder.
static void sleep_us(int usec) {
    if (usec <= 0) return;
    if (usec >= 1000000) sleep((unsigned int)(usec / 1000000));
    usleep((useconds_t)(usec % 1000000));
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <config|NoC> [SAMPLES] [COOLDOWN_US]\n"
        "  <config>     config label, e.g. 16C_2TST_0K_4576cycles -- NoC/TST/K/cycles come from it\n"
        "  <NoC>        bare power of two in [1,64]; TST/K/cycles fall back to the defaults\n"
        "               (%dTST, %dK, %dcycles)\n"
        "  [SAMPLES]    samples per stressor           (default %d)\n"
        "  [COOLDOWN_US] cooldown between samples, MICROSECONDS (default %d = %.2f s)\n"
        "environment (used only when the positional arg is absent):\n"
        "  FP_SAMPLES, FP_COOLDOWN_US\n",
        prog, DEF_FP_TST, DEF_FP_K, DEF_FP_CYCLES, DEF_NUM_SAMPLES,
        DEF_COOLDOWN_US, DEF_COOLDOWN_US / 1e6);
}

// The raw-socket HTTP + JSON-scalar helpers now live in fp_ctl.c, shared with
// website_orchestrator.c. They are port-parameterized (this file only ever talks to
// FP_SERVER_PORT) and no longer build the request in a fixed buffer -- see fp_ctl.h.

static pid_t launch_chrome(int noc, int tst, int k, int cycles) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://localhost:%d/?mode=fingerprint&label=fp_%dC_%dTST_%dK_%dcycles",
             FP_SERVER_PORT, noc, tst, k, cycles);
    pid_t pid = fork();
    if (pid == 0) {
        pin_to_core(BROWSER_CORE);           // inherited by chrome's child procs (renderer/GPU/...)
        setenv("DISPLAY", ":0", 1);
        // Orchestrator runs as root; the :0 X server's auth cookie lives in gdm's
        // Xauthority (NOT /home/ubu/.Xauthority, which is the :1 Xtigervnc cookie).
        // Without this Chrome gets "No protocol specified" / "Missing X server".
        setenv("XAUTHORITY", "/run/user/1000/gdm/Xauthority", 1);
        execlp("google-chrome", "google-chrome",
               "--no-sandbox",  // orchestrator runs as root; Chrome zygote aborts otherwise.
                                // Disables only seccomp/namespace syscall isolation, not Site
                                // Isolation or V8 allocation -> no effect on the LLC cache signal.
               "--user-data-dir=" CHROME_PROFILE,
               "--no-first-run", "--no-default-browser-check",
               "--new-window", url, (char *)NULL);
        perror("execlp google-chrome");
        _exit(127);
    }
    return pid;
}



// Signal handler: tear down OUR Chrome (the whole process tree, matched by its profile
// dir) and any stressor, then exit. A terminal Ctrl+C delivers SIGINT to the whole
// foreground group, so this root process receives it directly and self-cleans -- no
// orphaned Chrome/stress-ng. (system() in a handler isn't async-signal-safe, but this is
// the existing project pattern for a cleanup-then-exit handler.)
static void fp_cleanup(int sig) {
    (void)sig;
    system("pkill -9 -f 'user-data-dir=" CHROME_PROFILE "'");
    system("pkill -9 stress-ng");
    _exit(130);
}

// Block until the browser has built the mapping (/fp/state ready), or time out.
static int wait_ready(void) {
    char resp[512];
    for (int s = 0; s < READY_TIMEOUT_S; s++) {
        if (fp_http_get(FP_SERVER_PORT, "/fp/state", resp, sizeof(resp)) == 0 && fp_json_bool(resp, "ready"))
            return 0;
        sleep(1);
    }
    return -1;
}

// POST a sample request for `workload`/`config`; return the assigned seq.
static long request_sample(const char *workload, const char *config) {
    // Escape before interpolating: a stray quote or backslash would produce a body Flask
    // parses as {} (get_json(silent=True)), which silently reroutes the CSV to the "manual"
    // class dir instead of failing. Buffers are sized so nothing can truncate into that.
    char wl[512], cf[256], body[1024], resp[512];
    if (fp_json_escape(workload, wl, sizeof(wl)) != 0 ||
        fp_json_escape(config,   cf, sizeof(cf)) != 0) {
        fprintf(stderr, "[fp] ERROR: workload/config too long to encode\n");
        return -1;
    }
    snprintf(body, sizeof(body),
             "{\"cmd\":\"sample\",\"workload\":\"%s\",\"config\":\"%s\"}", wl, cf);
    if (fp_http_post(FP_SERVER_PORT, "/fp/cmd", body, resp, sizeof(resp)) != 0) return -1;
    return fp_json_int(resp, "seq", -1);
}

// Block until the browser acks it sampled + saved `seq`.
static void wait_done(long seq) {
    char resp[512];
    for (;;) {
        if (fp_http_get(FP_SERVER_PORT, "/fp/state", resp, sizeof(resp)) == 0 &&
            fp_json_int(resp, "done_seq", -1) >= seq)
            return;
        usleep(20000); // 20 ms
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *arg = argv[1];

    int noc, tst, k, cycles;
    if (is_all_digits(arg)) {
        // Legacy bare-NoC form: keep quick manual single-NoC runs working.
        noc = atoi(arg); tst = DEF_FP_TST; k = DEF_FP_K; cycles = DEF_FP_CYCLES;
    } else {
        // Config label, parsed with the SAME parsers the native sampler applies to its output dir
        // name (mastikElite.c), so one label means exactly one thing across the whole project.
        noc    = parse_NoC_from_dirname(arg);
        tst    = parse_TST_from_dirname(arg);
        k      = parse_K_from_dirname(arg);       // 0 is VALID here; only -1 means "absent"
        cycles = parse_cycles_from_dirname(arg);
        if (noc < 0 || tst < 0 || k < 0 || cycles < 0) {
            fprintf(stderr, "[fp] ERROR: cannot parse '%s' as "
                            "{NoC}C_{TST}TST_{K}K_{CYCLES}cycles\n", arg);
            usage(argv[0]);
            return 2;
        }
    }
    // Precedence: positional arg > environment > compiled-in default.
    int num_samples = env_int("FP_SAMPLES", DEF_NUM_SAMPLES);
    int cooldown_us  = env_int("FP_COOLDOWN_US", DEF_COOLDOWN_US);
    if (argc > 2) {
        num_samples = parse_nonneg_arg(argv[2], "SAMPLES");
        if (num_samples < 0) return 2;
    }
    if (argc > 3) {
        cooldown_us = parse_nonneg_arg(argv[3], "COOLDOWN_US");
        if (cooldown_us < 0) return 2;
    }

    // Validate everything BEFORE launching Chrome or a stressor: a bad tuple must cost seconds,
    // not a multi-hour run that writes a mislabelled tree.
    if (!is_pow2_le64(noc)) {
        fprintf(stderr, "[fp] ERROR: NoC must be a power of two in [1,64], got %d\n", noc);
        return 2;
    }
    if (tst < 1) {
        fprintf(stderr, "[fp] ERROR: TST must be >= 1 s, got %d\n", tst); return 2;
    }
    if (cycles < 1) {
        fprintf(stderr, "[fp] ERROR: cycles/address must be >= 1, got %d\n", cycles); return 2;
    }
    if (num_samples < 1) {
        fprintf(stderr, "[fp] ERROR: FP_SAMPLES must be >= 1, got %d\n", num_samples); return 2;
    }

    signal(SIGINT, fp_cleanup);   // self-tear-down Chrome + stress-ng on Ctrl+C / TERM
    signal(SIGTERM, fp_cleanup);

    // Keep cores 0/1 reserved for Chrome / stress-ng; the orchestrator just coordinates.
    pin_to_core(ORCH_CORE);

    // Config string drives /collect's path: data/realbrowser_{NoC}C_.../{stressor}/{n}.csv.
    // Rebuilt from the PARSED values (not copied from argv) so the bare-NoC form and any
    // zero-padded label still produce the canonical dir name finalize_realbrowser.sh expects.
    // The "realbrowser_" tag is load-bearing: a manual single-shot browser run sets its own
    // metadata and writes a BARE {NoC}C_... dir under the same JavaScript/data root, so without
    // the tag it could drop stray CSVs into a sweep's class dirs.
    char config[128];
    snprintf(config, sizeof(config), "realbrowser_%dC_%dTST_%dK_%dcycles", noc, tst, k, cycles);

    size_t num_stressors = stress_battery_count();
    printf("[fp] config : NoC=%d  TST=%ds  K=%d  cycles/addr=%d\n", noc, tst, k, cycles);
    printf("[fp] battery: %zu stressors x %d samples = %zu samples, %d us (%.3f s) cooldown each\n",
           num_stressors, num_samples, num_stressors * (size_t)num_samples,
           cooldown_us, cooldown_us / 1e6);
    printf("[fp] output : data/%s/<stressor>/<n>.csv\n", config);

    // Clear any stale coordinator state (e.g. a "ready" left by a previous, now-dead
    // browser) BEFORE launching Chrome, so wait_ready() only passes on the NEW browser.
    fp_http_post(FP_SERVER_PORT, "/fp/reset", "{}", NULL, 0);

    pid_t chrome = launch_chrome(noc, tst, k, cycles);
    printf("[fp] launched chrome (pid %d) on core %d; waiting for mapping build...\n",
           chrome, BROWSER_CORE);
    if (wait_ready() != 0) {
        fprintf(stderr, "[fp] FATAL: browser never became ready (is the server running "
                        "on :%d, and Chrome reachable on DISPLAY :0?)\n", FP_SERVER_PORT);
        if (chrome > 0) kill(chrome, SIGTERM);
        return 1;
    }
    printf("[fp] browser ready. starting round-robin collection.\n");

    // Round-robin: spread each stressor's samples across the whole run (state-drift safe).
    for (int iter = 0; iter < num_samples; iter++) {
        for (size_t s = 0; s < num_stressors; s++) {
            const char *name = stress_battery[s].stressor_name;
            printf("[fp] iter %d/%d  stressor %s\n", iter + 1, num_samples, name);

            // 1. Fork the noise injector (stress-ng) pinned to core 1.
            pid_t pid = fork();
            if (pid < 0) { perror("FATAL: fork"); break; }
            if (pid == 0) {
                pin_to_core(STRESSOR_CORE);
                execvp(stress_battery[s].exec_args[0], stress_battery[s].exec_args);
                perror("FATAL: execvp stress-ng");
                _exit(127);
            }

            // 2. Let the stressor reach steady state, then request a sample.
            usleep(STEADY_US);
            long seq = request_sample(name, config);
            if (seq < 0) {
                fprintf(stderr, "[fp] WARNING: sample request failed (iter %d, %s)\n", iter, name);
            } else {
                // 3. Block until the browser sampled + saved the CSV.
                sleep(tst);
                usleep(500000);   // +0.5s margin for poll pickup + /collect + /fp/done latency
                wait_done(seq);
            }

            // 4. Terminate the stressor and reap it.
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);

            // 5. Cooldown so the L3 returns to baseline before the next sample.
            sleep_us(cooldown_us);
        }
    }

    // Signal the browser to exit its loop and tear down Chrome.
    fp_http_post(FP_SERVER_PORT, "/fp/cmd", "{\"cmd\":\"stop\"}", NULL, 0);
    if (chrome > 0) kill(chrome, SIGTERM);
    printf("[fp] collection complete.\n");
    return 0;
}
