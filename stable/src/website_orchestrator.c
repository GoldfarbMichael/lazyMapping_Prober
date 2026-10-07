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
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>      // timeBeginPeriod (link -lwinmm)
#else
#include <sys/wait.h>
#endif

#include "label_parse.h"   // parse_*_from_dirname (no Mastik: this file does no cache probing)
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
//
// $CHROME_PROFILE overrides the default. sweep_lib.sh exports the value it reaps with, so the
// sweep's backstop kill and this process can never disagree about which browser is ours.
#define CHROME_PROFILE_DEFAULT "/tmp/chrome-website"

static const char *chrome_profile(void) {
    static char buf[512];
    const char *env = getenv("CHROME_PROFILE");
    if (env && *env) return env;
#ifdef _WIN32
    // chrome.exe is a native process: it needs a Windows path, not an MSYS /tmp.
    const char *tmp = getenv("TEMP");
    snprintf(buf, sizeof(buf), "%s\\chrome-website", (tmp && *tmp) ? tmp : ".");
    return buf;
#else
    (void)buf;
    return CHROME_PROFILE_DEFAULT;
#endif
}

// Warn if the gap between "sample requested" and "victim navigating" exceeds this. That gap
// is dead time at the HEAD of the memorygram -- the window where a page load is most
// distinctive -- so silent drift here would quietly degrade every trace in a long run.
#define ALIGN_WARN_MS   150

// Warn when Chrome's Efficiency-mode throttling stayed in effect longer than this before the QoS
// guard (web_victim.c) undid it. The guard polls every 1 ms right after navigation, so anything
// near this bound means the guard is falling behind.
#define QOS_WARN_MS     20

static Site  sites[WEB_MAX_SITES];   // Site, the caps, and the loader live in web_victim.h
static int   num_sites = 0;

// Set once launch_chrome() returns, so the signal handler can tear the browser down.
static volatile sig_atomic_t chrome_pid = 0;

// Monotonic milliseconds; used only to measure navigation alignment.
static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
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
// Chrome
// ---------------------------------------------------------------------------

