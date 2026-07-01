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
//   4. block on /fp/state until done_seq == seq, then SIGKILL the stressor + 5s cooldown.
//   5. next stressor.
//
// Sampling is stressor-by-stressor (round-robin: outer loop = iteration, inner = stressor)
// so each stressor's NUM_SAMPLES samples are spread across the run -- this avoids
// fingerprinting a drifting machine state.
//
// NoC is argv[1] (a future bash script sweeps NoCs); one lazy mapping per invocation.
// -----------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mastikElite.h"   // stress_battery, NUM_STRESSORS, pin_to_core, cleanup_handler

#define NUM_SAMPLES    50          // iterations (samples) per stressor
#define BROWSER_CORE   0           // Chrome (the JS sampler)
#define STRESSOR_CORE  1           // stress-ng
#define ORCH_CORE      2           // this orchestrator (keep cores 0/1 clean)

// Sampling config baked into the JS URL label ({workload}_{NoC}C_{TST}TST_{K}K_{cycles}cycles).
// NoC comes from argv; the rest match the JS defaults / the main stress-ng eval config.
#define FP_TST         2           // total sampling time (s)
#define FP_K           0          // accesses between timer polls (JS side)
#define FP_CYCLES      4576        // est. CPU cycles per address (JS Q sizing)

#define COOLDOWN_S     5           // cooldown between samples (let the L3 return to baseline)
#define STEADY_US      50000       // grace for the stressor to reach its loop before sampling
#define SERVER_PORT    8080
#define READY_TIMEOUT_S 120        // max wait for Chrome to load + build the mapping
                                   // (cold Chrome-as-root startup alone can take ~20-30s)
#define CHROME_PROFILE "/tmp/chrome-fingerprint"

// True iff x is a power of two in [1, 64] (lazy-mapping regime).
static int is_pow2_le64(int x) {
    return x >= 1 && x <= 64 && (x & (x - 1)) == 0;
}

// ---- tiny raw-socket HTTP to the Flask coordinator (no libcurl) ----
static int ctl_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

// POST a JSON body to `path`; copy the response body into `resp` (may be NULL to ignore).
static int http_post(const char *path, const char *body, char *resp, int resp_len) {
    int fd = ctl_connect();
    if (fd < 0) { perror("connect POST"); return -1; }
    int blen = (int)strlen(body);
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n%s", path, SERVER_PORT, blen, body);
    if (write(fd, req, rlen) < 0) { perror("write POST"); close(fd); return -1; }
    int total = 0;
    char buf[1024];
    int r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        if (resp && total < resp_len - 1) {
            int n = (r < resp_len - 1 - total) ? r : (resp_len - 1 - total);
            memcpy(resp + total, buf, n);
            total += n;
        }
    }
    if (resp) resp[total] = '\0';
    close(fd);
    return 0;
}

// GET `path`; copy the response (headers + body) into `resp`.
static int http_get(const char *path, char *resp, int resp_len) {
    int fd = ctl_connect();
    if (fd < 0) { perror("connect GET"); return -1; }
    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
        path, SERVER_PORT);
    if (write(fd, req, rlen) < 0) { perror("write GET"); close(fd); return -1; }
    int total = 0, r;
    char buf[1024];
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        if (total < resp_len - 1) {
            int n = (r < resp_len - 1 - total) ? r : (resp_len - 1 - total);
            memcpy(resp + total, buf, n);
            total += n;
        }
    }
    resp[total] = '\0';
    close(fd);
    return 0;
}

// Minimal JSON scalar extraction (the responses are tiny, flat objects).
// json_int: value of "key":<int>, or `dflt` if absent.
static long json_int(const char *body, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p) return dflt;
    p += strlen(pat);
    return strtol(p, NULL, 10);
}
// json_bool: 1 if "key": true, else 0. Tolerates whitespace after the colon -- Flask's
// jsonify emits "key": true (with a space), so a literal "key":true match would fail.
static int json_bool(const char *body, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return strncmp(p, "true", 4) == 0;
}

