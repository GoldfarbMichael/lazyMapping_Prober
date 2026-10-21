// website_orchestrator.c
// -----------------------------------------------------------------------------
// Stage 4: real-browser WEBSITE fingerprinting.
//
// Same coordinator protocol as fingerprint_orchestrator.c (Stage 3), with the victim
// swapped from a stress-ng process to a real website loaded in a SECOND TAB of the SAME
// Chrome that runs the sampler. This is the Shusterman et al. (USENIX Sec '19) threat
// model: attacker JS and victim page are tabs in one browser.
//
// It does NO cache probing itself -- all measurement is in JS (JavaScript/main.js,
// ?mode=fingerprint) -- and it does no CPU pinning, so it needs no Mastik mapping, no
// hugepages, and NO ROOT. Chrome therefore keeps its sandbox (no --no-sandbox), which
// matters here in a way it did not in Stage 3: this arm navigates to arbitrary live sites.
//
// WHY A SEPARATE FILE FROM fingerprint_orchestrator.c
// A website breaks every assumption of the stress-ng victim contract ("one static argv ->
// fork -> pin -> exec -> 50 ms to steady state -> SIGKILL one pid"):
//   * a page load is a TRANSIENT, not a steady state -- the discriminative signal is in the
//     first seconds, so the trace must be ALIGNED TO NAVIGATION START rather than started
//     after the victim settles;
//   * Chrome is a process tree, not a pid, and the victim shares it with the sampler;
//   * the victim is created/destroyed over CDP, not fork/exec/kill.
// The shared half (HTTP, JSON, label parsing) is factored into fp_ctl.c instead.
//
// NO CPU PINNING (deliberate, and a departure from Stages 1-3)
// Chrome is free to spread across all cores. The cost is that the channel can no longer be
// claimed LLC-only: unpinned, the two renderers may transiently co-reside on one core and
// share its private L1/L2, and the sampler may migrate mid-trace. With two busy renderers
// on 8 otherwise-idle cores that is a tail effect, but the honest claim for this arm is
// "microarchitectural contention", not "LLC contention" -- the same caveat that applies to
// the cache-occupancy channel in the literature. Stages 1-3 remain the controlled bound.
//
// PER-SAMPLE ORDERING (this is the load-bearing part)
//   1. GET :9222/json/version               -- CDP health check, OUTSIDE the trace window
//   2. POST /fp/cmd {sample}                -- attacker tab is still foreground here
//   3. block on /fp/state started_seq       -- sampler is now INSIDE its synchronous loop
//   4. PUT :9222/json/new?<url>             -- navigation starts; t = 0 of the trace
//   5. sleep(TST) + margin, block on done_seq
//   6. GET :9222/json/close/<id>
//   7. cooldown
// Step 3 exists because creating the victim tab backgrounds the attacker tab, and Chrome
// throttles background-tab timers (~1/min, then freezing). main.js's sampling loop is
// synchronous and immune ONCE RUNNING, but its /fp/poll idle loop is not -- so the sample
// must be under way before the victim tab exists. The throttling-disable flags below are
// belt-and-braces on top of that ordering.
//
// Parameters arrive as the SAME config label the rest of the project uses --
// {NoC}C_{TST}TST_{K}K_{CYCLES}cycles -- so an experiment's tuple travels unchanged from
// run_website_sweep.sh -> here -> the JS URL -> the CSV tree -> the .h5 filename.
//
// NOISE INJECTION (NOISE_P > 0): FlipStress (Basak et al., SecureComm 2025)
// The FlipStress defense -- p web workers that all switch to a randomly chosen cache stressor
// every 500 ms -- runs in a NOISE TAB opened FIRST, before the sampler. Tab order is
// noise | sampler | victim, so closing the victim (the last tab) re-activates the sampler
// exactly as in the noiseless layout. The noise is started ONCE per Chrome (= once per batch)
// and is deliberately NOT synchronised to samples: the defense runs continuously, unaware of
// the attacker, and restarting it per sample would put the same flip phase at t=0 of every
// trace. Instead:
//   * wait_noise_ready() blocks on the tab's "ready" (all workers built their buffers) before
//     the sampler is even opened -- a fixed sleep would give the FIRST site of every round
//     weaker noise, a bias that tracks the class label;
//   * noise_check() runs before every sample request AND after every trace. A noise tab that
//     died or stalled aborts the run, so a website_p*flip tree never silently holds un-noised
//     traces.
// The schedule seed is NOISE_SEED_BASE + BATCH (both from the sweep), so every batch's
// stressor sequence is reproducible and logged. Noisy data goes to website_p{p}flip_..., never
// into the clean website_... tree. NOISE_P = 0 is the original code path, unchanged.
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
#include <time.h>

