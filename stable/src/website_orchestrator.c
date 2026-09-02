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

#define MAX_SITES     512
#define MAX_SLUG_LEN   64
#define MAX_URL_LEN   512

typedef struct {
    char slug[MAX_SLUG_LEN];   // class label -> data/{config}/{slug}/{n}.csv
    char url[MAX_URL_LEN];     // what the victim tab navigates to
} Site;

static Site  sites[MAX_SITES];
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
        "  <config>      config label, e.g. 16C_6TST_180K_2288cycles -- NoC/TST/K/cycles come from it\n"
        "  <NoC>         bare power of two in [1,64]; TST/K/cycles fall back to the defaults\n"
        "                (%dTST, %dK, %dcycles)\n"
        "  [SAMPLES]     samples per site                (default %d)\n"
        "  [COOLDOWN_US] cooldown between samples, MICROSECONDS (default %d = %.2f s)\n"
        "  [SITES_FILE]  '<slug>\\t<url>' per line, '#' comments (default %s)\n"
        "\n"
        "Runs as the LOGIN USER -- no sudo. Chrome keeps its sandbox.\n",
        prog, DEF_WS_TST, DEF_WS_K, DEF_WS_CYCLES, DEF_NUM_SAMPLES,
        DEF_COOLDOWN_US, DEF_COOLDOWN_US / 1e6, DEF_SITES_FILE);
}

// ---------------------------------------------------------------------------
// Site list
// ---------------------------------------------------------------------------

// A slug becomes a DIRECTORY NAME at server.py's /collect and a label_map key in the .h5,
// so restrict it to what is safe on both sides. A '/' would make os.path.join() build a
// nested path that csv_to_h5.py's one-level class scan cannot see; a leading '/' would make
// it discard the data root entirely.
static int slug_ok(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    return 1;
}

// Load '<slug>\t<url>' lines, skipping blanks and '#' comments. Returns the count, or -1.
// Every rejection is fatal and reported with a line number: a malformed list must cost
// seconds here, not a multi-hour run that writes a mislabelled tree.
static int site_list_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[web] ERROR: cannot open sites file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[1024];
    int lineno = 0, n = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        line[strcspn(line, "\r\n")] = '\0';
        char *slug = p;
        char *sep = slug + strcspn(slug, " \t");
        if (*sep == '\0') {
            fprintf(stderr, "[web] ERROR: %s:%d: expected '<slug><TAB><url>', got '%s'\n",
                    path, lineno, slug);
            fclose(f); return -1;
        }
        *sep++ = '\0';
        while (*sep == ' ' || *sep == '\t') sep++;
        if (*sep == '\0') {
            fprintf(stderr, "[web] ERROR: %s:%d: missing URL for slug '%s'\n", path, lineno, slug);
            fclose(f); return -1;
        }

        if (n >= MAX_SITES) {
            fprintf(stderr, "[web] ERROR: %s:%d: more than %d sites\n", path, lineno, MAX_SITES);
            fclose(f); return -1;
        }
        if (!slug_ok(slug)) {
            fprintf(stderr, "[web] ERROR: %s:%d: slug '%s' must match [A-Za-z0-9._-]+ "
                            "(it becomes a directory name and an .h5 label)\n", path, lineno, slug);
            fclose(f); return -1;
        }
        if (strlen(slug) >= MAX_SLUG_LEN || strlen(sep) >= MAX_URL_LEN) {
            fprintf(stderr, "[web] ERROR: %s:%d: slug or URL too long\n", path, lineno);
            fclose(f); return -1;
        }
        // Duplicate slugs would silently merge two sites into one class dir -- the samples
        // would interleave and the class would be unlabelable after the fact.
        for (int i = 0; i < n; i++) {
            if (strcmp(sites[i].slug, slug) == 0) {
                fprintf(stderr, "[web] ERROR: %s:%d: duplicate slug '%s' (also line for '%s')\n",
                        path, lineno, slug, sites[i].url);
                fclose(f); return -1;
            }
        }
        snprintf(sites[n].slug, sizeof(sites[n].slug), "%s", slug);
        snprintf(sites[n].url,  sizeof(sites[n].url),  "%s", sep);
        n++;
    }
    fclose(f);
    if (n < 2) {
        fprintf(stderr, "[web] ERROR: %s has %d uncommented site(s); need at least 2 to "
                        "fingerprint\n", path, n);
        return -1;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

static pid_t launch_chrome(int noc, int tst, int k, int cycles) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://localhost:%d/?mode=fingerprint&label=web_%dC_%dTST_%dK_%dcycles",
             FP_SERVER_PORT, noc, tst, k, cycles);
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "--remote-debugging-port=%d", FP_CDP_PORT);

    pid_t pid = fork();
    if (pid == 0) {
        // No pin_to_core: Chrome is deliberately free to spread across all cores (see header).
        // Respect an operator-supplied value, but only a NON-EMPTY one: setenv(...,0) alone
        // would honour a set-but-empty DISPLAY (common in non-login shells and cron) and hand
        // Chrome an unusable display.
        const char *disp = getenv("DISPLAY");
        if (!disp || !*disp) setenv("DISPLAY", ":0", 1);
        // The :0 X server's auth cookie lives in gdm's Xauthority, not ~/.Xauthority (which
        // is the :1 Xtigervnc cookie). It is owned by uid 1000, so running as the login user
        // needs no `xhost +SI:localuser:root` -- unlike Stage 3, which ran Chrome as root.
        const char *xa = getenv("XAUTHORITY");
        if (!xa || !*xa) setenv("XAUTHORITY", "/run/user/1000/gdm/Xauthority", 1);
        execlp("google-chrome", "google-chrome",
               // NOTE: no --no-sandbox. We are not root, so the zygote sandbox works, and it
               // is worth keeping while navigating to arbitrary live sites.
               "--user-data-dir=" CHROME_PROFILE,
               "--no-first-run", "--no-default-browser-check",
               dbg,
               // Guarantee the victim page gets its own renderer process, as it would in a
               // real deployment.
               "--site-per-process",
               // The attacker tab is backgrounded the moment the victim tab opens. Without
               // these, Chrome throttles its timers and deprioritises its renderer, and the
               // /fp/poll loop stops picking up sample requests promptly.
               "--disable-background-timer-throttling",
               "--disable-backgrounding-occluded-windows",
               "--disable-renderer-backgrounding",
               "--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,"
                                  "SpareRendererForProcessPerSite",
               // Suppress the HTTP disk cache so sample N of a site is not a warm-cache load
               // that looks nothing like sample 0. DNS, TLS session resumption and connection
               // reuse still warm up -- a limitation to document, not one this can fix.
               "--disk-cache-size=1", "--media-cache-size=1",
               "--new-window", url, (char *)NULL);
        perror("execlp google-chrome");
        _exit(127);
    }
    return pid;
}

