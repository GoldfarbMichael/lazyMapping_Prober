// website_prober.c -- see website_prober.h for what this arm is and why it is a separate file.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mastik/l3.h>

#include "mastikElite.h"
#include "utils.h"
#include "web_victim.h"
#include "website_prober.h"

// Distinct from the JS arm's /tmp/chrome-website ON PURPOSE. Chrome reuses a running instance
// when --user-data-dir matches, so a shared profile would open this arm's browser as a TAB in
// the other run's window, and either arm's teardown pkill would tear down both.
#define WEB_PROBER_PROFILE "/tmp/chrome-website-native"

#define WEB_CDP_TIMEOUT_S   60        // max wait for the DevTools endpoint to come up
#define WEB_DEF_COOLDOWN_US 1000000   // between samples; matches runStressNG_batches' 1 s
#define WEB_INTER_ROUND_S   8         // between round-robin passes; matches the stress arm
#define WEB_ALIGN_WARN_MS   150       // a CDP round trip slower than this eats the load transient
// Calibration target: a page with no network fetch and no content, so the measured round trip
// is browser+loopback overhead only -- the part every real site also pays before its own work
// begins.
#define WEB_CALIBRATION_URL "about:blank"

// Chrome, so the signal handler can tear the browser down.
static volatile sig_atomic_t g_chrome_pid = 0;

// Read a uid/gid from the environment. Succeeds only for a well-formed, NONZERO id: 0 is root,
// which is not something you can drop *to*.
static int env_nonzero_id(const char *name, unsigned long *out) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    errno = 0;
    char *end;
    unsigned long x = strtoul(v, &end, 10);
    if (errno != 0 || *end != '\0' || x == 0 || x > 0x7fffffffUL) return 0;
    *out = x;
    return 1;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// usleep() is specified only below 1e6 (POSIX allows EINVAL at or above that), and the
// cooldown knob is in microseconds, so a >= 1 s value must not become a silent no-op.
static void sleep_us(long usec) {
    if (usec <= 0) return;
    if (usec >= 1000000) sleep((unsigned int)(usec / 1000000));
    usleep((useconds_t)(usec % 1000000));
}

// Replaces cleanup_handler() (main.c) for modes 10/11: that one only pkills stress-ng and
// would orphan a whole Chrome tree on Ctrl-C.
static void web_prober_cleanup(int sig) {
    (void)sig;
    // _exit() does NOT flush stdio, and stdout is block-buffered when the runner redirects it
    // to a log file. Without this, everything still in the buffer is discarded and the log
    // truncates mid-line -- which makes any failure here look like a crash somewhere earlier.
    fflush(NULL);
    web_pkill_armed();
    _exit(130);
}

// ---------------------------------------------------------------------------