#include "mastikElite.h"   // parse_*_from_dirname
#include "fp_ctl.h"        // raw-socket HTTP + JSON scalars (shared with fingerprint_orchestrator.c)
#include "web_victim.h"    // site list + Chrome + CDP victim tab (shared with website_prober.c)

// DEFAULTS ONLY. The sampling tuple normally comes from the config label (argv[1]); these
// apply to the bare-NoC form. Every one is a RUNTIME value below -- do not reintroduce
// compile-time uses, or the label would stop describing the data.
#define DEF_NUM_SAMPLES 50         // iterations (samples) per site
#define DEF_WS_TST      6          // total sampling time (s)
#define DEF_WS_K        180        // accesses between timer polls (JS side); 0 = dynamic K
#define DEF_WS_CYCLES   2288       // est. CPU cycles per address (JS Q sizing)
#define DEF_COOLDOWN_US 500000     // cooldown between samples, MICROSECONDS (0.5 s)
#define DEF_SITES_FILE  "sites.txt"

#define READY_TIMEOUT_S  120       // max wait for Chrome to load + build the lazy mapping
#define CDP_TIMEOUT_S    60        // max wait for the DevTools endpoint to come up
#define STARTED_TIMEOUT_S 30       // max wait for the browser to enter its sampling loop
#define DONE_TIMEOUT_S   60        // max wait for the browser to POST the CSV and ack

// Distinct from Stage 3's /tmp/chrome-fingerprint on purpose. Chrome reuses a running
// instance when --user-data-dir matches, so a shared profile would open the sampler page as
// a TAB in the other run's window instead of a new browser -- and would make either run's
// cleanup pkill tear down both.
#define CHROME_PROFILE "/tmp/chrome-website"

// Warn if the gap between "sample requested" and "victim navigating" exceeds this. That gap
// is dead time at the HEAD of the memorygram -- the window where a page load is most
// distinctive -- so silent drift here would quietly degrade every trace in a long run.
#define ALIGN_WARN_MS   150

// ---- FlipStress noise tab (NOISE_P > 0) ----
#define NOISE_MAX_P            16    // keep in sync with flipstress/noise.html MAX_P
#define DEF_NOISE_LLC_MB       12    // stressor buffer size; = LLC_SIZE_MB in JavaScript/main.js
#define NOISE_MAX_LLC_MB       64
#define NOISE_READY_TIMEOUT_S 180    // p workers x 7 stressors' buffers; grows with p
// The tab posts a heartbeat at most once per second, driven by worker flips (~every 500 ms),
// so a healthy beat is <~1.5 s old. Older than this, or the slowest worker's flip count
// frozen this long, means the noise is gone.
#define NOISE_STALE_MS       3000

static Site  sites[WEB_MAX_SITES];   // Site, the caps, and the loader live in web_victim.h
static int   num_sites = 0;

// Set once launch_chrome() returns, so the signal handler can tear the browser down.
static volatile sig_atomic_t chrome_pid = 0;

// Monotonic milliseconds; used only to measure navigation alignment.
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// Argument / label parsing (same helpers and validation shape as Stage 3)
// ---------------------------------------------------------------------------

// True iff x is a power of two in [1, 64] (the Lazy Mapping regime).
static int is_pow2_le64(int x) {
    return x >= 1 && x <= 64 && (x & (x - 1)) == 0;
}

// True iff s is a non-empty run of digits -- i.e. the bare-NoC argv[1] rather than a label.
static int is_all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