// The Stage 4 JS arm runs UNPRIVILEGED and UNPINNED, so it takes web_victim's defaults for
// both: no privilege drop (there is nothing to drop from) and no affinity mask (this process
// was never pinned, so Chrome inherits an all-cores mask already).
static pid_t launch_chrome(int noc, int tst, int k, int cycles) {
    char url[256];
#ifdef _WIN32
    // 127.0.0.1, not localhost: Windows tries ::1 first, the server is IPv4-only, and each
    // refused attempt costs ~200 ms. main.js talks to whatever origin it was loaded from,
    // so this one choice makes every browser->server fetch take the fast path.
    const char *host = "127.0.0.1";
#else
    const char *host = "localhost";
#endif
    snprintf(url, sizeof(url),
             "http://%s:%d/?mode=fingerprint&label=web_%dC_%dTST_%dK_%dcycles",
             host, FP_SERVER_PORT, noc, tst, k, cycles);
    WebVictimCfg cfg = {
        .profile_dir  = chrome_profile(),
        .initial_url  = url,
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

#ifdef _WIN32
// Ctrl+C / Ctrl+Break / console close. Windows runs this on its own thread, so the teardown
// (TerminateJobObject + a bounded wait) is safe here, unlike in a POSIX signal handler.
static BOOL WINAPI web_console_handler(DWORD type) {
    (void)type;
    web_pkill_armed();
    ExitProcess(130);
    return TRUE;
}
#endif

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

int main(int argc, char **argv) {
    // Unbuffered: run_website_sweep.sh pipes this through tee, and a native Windows exe writing
    // to a pipe is fully buffered -- progress would only appear in large bursts.
    setvbuf(stdout, NULL, _IONBF, 0);
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

    num_sites = web_site_list_load(sites_file, sites, WEB_MAX_SITES);
    if (num_sites < 0) return 2;

#ifdef _WIN32
    // The Windows twin of the Linux DISPLAY=:0 requirement. A process started over SSH lives in
    // session 0, which has no desktop: Chrome still runs there, but on an invisible window station
    // with no real GPU, so the victim renders in software and every trace measures the wrong
    // thing. Refuse that outright (WEB_ALLOW_SESSION0=1 overrides, for plumbing tests only), and
    // warn when our session is not the one on the monitor (switched user / RDP / locked console).
    {
        DWORD sess = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sess);
        const char *allow = getenv("WEB_ALLOW_SESSION0");
        if (sess == 0 && !(allow && strcmp(allow, "1") == 0)) {
            fprintf(stderr, "[web] ERROR: running in Windows session 0 (e.g. from SSH): Chrome would "
                            "have no desktop and no GPU, so the data would not be valid.\n"
                            "       Start the sweep with run_website_sweep_desktop.ps1 instead "
                            "(or set WEB_ALLOW_SESSION0=1 for a plumbing-only test).\n");
            return 3;
        }
        DWORD console = WTSGetActiveConsoleSessionId();
        if (sess != 0 && sess != console)
            fprintf(stderr, "[web] WARNING: this is session %lu but the monitor shows session %lu -- "
                            "Chrome is not on the visible desktop\n", sess, console);
    }
#endif

    signal(SIGINT,  web_cleanup);
    signal(SIGTERM, web_cleanup);
#ifdef _WIN32
    SetConsoleCtrlHandler(web_console_handler, TRUE);
    // The default Windows timer tick is ~15.6 ms, so wait_started()'s usleep(1000) poll would
    // really sleep ~15 ms and add that as dead time at the head of every trace. 1 ms restores
    // the poll cadence the alignment budget (ALIGN_WARN_MS) assumes. Released at process exit.
    timeBeginPeriod(1);
    // Windows 11 ignores timer-resolution requests from processes with no visible window --
    // which is exactly how the desktop launcher runs this (hidden console) -- so the request
    // above would silently fall back to ~15.6 ms ticks, slowing both wait_started() and the QoS
    // guard's 1 ms burst polling. Opting out of that throttle (and of EcoQoS) restores 1 ms.
    {
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof(pt));
        pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                         PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask   = 0;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)))
            fprintf(stderr, "[web] WARNING: could not opt out of timer throttling (error %lu); "
                            "1 ms polls may run at ~15 ms\n", GetLastError());
    }
#endif

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
    printf("[web] profile: %s\n", chrome_profile());

    // Clear any stale coordinator state (e.g. a "ready" left by a previous, now-dead browser)
    // BEFORE launching Chrome, so wait_ready() only passes on the NEW browser.
    fp_http_post(FP_SERVER_PORT, "/fp/reset", "{}", NULL, 0);

    pid_t launched = launch_chrome(noc, tst, k, cycles);
    if (launched < 0) {
        fprintf(stderr, "[web] FATAL: could not launch Chrome (see the error above)\n");
        return 1;
    }
    chrome_pid = launched;
    printf("[web] launched chrome (pid %d, sandboxed, unpinned); waiting for mapping build...\n",
           (int)chrome_pid);

    if (web_wait_cdp(CDP_TIMEOUT_S) != 0) {
        fprintf(stderr, "[web] FATAL: DevTools endpoint never came up on :%d\n", FP_CDP_PORT);
        web_cleanup(0);
    }
    if (wait_ready() != 0) {
        fprintf(stderr, "[web] FATAL: browser never became ready (is the server running on :%d, "
                        "and Chrome reachable on DISPLAY :0?)\n", FP_SERVER_PORT);
        web_cleanup(0);
    }
    printf("[web] browser ready (CDP + lazy mapping). starting round-robin collection.\n");

    // Windows: keep Chrome from putting the background sampler in Efficiency mode (see
    // web_victim.h). No-op elsewhere.
    web_qos_guard_start();
#ifdef _WIN32
    printf("[web] QoS guard: priority class %s; EcoQoS + memory priority restored by the watcher\n",
           web_qos_priority_locked() ? "pinned at NORMAL by the job"
                                     : "restored by the watcher (job priority limit refused)");