int runWebsite_batches(double tst_sec, int batch_size, int start_iteration,
                       char *output_dir, const char *backing_file, const char *BIN_file,
                       int timer_mode, const char *sites_file) {
    l3pp_t l3 = NULL;
    void **e_sets = NULL;

    // ---- Mastik setup: identical sizing to runStressNG_batches, minus the jsmap branch ----
    load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, backing_file, BIN_file);

    int NoC = parse_NoC_from_dirname(output_dir);
    if (NoC <= 0) {
        fprintf(stderr, "[web] FATAL: no {NoC}C_ field in config label '%s'\n", output_dir);
        return 1;
    }
    // Clusters_t.counts[] is a fixed MAX_NUM_CLUSTERS array while clusterHeads is malloc'd to
    // NoC, so a larger NoC would write past it. The Lazy Mapping regime is <= 64 anyway.
    if (NoC > MAX_NUM_CLUSTERS) {
        fprintf(stderr, "[web] FATAL: NoC %d exceeds MAX_NUM_CLUSTERS %d\n", NoC, MAX_NUM_CLUSTERS);
        return 1;
    }
    int setsPerCluster = l3_getSets(l3) / NoC;

    int cpa = parse_cycles_from_dirname(output_dir);
    if (cpa <= 0) {
        cpa = (timer_mode == 10) ? 300 : 2288;
        printf("[WARN] No {N}cycles field in '%s'; falling back to %d cycles/address\n",
               output_dir, cpa);
    }
    // K = accesses between clock polls, from the label's "{K}K" field. 0 selects the dynamic-K
    // sweep (same contract as the jsmap samplers and JS main.js); a missing/malformed field
    // falls back to the historical 90. Mode 10 polls the native clock, mode 11 the mock clock.
    int K = parse_K_from_dirname(output_dir);
    if (K < 0) {
        K = 90;
        printf("[WARN] No {N}K field in '%s'; falling back to K=%d\n", output_dir, K);
    }

    int tst_label = parse_TST_from_dirname(output_dir);
    if (tst_label > 0) tst_sec = (double)tst_label;
    else printf("[WARN] No {N}TST field in '%s'; falling back to %.1f s\n", output_dir, tst_sec);

    uint64_t TST_cycles = g_tsc_freq_hz * tst_sec;
    uint64_t SST_cycles = (uint64_t)cpa * setsPerCluster * l3_getAssociativity(l3);

    // 500us floor (JS MIN_QUANTUM_MS): mock clock only (mode 11). Its 100us clamp makes a
    // sub-500us quantum meaningless; the native clock (mode 10) has cycle resolution.
    if (timer_mode == 11) {
        uint64_t min_SST_cycles_for_500us = (500 * g_tsc_freq_hz) / 1000000;
        if (SST_cycles < min_SST_cycles_for_500us) {
            printf("[TIMER_MODE=%d] SST_cycles adjusted: %lu -> %lu (minimum 500us window)\n",
                   timer_mode, SST_cycles, min_SST_cycles_for_500us);
            SST_cycles = min_SST_cycles_for_500us;
        }
    }

    uint64_t totalSweeps_forCluster = TST_cycles / (NoC * SST_cycles);
    if (totalSweeps_forCluster == 0) {
        fprintf(stderr, "[web] FATAL: TST/(NoC*SST) rounds to 0 rows -- raise TST or lower cycles\n");
        return 1;
    }

    // ---- Site list: validate BEFORE launching anything ----
    static Site sites[WEB_MAX_SITES];
    int num_sites = web_site_list_load(sites_file, sites, WEB_MAX_SITES);
    if (num_sites < 0) return 1;

    const char *clock_subdir = timer_mode_subdir(timer_mode);

    long cooldown_us = WEB_DEF_COOLDOWN_US;
    const char *cd = getenv("WEB_COOLDOWN_US");
    if (cd && *cd) {
        long v = strtol(cd, NULL, 10);
        if (v >= 0) cooldown_us = v;
        else fprintf(stderr, "[web] WARNING: ignoring WEB_COOLDOWN_US='%s'\n", cd);
    }

    printf("[web] config : NoC=%d  TST=%.1fs  K=%d%s  cycles/addr=%d  SST_cycles=%lu  rows=%lu\n",
           NoC, tst_sec, K, (K == 0) ? " (dynamic)" : " (fixed)", cpa, SST_cycles,
           totalSweeps_forCluster);
    printf("[web] sites  : %d from %s\n", num_sites, sites_file);
    printf("[web] plan   : %d sites x %d samples = %d traces, %ld us cooldown each\n",
           num_sites, batch_size, num_sites * batch_size, cooldown_us);
    printf("[web] output : data/%s/%s/<site>/<n>.csv\n", clock_subdir, output_dir);
    printf("[web] est.   : ~%.1f h wall time\n",
           (double)num_sites * batch_size * (tst_sec + cooldown_us / 1e6 + 0.5) / 3600.0);

    // ---- Prober pinned; everything else kept off its core ----
    printf("[INFO] Pinning Mastik prober to Core 0...\n");
    pin_to_core(0);

    Clusters_t *Clusters = eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), NoC);
    if (!Clusters) {
        fprintf(stderr, "[web] FATAL: eviction_sets_to_Clusters failed\n");
        l3_release(l3);
        return 1;
    }
    uint32_t *matrix = (uint32_t *)calloc(totalSweeps_forCluster * NoC, sizeof(uint32_t));
    if (!matrix) {
        fprintf(stderr, "[web] FATAL: matrix allocation failed\n");
        free_Clusters(Clusters);
        l3_release(l3);
        return 1;
    }

    // Chrome is deliberately UNPINNED: an explicit ALL-CORES mask, not "no mask". Affinity is
    // inherited across fork/exec and this process is pinned to core 0, so omitting the mask
    // would silently confine the whole browser to the prober's measurement core.
    //
    // Consequence, and the honest claim for this arm: renderers may transiently co-reside on
    // core 0 and share its private L1/L2, so this is MICROARCHITECTURAL contention, not
    // LLC-only -- the same caveat the Stage 4 JS arm carries, which is what keeps the two
    // website arms comparable. Stages 1-3 remain the LLC-only controlled bound.
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
    cpu_set_t all_cores;
    CPU_ZERO(&all_cores);
    for (long i = 0; i < nproc && i < CPU_SETSIZE; i++) CPU_SET((int)i, &all_cores);

    // Chrome must not be root while visiting live sites, but the prober must be (hugepages,
    // /proc/self/pagemap). Drop to the invoking login user so the zygote sandbox keeps working.
    WebVictimCfg cfg = {
        .profile_dir  = WEB_PROBER_PROFILE,
        .initial_url  = "about:blank",   // no JS sampler in this arm; the tab is just a parking spot
        .drop_privs   = 0,
        .uid = 0, .gid = 0, .home = NULL,
        .no_sandbox   = 0,
        .set_affinity = 1,
        .cpu_mask     = &all_cores,
    };
    // Which unprivileged user should Chrome run as?
    //
    // SUDO_UID alone is NOT sufficient. The sweep reaches this binary through TWO levels of
    // sudo (run_all_configs.sh -> sudo -> batch_runner.sh -> sudo -> here), and when *root*
    // invokes sudo, sudo sets SUDO_UID=0. Believing that yields a "drop" to uid 0, which is no
    // drop at all -- the child's own assertion catches it and exits, so Chrome never launches
    // and the run dies 60 s later at wait_cdp with a misleading DevTools error.
    //
    // So: prefer CHROME_UID/CHROME_GID, which run_all_configs.sh captures while it is still
    // the login user and forwards through both sudo layers. Fall back to SUDO_UID/SUDO_GID for
    // a direct single-sudo invocation (the manual smoke-test path), and believe either only
    // when NONZERO.
    static char home_buf[256];
    unsigned long u = 0, g = 0;
    const char *id_src = NULL;
    if (env_nonzero_id("CHROME_UID", &u) && env_nonzero_id("CHROME_GID", &g))
        id_src = "CHROME_UID/CHROME_GID";
    else if (env_nonzero_id("SUDO_UID", &u) && env_nonzero_id("SUDO_GID", &g))
        id_src = "SUDO_UID/SUDO_GID";

    if (geteuid() == 0 && id_src) {
        cfg.drop_privs = 1;
        cfg.uid = (uid_t)u;
        cfg.gid = (gid_t)g;
        // Ask the passwd database rather than assuming /home/<name>: the home directory is
        // where Chrome puts its caches, and a wrong one fails in obscure ways.
        struct passwd *pw = getpwuid(cfg.uid);
        if (pw && pw->pw_dir && *pw->pw_dir) {
            snprintf(home_buf, sizeof(home_buf), "%s", pw->pw_dir);
            cfg.home = home_buf;
        }
        printf("[web] chrome: dropping to uid=%lu gid=%lu (from %s), sandbox kept, unpinned\n",
               u, g, id_src);
    } else if (geteuid() == 0) {
        // Nothing to drop to. Chrome cannot start sandboxed as root, and this is the risky
        // mode -- say so loudly rather than quietly browsing live sites as root.
        cfg.no_sandbox = 1;
        fprintf(stderr, "[web] WARNING: no non-root uid available (CHROME_UID and SUDO_UID are "
                        "unset or 0) -- running Chrome AS ROOT with --no-sandbox while visiting "
                        "live sites.\n"
                        "[web] WARNING: set CHROME_UID/CHROME_GID to the login user. You may "
                        "also need `xhost +SI:localuser:root`.\n");
    }

    // A stale profile owned by the other uid (e.g. left by a --no-sandbox fallback run) would
    // stop the dropped-privilege Chrome from starting. Clear it; it holds no run state.
    {
        char rm[512];
        snprintf(rm, sizeof(rm), "rm -rf '%s'", WEB_PROBER_PROFILE);
        if (system(rm) == -1) fprintf(stderr, "[web] WARNING: could not clear the profile dir\n");
    }

    signal(SIGINT,  web_prober_cleanup);
    signal(SIGTERM, web_prober_cleanup);
    // A write() to a socket Chrome has closed would otherwise raise SIGPIPE, whose default
    // action kills the prober outright, mid-run, with no diagnostic. Ignoring it turns the same
    // condition into an EPIPE return that web_victim_open_send reports.
    signal(SIGPIPE, SIG_IGN);

    g_chrome_pid = web_launch_chrome(&cfg);
    if (g_chrome_pid <= 0) {
        fprintf(stderr, "[web] FATAL: could not launch Chrome\n");
        free(matrix); free_Clusters(Clusters); l3_release(l3);
        return 1;
    }
    printf("[web] launched chrome (pid %d); waiting for DevTools...\n", (int)g_chrome_pid);
    if (web_wait_cdp(WEB_CDP_TIMEOUT_S) != 0) {
        fprintf(stderr, "[web] FATAL: DevTools endpoint never came up (is Chrome reachable on "
                        "DISPLAY :0?)\n");
        web_prober_cleanup(0);
    }
    // Calibrate the navigation offset ONCE, with a throwaway synchronous open of about:blank.
    // Per-sample alignment cannot be observed in the split-send design (the reply is only read
    // after the trace), and it does not need to be: this is a property of the browser and the
    // loopback path, not of any particular site. It is the number that says how far INTO each
    // trace the page load actually commits.
    {
        char cal_id[128];
        double c0 = now_ms();
        if (web_victim_open(WEB_CALIBRATION_URL, cal_id, sizeof(cal_id)) == 0) {
            double cal_ms = now_ms() - c0;
            web_victim_close(cal_id);
            printf("[web] CDP round trip: %.1f ms (one throwaway open of %s)\n",
                   cal_ms, WEB_CALIBRATION_URL);
            printf("[web] => navigation commits ~%.1f ms into each %.0f ms trace (%.2f%% of it, "
                   "at the head)\n", cal_ms, tst_sec * 1000.0, 100.0 * cal_ms / (tst_sec * 1000.0));
            if (cal_ms > WEB_ALIGN_WARN_MS)
                fprintf(stderr, "[web] WARNING: that is more than %d ms -- an unusually large "
                        "slice of the load transient is under-sampled\n", WEB_ALIGN_WARN_MS);
        } else {
            fprintf(stderr, "[web] WARNING: calibration open failed; continuing without an "
                            "alignment figure\n");
        }
    }
    printf("[web] browser ready. starting round-robin collection.\n");

    // ---- Collection: outer = iteration, inner = site ----
    // Round-robin so every site's samples spread across the whole run. With LIVE page loads
    // this is what stops the classifier learning time-of-day and network drift instead of the
    // site.
    int collected = 0, failed = 0;
    for (int iteration = start_iteration; iteration < start_iteration + batch_size; iteration++) {
        for (int s = 0; s < num_sites; s++) {
            const char *slug = sites[s].slug;
            const char *url  = sites[s].url;

            // Liveness check OUTSIDE the trace window, so a dead browser is caught before a
            // CSV has already been written for it.
            if (!web_cdp_alive()) {
                web_diagnose_chrome_death((pid_t)g_chrome_pid);
                fprintf(stderr, "[web] FATAL: Chrome/DevTools is gone; aborting so the run does "
                                "not fill with victim-less traces. CSVs so far are intact.\n");
                web_prober_cleanup(0);
            }

            char path[512];
            snprintf(path, sizeof(path), "data/%s/%s/%s/%d.csv",
                     clock_subdir, output_dir, slug, iteration);
            char mkdir_cmd[640];
            // Quoted: the slug is website-derived, so do not rely on the charset check alone.
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"$(dirname '%s')\"", path);
            if (system(mkdir_cmd) == -1) {
                fprintf(stderr, "[web] WARNING: mkdir failed for %s\n", path);
            }

            printf("[web] iter %d  site %-20s %s\n", iteration, slug, url);
            fflush(stdout);

            // Connect and build the request BEFORE t=0, so the only thing standing between
            // "not sampling" and "sampling" is a single write() syscall.
            int cdp_fd = web_victim_open_send(url);
            if (cdp_fd < 0) {
                fprintf(stderr, "[web] WARNING: could not send the tab-open for %s; skipping\n", slug);
                failed++;
                sleep_us(cooldown_us);
                continue;
            }

            // ---- t = 0 ----
            // The write above only queued bytes in the kernel. Chrome still has to wake its
            // DevTools thread, create the target and spawn a renderer -- milliseconds -- while
            // the first cache probe below happens microseconds from now. So sampling is
            // definitively running before the page starts loading, which is the ordering the
            // JS arm gets for free by having a separate sampler process.
            switch (timer_mode) {
                case 10:
                    get_spatioTemporal_memoryGram(Clusters, NoC, TST_cycles, SST_cycles,
                                                  matrix, path, K);
                    break;
                case 11:
                    get_spatioTemporal_memoryGram_ChromeMock(Clusters, NoC, TST_cycles,
                                                             SST_cycles, matrix, path, K);
                    break;
                default:
                    fprintf(stderr, "[web] FATAL: timer_mode %d is not a website mode\n", timer_mode);
                    web_prober_cleanup(0);
            }

            // Collect the reply Chrome sent ~TST seconds ago; it has been sitting in the
            // socket receive buffer ever since. Closes cdp_fd either way.
            char id[128];
            if (web_victim_open_recv(cdp_fd, id, sizeof(id)) != 0) {
                // The trace ran with no victim. The sampler already wrote the CSV, so remove it
                // rather than leave a mislabelled sample in the class directory.
                fprintf(stderr, "[web] WARNING: victim tab failed for %s; discarding %s\n", slug, path);
                unlink(path);
                failed++;
            } else {
                collected++;
                // Close the victim tab: kills its renderer, and with it that page's memory cache.
                web_victim_close(id);
            }

            sleep_us(cooldown_us);
            memset(matrix, 0, totalSweeps_forCluster * NoC * sizeof(uint32_t));
        }
        if (iteration + 1 < start_iteration + batch_size) {
            printf("[COOLDOWN] Inter-round cooldown %ds...\n", WEB_INTER_ROUND_S);
            sleep(WEB_INTER_ROUND_S);
        }
    }

    // ---- Teardown ----
    if (g_chrome_pid > 0) kill((pid_t)g_chrome_pid, SIGTERM);
    sleep(1);
    web_pkill_armed();

    free(matrix);
    free_Clusters(Clusters);
    l3_release(l3);

    printf("[web] collection complete: %d traces, %d failed.\n", collected, failed);
    return failed == 0 ? 0 : 1;
}