// Parse a non-negative int argument with range checking. atoi() would silently overflow on
// a long digit run -- plausible now that the cooldown is expressed in microseconds.
static int parse_nonneg_arg(const char *s, const char *what) {
    if (!is_all_digits(s)) {
        fprintf(stderr, "[web] ERROR: %s must be a non-negative integer, got '%s'\n", what, s);
        return -1;
    }
    errno = 0;
    char *end;
    long x = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || x > INT_MAX) {
        fprintf(stderr, "[web] ERROR: %s out of range: '%s' (max %d)\n", what, s, INT_MAX);
        return -1;
    }
    return (int)x;
}

// usleep() is specified only below 1e6 (POSIX allows EINVAL at or above that). The cooldown
// knob is in microseconds, so a >= 1 s value must not become a silent no-op.
static void sleep_us(int usec) {
    if (usec <= 0) return;
    if (usec >= 1000000) sleep((unsigned int)(usec / 1000000));
    usleep((useconds_t)(usec % 1000000));
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <config|NoC> [SAMPLES] [COOLDOWN_US] [SITES_FILE]\n"
        "                [NOISE_P] [NOISE_LLC_MB] [NOISE_SEED_BASE] [BATCH]\n"
        "  <config>      config label, e.g. 16C_6TST_180K_2288cycles -- NoC/TST/K/cycles come from it\n"
        "  <NoC>         bare power of two in [1,64]; TST/K/cycles fall back to the defaults\n"
        "                (%dTST, %dK, %dcycles)\n"
        "  [SAMPLES]     samples per site                (default %d)\n"
        "  [COOLDOWN_US] cooldown between samples, MICROSECONDS (default %d = %.2f s)\n"
        "  [SITES_FILE]  '<slug>\\t<url>' per line, '#' comments (default %s)\n"
        "  [NOISE_P]     FlipStress workers in a noise tab opened first, 0..%d (default 0 = off)\n"
        "  [NOISE_LLC_MB] stressor buffer size, MB      (default %d)\n"
        "  [NOISE_SEED_BASE] [BATCH]  flip-schedule seed = base + batch (defaults 0, 1)\n"
        "\n"
        "Runs as the LOGIN USER -- no sudo. Chrome keeps its sandbox.\n",
        prog, DEF_WS_TST, DEF_WS_K, DEF_WS_CYCLES, DEF_NUM_SAMPLES,
        DEF_COOLDOWN_US, DEF_COOLDOWN_US / 1e6, DEF_SITES_FILE, NOISE_MAX_P, DEF_NOISE_LLC_MB);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

// The Stage 4 JS arm runs UNPRIVILEGED and UNPINNED, so it takes web_victim's defaults for
// both: no privilege drop (there is nothing to drop from) and no affinity mask (this process
// was never pinned, so Chrome inherits an all-cores mask already).
// `initial_url` is the sampler page, or the noise page when NOISE_P > 0 (the sampler then
// follows as a second tab, see main).
static pid_t launch_chrome(const char *initial_url) {
    WebVictimCfg cfg = {
        .profile_dir  = CHROME_PROFILE,
        .initial_url  = initial_url,
        .drop_privs   = 0,
        .uid = 0, .gid = 0, .home = NULL,
        .no_sandbox   = 0,   // not root -> the zygote sandbox works, and it is worth keeping
                             // while navigating to arbitrary live sites
        .set_affinity = 0,   // deliberately unpinned; see the header
        .cpu_mask     = NULL,
    };
    return web_launch_chrome(&cfg);
}

// Tear down OUR Chrome (the whole process tree, matched by its profile dir), then exit.
//
// The bracket around one character keeps the pattern from matching this pkill's own
// /bin/sh -c command line, which contains the pattern verbatim -- otherwise pkill races to
// kill the shell that is running it. (system() in a handler is not async-signal-safe, but
// this is the existing project pattern for a cleanup-then-exit handler.)
static void web_cleanup(int sig) {
    (void)sig;
    web_pkill_armed();
    _exit(130);
}

// ---------------------------------------------------------------------------
// Coordinator (Flask, :8080)
// ---------------------------------------------------------------------------