// Tear down OUR Chrome (the whole process tree, matched by its profile dir), then exit.
//
// The bracket around one character keeps the pattern from matching this pkill's own
// /bin/sh -c command line, which contains the pattern verbatim -- otherwise pkill races to
// kill the shell that is running it. (system() in a handler is not async-signal-safe, but
// this is the existing project pattern for a cleanup-then-exit handler.)
static void web_cleanup(int sig) {
    (void)sig;
    system("pkill -9 -f 'user-data-dir=/tmp/chrome-websit[e]'");
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
// Victim tab (Chrome DevTools HTTP endpoint, :9222)
// ---------------------------------------------------------------------------

// Block until the DevTools endpoint answers. Returns 0, or -1 on timeout.
static int wait_cdp(void) {
    char resp[1024];
    for (int s = 0; s < CDP_TIMEOUT_S; s++) {
        if (fp_http_get(FP_CDP_PORT, "/json/version", resp, sizeof(resp)) == 0 &&
            strstr(resp, "webSocketDebuggerUrl"))
            return 0;
        sleep(1);
    }
    return -1;
}

// Cheap liveness check, run OUTSIDE the trace window so a dead browser is caught before a
// sample is requested rather than after a CSV has already been written for it.
static int cdp_alive(void) {
    char resp[1024];
    return fp_http_get(FP_CDP_PORT, "/json/version", resp, sizeof(resp)) == 0 &&
           strstr(resp, "webSocketDebuggerUrl") != NULL;
}

// Open `url` in a new tab; copy the target id into `id`. Returns 0, or -1.
//
// The URL is the RAW QUERY STRING: `PUT /json/new?<url>`. Passing it as `?url=<url>` is
// accepted and silently opens about:blank instead -- verified against Chrome 150. The
// method must be PUT (it was GET before Chrome 111).
static int victim_open(const char *url, char *id, size_t id_len) {
    char enc[MAX_URL_LEN * 3 + 1], path[MAX_URL_LEN * 3 + 32], resp[4096];
    if (fp_url_encode(url, enc, sizeof(enc)) != 0) {
        fprintf(stderr, "[web] ERROR: URL too long to encode: %s\n", url);
        return -1;
    }
    snprintf(path, sizeof(path), "/json/new?%s", enc);
    if (fp_http_request(FP_CDP_PORT, "PUT", path, NULL, NULL, resp, sizeof(resp)) != 0)
        return -1;
    if (fp_json_str(resp, "id", id, id_len) != 0) {
        fprintf(stderr, "[web] ERROR: /json/new gave no target id (response: %.200s)\n", resp);
        return -1;
    }
    return 0;
}

// Close the victim tab. Best-effort: a failure here leaks one tab, which the next sample's
// fresh tab makes harmless, so it warns rather than aborting.
static void victim_close(const char *id) {
    char path[256], resp[512];
    snprintf(path, sizeof(path), "/json/close/%s", id);
    if (fp_http_get(FP_CDP_PORT, path, resp, sizeof(resp)) != 0)
        fprintf(stderr, "[web] WARNING: failed to close target %s\n", id);
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

    // Validate EVERYTHING before launching Chrome: a bad tuple or site list must cost seconds,
    // not a multi-hour run that writes a mislabelled tree.
    if (!is_pow2_le64(noc)) {
        fprintf(stderr, "[web] ERROR: NoC must be a power of two in [1,64], got %d\n", noc); return 2;
    }
    if (tst < 1)        { fprintf(stderr, "[web] ERROR: TST must be >= 1 s, got %d\n", tst); return 2; }
    if (cycles < 1)     { fprintf(stderr, "[web] ERROR: cycles/address must be >= 1, got %d\n", cycles); return 2; }
    if (num_samples < 1){ fprintf(stderr, "[web] ERROR: SAMPLES must be >= 1, got %d\n", num_samples); return 2; }

    num_sites = site_list_load(sites_file);
    if (num_sites < 0) return 2;

    signal(SIGINT,  web_cleanup);
    signal(SIGTERM, web_cleanup);

    // Config string drives /collect's path: data/website_{NoC}C_.../{slug}/{n}.csv.
    // Rebuilt from the PARSED values (not copied from argv) so the bare-NoC form and any
    // zero-padded label still produce the canonical dir name finalize_website.sh expects.
    // The "website_" tag keeps this tree disjoint from Stage 3's "realbrowser_" tree and from
    // the BARE {NoC}C_... dirs a manual single-shot browser run writes under the same root.
    char config[128];
    snprintf(config, sizeof(config), "website_%dC_%dTST_%dK_%dcycles", noc, tst, k, cycles);

    printf("[web] config : NoC=%d  TST=%ds  K=%d  cycles/addr=%d\n", noc, tst, k, cycles);
    printf("[web] sites  : %d from %s\n", num_sites, sites_file);
    printf("[web] plan   : %d sites x %d samples = %d samples, %d us (%.3f s) cooldown each\n",
           num_sites, num_samples, num_sites * num_samples, cooldown_us, cooldown_us / 1e6);
    printf("[web] est.   : ~%.1f h wall time\n",
           (double)num_sites * num_samples * (tst + 0.5 + cooldown_us / 1e6 + 1.0) / 3600.0);
    printf("[web] output : data/%s/<site>/<n>.csv\n", config);

    // Clear any stale coordinator state (e.g. a "ready" left by a previous, now-dead browser)
    // BEFORE launching Chrome, so wait_ready() only passes on the NEW browser.
    fp_http_post(FP_SERVER_PORT, "/fp/reset", "{}", NULL, 0);

    chrome_pid = launch_chrome(noc, tst, k, cycles);
    printf("[web] launched chrome (pid %d, sandboxed, unpinned); waiting for mapping build...\n",
           (int)chrome_pid);

    if (wait_cdp() != 0) {
        fprintf(stderr, "[web] FATAL: DevTools endpoint never came up on :%d\n", FP_CDP_PORT);
        web_cleanup(0);
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
            if (!cdp_alive()) {
                // Report HOW the browser died, not just that it did. Without this the log only
                // ever says "DevTools is gone", which cannot distinguish a normal exit from a
                // kill (OOM/watchdog) -- the difference that decides what to fix.
                int st;
                pid_t r = waitpid((pid_t)chrome_pid, &st, WNOHANG);
                if (r == (pid_t)chrome_pid) {
                    if (WIFSIGNALED(st))
                        fprintf(stderr, "[web] chrome pid %d was KILLED by signal %d (%s)\n",
                                (int)chrome_pid, WTERMSIG(st), strsignal(WTERMSIG(st)));
                    else if (WIFEXITED(st))
                        fprintf(stderr, "[web] chrome pid %d exited normally with status %d\n",
                                (int)chrome_pid, WEXITSTATUS(st));
                } else if (r == 0) {
                    fprintf(stderr, "[web] chrome pid %d is still alive but its DevTools port is "
                                    "unreachable (browser process wedged, not dead)\n",
                            (int)chrome_pid);
                } else {
                    fprintf(stderr, "[web] chrome pid %d already reaped (waitpid: %s)\n",
                            (int)chrome_pid, strerror(errno));
                }
                fprintf(stderr, "[web] FATAL: Chrome/DevTools is gone; aborting so the run does "
                                "not fill with victim-less traces. CSVs so far are intact.\n");
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
            if (victim_open(url, id, sizeof(id)) != 0) {
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

            // 5. Close the victim tab: kills its renderer, and with it that page's memory cache.
            victim_close(id);

            // 6. Cooldown so the L3 returns to baseline before the next trace.
            sleep_us(cooldown_us);
        }
    }

    // Signal the browser to exit its loop and tear down Chrome.
    fp_http_post(FP_SERVER_PORT, "/fp/cmd", "{\"cmd\":\"stop\"}", NULL, 0);
    if (chrome_pid > 0) kill((pid_t)chrome_pid, SIGTERM);
    sleep(1);
    system("pkill -9 -f 'user-data-dir=/tmp/chrome-websit[e]'");

    printf("[web] collection complete: %d samples, %d failed.\n", collected, failed);
    // Nonzero exit makes run_website_sweep.sh count this NoC as failed and skip the finalize,
    // so a partially-collected tree is never packed into an .h5 and deleted.
    return failed == 0 ? 0 : 1;
}