#endif
    long   qos_total_fixes = 0;
    int    qos_samples_hit = 0, qos_reported = 0;
    double qos_worst_ms = 0.0, qos_worst_burst_ms = 0.0, qos_worst_pass_ms = 0.0, qos_worst_gap_ms = 0.0;
    (void)qos_reported;   // only read in the Windows report branch

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

            // 3. Navigate. This is t = 0 of the trace. The QoS guard enters burst polling first:
            //    backgrounding the sampler is exactly what makes Chrome throttle it.
            double t_started = now_ms();
            WebQosStats qs;
            web_qos_guard_take(&qs);   // discard anything from before this trace
            web_qos_guard_navigated();
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

            // QoS report for this trace: once in full, then only when the throttled window grows
            // past QOS_WARN_MS -- the same reporting policy as the navigation alignment above.
            web_qos_guard_take(&qs);
            if (qs.fixes > 0) {
                qos_total_fixes += qs.fixes;
                qos_samples_hit++;
                if (qs.max_exposure_ms > qos_worst_ms) qos_worst_ms = qs.max_exposure_ms;
            }
            if (qs.max_exposure_burst_ms > qos_worst_burst_ms) qos_worst_burst_ms = qs.max_exposure_burst_ms;
            if (qs.max_pass_ms > qos_worst_pass_ms)    qos_worst_pass_ms = qs.max_pass_ms;
            if (qs.max_burst_gap_ms > qos_worst_gap_ms) qos_worst_gap_ms  = qs.max_burst_gap_ms;
#ifdef _WIN32
            if (qs.max_exposure_ms > QOS_WARN_MS) {
                fprintf(stderr, "[web] WARNING: Chrome throttled a process for up to %.1f ms before "
                        "the QoS guard undid it (<= %.1f ms during the post-navigation burst; first "
                        "correction %.1f ms after navigation; longest pass %.1f ms, longest burst "
                        "gap %.1f ms)\n", qs.max_exposure_ms, qs.max_exposure_burst_ms,
                        qs.first_fix_ms, qs.max_pass_ms, qs.max_burst_gap_ms);
            } else if (!qos_reported) {
                printf("[web] QoS guard, first trace: %ld correction(s), first %.1f ms after "
                       "navigation, throttled window <= %.1f ms (<= %.1f ms during the "
                       "post-navigation burst; longest pass %.1f ms, longest burst gap %.1f ms)\n",
                       qs.fixes, qs.first_fix_ms, qs.max_exposure_ms, qs.max_exposure_burst_ms,
                       qs.max_pass_ms, qs.max_burst_gap_ms);
                qos_reported = 1;
            }
#endif

            // 5. Close the victim tab: kills its renderer, and with it that page's memory cache.
            web_victim_close(id);

            // 6. Cooldown so the L3 returns to baseline before the next trace.
            sleep_us(cooldown_us);
        }
    }

    web_qos_guard_stop();
#ifdef _WIN32
    printf("[web] QoS guard summary: %ld correction(s) in %d of %d traces, worst throttled "
           "window <= %.1f ms (<= %.1f ms during post-navigation bursts; longest pass %.1f ms, "
           "longest burst gap %.1f ms)\n",
           qos_total_fixes, qos_samples_hit, collected + failed, qos_worst_ms, qos_worst_burst_ms,
           qos_worst_pass_ms, qos_worst_gap_ms);
#endif

    // Signal the browser to exit its loop and tear down Chrome.
    fp_http_post(FP_SERVER_PORT, "/fp/cmd", "{\"cmd\":\"stop\"}", NULL, 0);
#ifndef _WIN32
    if (chrome_pid > 0) kill((pid_t)chrome_pid, SIGTERM);
#endif
    sleep(1);
    web_pkill_armed();   // Windows: terminates Chrome's job (the whole browser tree)

    printf("[web] collection complete: %d samples, %d failed.\n", collected, failed);
    // Nonzero exit makes run_website_sweep.sh count this NoC as failed and skip the finalize,
    // so a partially-collected tree is never packed into an .h5 and deleted.
    return failed == 0 ? 0 : 1;
}