// Block until the browser has built the lazy mapping (/fp/state ready), or time out.
static int wait_ready(void) {
    char resp[512];
    for (int s = 0; s < READY_TIMEOUT_S; s++) {
        if (fp_http_get(FP_SERVER_PORT, "/fp/state", resp, sizeof(resp)) == 0 &&
            fp_json_bool(resp, "ready"))
            return 0;
        sleep(1);
    }
    return -1;
}

// POST a sample request for `workload`/`config`; return the assigned seq, or -1.
static long request_sample(const char *workload, const char *config) {
    char wl[512], cf[256], body[1024], resp[512];
    if (fp_json_escape(workload, wl, sizeof(wl)) != 0 ||
        fp_json_escape(config,   cf, sizeof(cf)) != 0) {
        fprintf(stderr, "[web] ERROR: workload/config too long to encode\n");
        return -1;
    }
    snprintf(body, sizeof(body),
             "{\"cmd\":\"sample\",\"workload\":\"%s\",\"config\":\"%s\"}", wl, cf);
    if (fp_http_post(FP_SERVER_PORT, "/fp/cmd", body, resp, sizeof(resp)) != 0) return -1;
    return fp_json_int(resp, "seq", -1);
}

// Block until the browser reports it has ENTERED the sampling loop for `seq`. This is the
// alignment gate: everything after it happens inside the trace window. Polled tightly (1 ms)
// because the delay here is dead time at the head of the memorygram.
// Returns 0, or -1 on timeout.
static int wait_started(long seq) {
    char resp[512];
    for (int i = 0; i < STARTED_TIMEOUT_S * 1000; i++) {
        if (fp_http_get(FP_SERVER_PORT, "/fp/state", resp, sizeof(resp)) == 0 &&
            fp_json_int(resp, "started_seq", -1) >= seq)
            return 0;
        usleep(1000);
    }
    return -1;
}

// Block until the browser acks it sampled + saved `seq`. Returns 0, or -1 on timeout.
static int wait_done(long seq) {
    char resp[512];
    for (int i = 0; i < DONE_TIMEOUT_S * 50; i++) {
        if (fp_http_get(FP_SERVER_PORT, "/fp/state", resp, sizeof(resp)) == 0 &&
            fp_json_int(resp, "done_seq", -1) >= seq)
            return 0;
        usleep(20000); // 20 ms
    }
    return -1;
}

// ---------------------------------------------------------------------------
// FlipStress noise tab (server.py /noise/*)
// ---------------------------------------------------------------------------

// Stall tracking for noise_check(): the slowest worker's flip count, and when it last rose.
static long   noise_last_flips  = -1;
static double noise_last_adv_ms = 0;
static int    noise_mismatch_reported = 0;

// Block until every noise worker has built its buffers and is flipping, then print the
// launch's seed and schedule into the run log. Fails fast on a reported page error.
// Returns 0, or -1.
static int wait_noise_ready(void) {
    char resp[1024], err[256];
    for (int s = 0; s < NOISE_READY_TIMEOUT_S; s++) {
        if (fp_http_get(FP_SERVER_PORT, "/noise/state", resp, sizeof(resp)) == 0) {
            if (fp_json_str(resp, "error", err, sizeof(err)) == 0 && err[0]) {
                fprintf(stderr, "[web] FATAL: noise tab reported: %s\n", err);
                return -1;
            }
            if (fp_json_bool(resp, "ready")) {
                char sched[256] = "?";
                fp_json_str(resp, "schedule", sched, sizeof(sched));
                printf("[web] noise ready: p=%ld llc=%ldMB seed=%ld init=%ldms (%d s wait) "
                       "schedule=[%s...]\n",
                       fp_json_int(resp, "p", -1), fp_json_int(resp, "llc_mb", -1),
                       fp_json_int(resp, "seed", -1), fp_json_int(resp, "init_ms", -1), s, sched);
                fflush(stdout);
                noise_last_flips  = -1;
                noise_last_adv_ms = now_ms();
                return 0;
            }
        }
        sleep(1);
    }
    fprintf(stderr, "[web] FATAL: noise tab never became ready within %d s\n", NOISE_READY_TIMEOUT_S);
    return -1;
}