static pid_t launch_chrome(int noc) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://localhost:%d/?mode=fingerprint&label=fp_%dC_%dTST_%dK_%dcycles",
             SERVER_PORT, noc, FP_TST, FP_K, FP_CYCLES);
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
        if (http_get("/fp/state", resp, sizeof(resp)) == 0 && json_bool(resp, "ready"))
            return 0;
        sleep(1);
    }
    return -1;
}

// POST a sample request for `workload`/`config`; return the assigned seq.
static long request_sample(const char *workload, const char *config) {
    char body[256], resp[512];
    snprintf(body, sizeof(body),
             "{\"cmd\":\"sample\",\"workload\":\"%s\",\"config\":\"%s\"}", workload, config);
    if (http_post("/fp/cmd", body, resp, sizeof(resp)) != 0) return -1;
    return json_int(resp, "seq", -1);
}

// Block until the browser acks it sampled + saved `seq`.
static void wait_done(long seq) {
    char resp[512];
    for (;;) {
        if (http_get("/fp/state", resp, sizeof(resp)) == 0 &&
            json_int(resp, "done_seq", -1) >= seq)
            return;
        usleep(20000); // 20 ms
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || !is_pow2_le64(atoi(argv[1]))) {
        fprintf(stderr, "usage: %s <NoC>   (NoC = power of two in [1,64])\n", argv[0]);
        return 2;
    }
    int noc = atoi(argv[1]);

    signal(SIGINT, fp_cleanup);   // self-tear-down Chrome + stress-ng on Ctrl+C / TERM
    signal(SIGTERM, fp_cleanup);

    // Keep cores 0/1 reserved for Chrome / stress-ng; the orchestrator just coordinates.
    pin_to_core(ORCH_CORE);

    // Config string drives /collect's path: data/realbrowser_{NoC}C_.../{stressor}/{n}.csv
    char config[64];
    snprintf(config, sizeof(config), "realbrowser_%dC_%dTST_%dK_%dcycles",
             noc, FP_TST, FP_K, FP_CYCLES);

    size_t num_stressors = stress_battery_count();
    printf("[fp] NoC=%d, %zu stressors, %d samples each -> %s\n",
           noc, num_stressors, NUM_SAMPLES, config);

    // Clear any stale coordinator state (e.g. a "ready" left by a previous, now-dead
    // browser) BEFORE launching Chrome, so wait_ready() only passes on the NEW browser.
    http_post("/fp/reset", "{}", NULL, 0);

    pid_t chrome = launch_chrome(noc);
    printf("[fp] launched chrome (pid %d) on core %d; waiting for mapping build...\n",
           chrome, BROWSER_CORE);
    if (wait_ready() != 0) {
        fprintf(stderr, "[fp] FATAL: browser never became ready (is the server running "
                        "on :%d, and Chrome reachable on DISPLAY :0?)\n", SERVER_PORT);
        if (chrome > 0) kill(chrome, SIGTERM);
        return 1;
    }
    printf("[fp] browser ready. starting round-robin collection.\n");

    // Round-robin: spread each stressor's samples across the whole run (state-drift safe).
    for (int iter = 0; iter < NUM_SAMPLES; iter++) {
        for (size_t s = 0; s < num_stressors; s++) {
            const char *name = stress_battery[s].stressor_name;
            printf("[fp] iter %d/%d  stressor %s\n", iter + 1, NUM_SAMPLES, name);

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
                sleep(FP_TST);
                usleep(500000);   // +0.5s margin for poll pickup + /collect + /fp/done latency
                wait_done(seq);
            }

            // 4. Terminate the stressor and reap it.
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);

            // 5. Cooldown so the L3 returns to baseline before the next sample.
            sleep(COOLDOWN_S);
        }
    }

    // Signal the browser to exit its loop and tear down Chrome.
    http_post("/fp/cmd", "{\"cmd\":\"stop\"}", NULL, 0);
    if (chrome > 0) kill(chrome, SIGTERM);
    printf("[fp] collection complete.\n");
    return 0;
}