// Is the noise still running? Checks: no page error, a fresh heartbeat, and the SLOWEST
// worker's flip count still rising (a single stalled worker would otherwise hide behind the
// others' heartbeats). A schedule mismatch does not abort -- the cache is still being
// stressed -- but it means the logged schedule is wrong, so it is reported loudly once.
// Returns 0 if healthy, -1 (with the reason printed) if not.
static int noise_check(void) {
    char resp[1024], err[256];
    if (fp_http_get(FP_SERVER_PORT, "/noise/state", resp, sizeof(resp)) != 0) {
        fprintf(stderr, "[web] noise: /noise/state unreachable\n");
        return -1;
    }
    if (fp_json_str(resp, "error", err, sizeof(err)) == 0 && err[0]) {
        fprintf(stderr, "[web] noise: tab reported: %s\n", err);
        return -1;
    }
    long age = fp_json_int(resp, "beat_age_ms", -1);
    if (age < 0 || age > NOISE_STALE_MS) {
        fprintf(stderr, "[web] noise: heartbeat stale (%ld ms, limit %d ms)\n", age, NOISE_STALE_MS);
        return -1;
    }
    double t = now_ms();
    long flips = fp_json_int(resp, "min_flips", -1);
    if (flips > noise_last_flips) {
        noise_last_flips  = flips;
        noise_last_adv_ms = t;
    } else if (t - noise_last_adv_ms > NOISE_STALE_MS) {
        fprintf(stderr, "[web] noise: slowest worker stuck at %ld flips for %.0f ms\n",
                flips, t - noise_last_adv_ms);
        return -1;
    }
    long mism = fp_json_int(resp, "mismatches", 0);
    if (mism > 0 && !noise_mismatch_reported) {
        fprintf(stderr, "[web] WARNING: noise workers deviated from the logged schedule "
                        "(%ld flip(s)) -- the run's seed no longer reproduces its noise\n", mism);
        noise_mismatch_reported = 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *arg = argv[1];

    int noc, tst, k, cycles;
    if (is_all_digits(arg)) {
        // Bare-NoC form: keeps quick manual single-NoC runs working.
        noc = atoi(arg); tst = DEF_WS_TST; k = DEF_WS_K; cycles = DEF_WS_CYCLES;
    } else {
        // Config label, parsed with the SAME parsers the native sampler applies to its output
        // dir name (mastikElite.c), so one label means exactly one thing across the project.
        noc    = parse_NoC_from_dirname(arg);
        tst    = parse_TST_from_dirname(arg);
        k      = parse_K_from_dirname(arg);       // 0 is VALID here; only -1 means "absent"
        cycles = parse_cycles_from_dirname(arg);
        if (noc < 0 || tst < 0 || k < 0 || cycles < 0) {
            fprintf(stderr, "[web] ERROR: cannot parse '%s' as "
                            "{NoC}C_{TST}TST_{K}K_{CYCLES}cycles\n", arg);
            usage(argv[0]);
            return 2;
        }
    }

    int num_samples = DEF_NUM_SAMPLES;
    int cooldown_us = DEF_COOLDOWN_US;
    const char *sites_file = DEF_SITES_FILE;
    if (argc > 2) { num_samples = parse_nonneg_arg(argv[2], "SAMPLES");     if (num_samples < 0) return 2; }
    if (argc > 3) { cooldown_us = parse_nonneg_arg(argv[3], "COOLDOWN_US"); if (cooldown_us < 0) return 2; }
    if (argc > 4) { sites_file  = argv[4]; }

    int noise_p = 0, noise_llc_mb = DEF_NOISE_LLC_MB, noise_seed_base = 0, batch = 1;
    if (argc > 5) { noise_p         = parse_nonneg_arg(argv[5], "NOISE_P");         if (noise_p < 0) return 2; }
    if (argc > 6) { noise_llc_mb    = parse_nonneg_arg(argv[6], "NOISE_LLC_MB");    if (noise_llc_mb < 0) return 2; }
    if (argc > 7) { noise_seed_base = parse_nonneg_arg(argv[7], "NOISE_SEED_BASE"); if (noise_seed_base < 0) return 2; }
    if (argc > 8) { batch           = parse_nonneg_arg(argv[8], "BATCH");           if (batch < 0) return 2; }

    // Validate EVERYTHING before launching Chrome: a bad tuple or site list must cost seconds,
    // not a multi-hour run that writes a mislabelled tree.
    if (!is_pow2_le64(noc)) {
        fprintf(stderr, "[web] ERROR: NoC must be a power of two in [1,64], got %d\n", noc); return 2;
    }
    if (tst < 1)        { fprintf(stderr, "[web] ERROR: TST must be >= 1 s, got %d\n", tst); return 2; }
    if (cycles < 1)     { fprintf(stderr, "[web] ERROR: cycles/address must be >= 1, got %d\n", cycles); return 2; }
    if (num_samples < 1){ fprintf(stderr, "[web] ERROR: SAMPLES must be >= 1, got %d\n", num_samples); return 2; }
    if (noise_p > NOISE_MAX_P) {
        fprintf(stderr, "[web] ERROR: NOISE_P must be in [0,%d], got %d\n", NOISE_MAX_P, noise_p); return 2;
    }
    if (noise_llc_mb < 1 || noise_llc_mb > NOISE_MAX_LLC_MB) {
        fprintf(stderr, "[web] ERROR: NOISE_LLC_MB must be in [1,%d], got %d\n", NOISE_MAX_LLC_MB, noise_llc_mb);
        return 2;
    }
    // The seed must fit noise.html's [0, 2^31-1] (keeps the JS LCG's first multiply exact).
    if ((long)noise_seed_base + batch > INT_MAX) {
        fprintf(stderr, "[web] ERROR: NOISE_SEED_BASE + BATCH overflows (%d + %d)\n", noise_seed_base, batch);
        return 2;
    }
    const long noise_seed = (long)noise_seed_base + batch;

    num_sites = web_site_list_load(sites_file, sites, WEB_MAX_SITES);
    if (num_sites < 0) return 2;

    signal(SIGINT,  web_cleanup);
    signal(SIGTERM, web_cleanup);

    // Config string drives /collect's path: data/website_{NoC}C_.../{slug}/{n}.csv.
    // Rebuilt from the PARSED values (not copied from argv) so the bare-NoC form and any
    // zero-padded label still produce the canonical dir name finalize_website.sh expects.
    // The "website_" tag keeps this tree disjoint from Stage 3's "realbrowser_" tree and from
    // the BARE {NoC}C_... dirs a manual single-shot browser run writes under the same root.
    // With noise, "website_p{p}flip_" keeps noisy traces out of the clean tree. The tag holds no
    // "C_", so mastikElite.c's parse_NoC_from_dirname and csv_to_h5.py's NOC_RE still find the
    // NoC field. run_website_sweep.sh composes the SAME tag as its STAGE_TAG.
    char tag[32];
    if (noise_p > 0) snprintf(tag, sizeof(tag), "website_p%dflip", noise_p);
    else             snprintf(tag, sizeof(tag), "website");
    char config[128];
    snprintf(config, sizeof(config), "%s_%dC_%dTST_%dK_%dcycles", tag, noc, tst, k, cycles);

    printf("[web] config : NoC=%d  TST=%ds  K=%d  cycles/addr=%d\n", noc, tst, k, cycles);
    if (noise_p > 0)
        printf("[web] noise  : FlipStress p=%d llc=%dMB seed=%ld (base %d + batch %d)\n",
               noise_p, noise_llc_mb, noise_seed, noise_seed_base, batch);
    else
        printf("[web] noise  : off\n");
    printf("[web] sites  : %d from %s\n", num_sites, sites_file);
    printf("[web] plan   : %d sites x %d samples = %d samples, %d us (%.3f s) cooldown each\n",
           num_sites, num_samples, num_sites * num_samples, cooldown_us, cooldown_us / 1e6);
    printf("[web] est.   : ~%.1f h wall time\n",
           (double)num_sites * num_samples * (tst + 0.5 + cooldown_us / 1e6 + 1.0) / 3600.0);
    printf("[web] output : data/%s/<site>/<n>.csv\n", config);

    // Clear any stale coordinator state (e.g. a "ready" left by a previous, now-dead browser)
    // BEFORE launching Chrome, so wait_ready() only passes on the NEW browser.
    fp_http_post(FP_SERVER_PORT, "/fp/reset", "{}", NULL, 0);
    fp_http_post(FP_SERVER_PORT, "/noise/reset", "{}", NULL, 0);

    char sampler_url[256];
    snprintf(sampler_url, sizeof(sampler_url),
             "http://localhost:%d/?mode=fingerprint&label=web_%dC_%dTST_%dK_%dcycles",
             FP_SERVER_PORT, noc, tst, k, cycles);

    if (noise_p > 0) {
        // Noise tab FIRST: the stressors are flipping before the sampler exists, so even the
        // first sample of the batch sees full-strength noise.
        char noise_url[256];
        snprintf(noise_url, sizeof(noise_url),
                 "http://localhost:%d/flipstress/noise.html?p=%d&llc_mb=%d&seed=%ld",
                 FP_SERVER_PORT, noise_p, noise_llc_mb, noise_seed);
        chrome_pid = launch_chrome(noise_url);
        printf("[web] launched chrome (pid %d, sandboxed, unpinned) on the noise tab; "
               "waiting for %d worker(s) to initialize...\n", (int)chrome_pid, noise_p);
        if (web_wait_cdp(CDP_TIMEOUT_S) != 0) {
            fprintf(stderr, "[web] FATAL: DevTools endpoint never came up on :%d\n", FP_CDP_PORT);
            web_cleanup(0);
        }
        if (wait_noise_ready() != 0) web_cleanup(0);

        // Sampler as the SECOND tab. Its target id is not needed: it lives until teardown.
        char sampler_id[128];
        if (web_victim_open(sampler_url, sampler_id, sizeof(sampler_id)) != 0) {
            fprintf(stderr, "[web] FATAL: could not open the sampler tab\n");
            web_cleanup(0);
        }
        printf("[web] sampler tab opened; waiting for mapping build...\n");
    } else {
        chrome_pid = launch_chrome(sampler_url);
        printf("[web] launched chrome (pid %d, sandboxed, unpinned); waiting for mapping build...\n",
               (int)chrome_pid);
        if (web_wait_cdp(CDP_TIMEOUT_S) != 0) {
            fprintf(stderr, "[web] FATAL: DevTools endpoint never came up on :%d\n", FP_CDP_PORT);
            web_cleanup(0);
        }
    }
    if (wait_ready() != 0) {
        fprintf(stderr, "[web] FATAL: browser never became ready (is the server running on :%d, "
                        "and Chrome reachable on DISPLAY :0?)\n", FP_SERVER_PORT);
        web_cleanup(0);
    }
    printf("[web] browser ready (CDP + lazy mapping). starting round-robin collection.\n");

    // Round-robin: outer loop = iteration, inner = site, so every site's samples are spread
    // across the whole run. With LIVE page loads this is what stops the classifier learning
    // time-of-day and network drift instead of the site.
    int collected = 0, failed = 0, align_reported = 0;
    for (int iter = 0; iter < num_samples; iter++) {
        for (int s = 0; s < num_sites; s++) {
            const char *slug = sites[s].slug;
            const char *url  = sites[s].url;
            printf("[web] iter %d/%d  site %-20s %s\n", iter + 1, num_samples, slug, url);
            fflush(stdout);

            // 0. Liveness check OUTSIDE the trace window (see header).
            if (!web_cdp_alive()) {
                // Report HOW the browser died, not just that it did. Without this the log only
                // ever says "DevTools is gone", which cannot distinguish a normal exit from a
                // kill (OOM/watchdog) -- the difference that decides what to fix.
                web_diagnose_chrome_death((pid_t)chrome_pid);
                fprintf(stderr, "[web] FATAL: Chrome/DevTools is gone; aborting so the run does "
                                "not fill with victim-less traces. CSVs so far are intact.\n");
                web_cleanup(0);
            }
            // 0b. Noise gate, also OUTSIDE the trace window: never start a noisy-tree trace
            //     without live noise.
            if (noise_p > 0 && noise_check() != 0) {
                fprintf(stderr, "[web] FATAL: FlipStress noise is not running; aborting so "
                                "data/%s/ does not fill with un-noised traces. CSVs so far are "
                                "intact.\n", config);
                web_cleanup(0);
            }

            // 1. Request the sample while the attacker tab is still foreground.
            double t_req = now_ms();
            long seq = request_sample(slug, config);
            if (seq < 0) {
                fprintf(stderr, "[web] WARNING: sample request failed (iter %d, %s)\n", iter, slug);
                failed++;
                continue;
            }

            // 2. Wait until the sampler is INSIDE its synchronous loop -- past this point it
            //    cannot be throttled by being backgrounded.
            if (wait_started(seq) != 0) {
                fprintf(stderr, "[web] WARNING: browser never started seq %ld (iter %d, %s)\n",
                        seq, iter, slug);
                failed++;
                continue;
            }

            // 3. Navigate. This is t = 0 of the trace.
            double t_started = now_ms();
            char id[128];
            if (web_victim_open(url, id, sizeof(id)) != 0) {
                fprintf(stderr, "[web] FATAL: could not open victim tab for %s (%s).\n"
                                "       The in-flight trace has no victim -- DELETE the newest CSV "
                                "in data/%s/%s/ before reusing this tree.\n",
                        slug, url, config, slug);
                web_cleanup(0);
            }

            // Report the alignment once per run, and thereafter only when it degrades: a
            // 30 h run must not need 35,000 timing lines to prove it stayed aligned, but it
            // must not hide drift either.
            double align_ms = now_ms() - t_req;
            if (align_ms > ALIGN_WARN_MS) {
                fprintf(stderr, "[web] WARNING: navigation alignment %.1f ms after sample request "
                        "(%.1f ms waiting for the sampler, %.1f ms to open the tab) -- that is "
                        "dead time at the head of the trace\n",
                        align_ms, t_started - t_req, now_ms() - t_started);
                align_reported = 1;
            } else if (!align_reported) {
                printf("[web] navigation alignment: %.1f ms after sample request "
                       "(%.1f ms waiting for the sampler, %.1f ms to open the tab)\n",
                       align_ms, t_started - t_req, now_ms() - t_started);
                fflush(stdout);
                align_reported = 1;
            }

            // 4. Let the trace run, then block on the browser's ack.
            sleep((unsigned int)tst);
            usleep(500000);   // margin for /collect + /fp/done latency
            if (wait_done(seq) != 0) {
                fprintf(stderr, "[web] WARNING: browser never acked seq %ld (iter %d, %s)\n",
                        seq, iter, slug);
                failed++;
            } else {
                collected++;
            }

            // 4b. Post-trace noise gate: the heartbeat is <= NOISE_STALE_MS old, so the noise
            //     was alive up to the end of the trace that was just written.
            if (noise_p > 0 && noise_check() != 0) {
                fprintf(stderr, "[web] FATAL: FlipStress noise stopped during this trace -- "
                                "DELETE the newest CSV in data/%s/%s/ before reusing this tree.\n",
                        config, slug);
                web_victim_close(id);
                web_cleanup(0);
            }

            // 5. Close the victim tab: kills its renderer, and with it that page's memory cache.
            web_victim_close(id);

            // 6. Cooldown so the L3 returns to baseline before the next trace.
            sleep_us(cooldown_us);
        }
    }

    // Signal the browser to exit its loop and tear down Chrome.
    fp_http_post(FP_SERVER_PORT, "/fp/cmd", "{\"cmd\":\"stop\"}", NULL, 0);
    if (chrome_pid > 0) kill((pid_t)chrome_pid, SIGTERM);
    sleep(1);
    web_pkill_armed();

    printf("[web] collection complete: %d samples, %d failed.\n", collected, failed);
    // Nonzero exit makes run_website_sweep.sh count this NoC as failed and skip the finalize,
    // so a partially-collected tree is never packed into an .h5 and deleted.
    return failed == 0 ? 0 : 1;
}
