// coverage_validator.c
// -----------------------------------------------------------------------------
// Cross-process Prime+Probe validation of the JS lazy mapping.
//
// This process (the PROBER, pinned to core 0) loads the real Mastik LLC mapping
// from a pre-saved BIN file and launches Chrome (pinned to core 1) on the JS
// "validate" page. The browser continuously hammers ONE lazy cluster at a time
// (told which via the Flask coordinator, /ctl/set). For each Mastik set we do an
// address-by-address Prime+Probe (l3_monitor_manual + l3_bprobecount, the
// get_transTable pattern) and count JS-induced misses. A set that misses while
// cluster c is hammered means cluster c physically contends on that set.
//
// Works without shared memory because the LLC is inclusive (Coffee Lake i7-9700k):
// the two processes contend on the same physical sets. The only sync is temporal
// (which cluster the browser hammers), done via the Flask server.
//
// Output: per-cluster miss vector over all sets -> coverage (which sets are ever
// hit, which are cold) and orthogonality (do clusters map to disjoint Mastik
// groups). get_active_group() reports each cluster's dominant group and %.
// -----------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>   // mkdir
#include <sys/types.h>
#include <sys/mman.h>   // mmap (JS-faithful lazy-map victim, jsmap mode)
#include <math.h>       // log2

#include <mastik/l3.h>
#include <mastik/low.h>   // rdtscp64
#include <mastik/impl.h>  // LNEXT (pointer-chase macro)
#include "utils.h"        // virt_to_phys, load_physical_mapping, maccessMy
#include "mastikElite.h"  // pin_to_core, load_mapping_and_eSetsFrom_BIN_file, eviction_sets_to_Clusters
#include "lazy_map.h"     // LazyMap, build_lazy_mapping, free_lazy_mapping, sweep_lazy_once

#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define MAPPING_FILE_A  "mapping_A.bin"
// Native experiment: mapping B is the lazy victim (clusters built from it).
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"
#define MAPPING_FILE_B  "mapping_B.bin"

#define PROBER_CORE   0
#define BROWSER_CORE  1

#define DEFAULT_NOC   16          // NoC when none given on the command line

// Coordinator is one-way (C -> browser): C publishes which cluster to sweep; the
// browser polls and sweeps it continuously, never acking. Sentinels:
#define CTL_IDLE  (-1)            // browser sweeps nothing (baseline noise floor)
#define CTL_STOP  (-2)            // browser exits

#define TSC_HZ        3.6e9       // invariant TSC frequency (i7-9700k base); calibrate per CPU
// Spin (prime->probe) window scales ~1/NoC because one JS sweep does: 800us @ NoC=16,
// doubling per NoC halving (64->200, 32->400, 8->1600, ...) = SPIN_US_AT_16 * 16 / NoC.
#define SPIN_US_AT_16 800
#define BUILD_WAIT_S  10          // fixed wait for Chrome to load + build the lazy mapping
#define RAMP_MS       400         // realign delay after switching cluster, before scanning
#define BASELINE_ROWS 15           // number of idle baseline rows to collect (noise floor)

#define DATA_DIR      "data/coverage"   // browser outputs go under stable/data/coverage/
#define NATIVE_DATA_DIR "data/coverage/native"  // native outputs go under stable/data/coverage/native/
#define CHROME_PROFILE "/tmp/chrome-validate"
#define SERVER_PORT 8080

// Recursive "mkdir -p" for a relative path (ignores already-exists).
static void mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

// True iff x is a power of two in [1, 64].
static int is_pow2_le64(int x) {
    return x >= 1 && x <= 64 && (x & (x - 1)) == 0;
}

// ---- tiny raw-socket HTTP POST to the Flask coordinator (no libcurl) ----
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

// POST a JSON body to `path`; drain (and thus wait for) the response.
static int http_post(const char *path, const char *body) {
    int fd = ctl_connect();
    if (fd < 0) { perror("connect POST"); return -1; }
    int blen = (int)strlen(body);
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n%s", path, SERVER_PORT, blen, body);
    if (write(fd, req, rlen) < 0) { perror("write POST"); close(fd); return -1; }
    char buf[256];
    while (read(fd, buf, sizeof(buf)) > 0) { /* drain */ }
    close(fd);
    return 0;
}

// Publish which cluster the browser should sweep (or CTL_IDLE / CTL_STOP).
static void ctl_set(int cluster) {
    char body[48];
    snprintf(body, sizeof(body), "{\"cluster\":%d}", cluster);
    http_post("/ctl/set", body);
}

// Inert busy-wait of `cycles` TSC ticks. Near-zero memory footprint, so it does NOT
// itself evict the primed set -- only the browser's continuous sweep does.
static inline void spin_cycles(uint64_t cycles) {
    uint64_t t0 = rdtscp64();
    while (rdtscp64() - t0 < cycles) { /* busy wait */ }
}

// Prime -> spin (~spinCycles, during which the browser sweeps the cluster
// continuously on the other core) -> probe ONE set. res[0] after the forward probe =
// miss count = lines the browser's sweep evicted. Mirrors the get_transTable inner
// loop, with the browser's hammer replacing the second Mastik "attacker" mapping.
static uint16_t probe_set(l3pp_t l3, int setIdx, void *head, uint64_t spinCycles) {
    uint16_t res[4];
    l3_unmonitorall(l3);
    l3_monitor_manual(l3, setIdx, head);
    l3_bprobecount(l3, res);     // prime (backward warm)
    spin_cycles(spinCycles);     // window: browser sweeps the cluster >= once
    l3_probecount(l3, res);      // measure (forward); res[0] = misses
    return res[0];
}

// Scan every set once while the browser hammers the currently-set cluster.
static void scan_all_sets(l3pp_t l3, void **e_sets, int numSets, uint16_t *out,
                          uint64_t spinCycles) {
    for (int i = 0; i < numSets; i++) {
        out[i] = e_sets[i] ? probe_set(l3, i, e_sets[i], spinCycles) : 0;
    }
}

// ---- NATIVE experiment helpers (serial; the victim sweep runs in-process) ----

// One full untimed traversal of cluster c's circular list: counts[c]*assoc lines,
// each touched exactly once. Serial analog of JS main.js hammerCluster (no timer in
// the hot path). Replaces the browser mode's spin_cycles() window.
static void sweep_cluster_once(Clusters_t *clusters, int c, int assoc) {
    void *head = clusters->clusterHeads[c];
    if (!head) return;
    void *curr = head;
    int n = clusters->counts[c] * assoc;   // lines in this cluster (== JS nodeCounts[c])
    for (int i = 0; i < n; i++) {
        maccessMy(curr);
        curr = LNEXT(curr);
    }
}

// shuffle_cluster_nodes() (Fisher-Yates over a cluster's circular node list) now lives in
// mastikElite.c and is declared in mastikElite.h, so it is shared with the fingerprinting
// path's shuffled-cluster experiment. Behaviour here is unchanged.

// Prime ONE set (mapping A) -> sweep cluster c once (mapping B, in-process) -> probe.
// res[0] after the forward probe = miss count = lines the lazy sweep evicted. Serial
// analog of probe_set: the single cluster sweep replaces the spin window.
static uint16_t probe_set_native(l3pp_t l3, int setIdx, void *head,
                                 Clusters_t *clusters, int c, int assoc) {
    uint16_t res[4];
    l3_unmonitorall(l3);
    l3_monitor_manual(l3, setIdx, head);
    l3_bprobecount(l3, res);                 // prime (backward warm)
    sweep_cluster_once(clusters, c, assoc);  // victim sweeps cluster c once
    l3_probecount(l3, res);                  // measure (forward); res[0] = misses
    return res[0];
}

// ---- JSMAP experiment: a C port of the JS main.js LazyMapping victim ----
// The LazyMap struct, build_lazy_mapping(), free_lazy_mapping() and sweep_lazy_once()
// now live in the shared src/lazy_map.{c,h} module (also used by MastikElite's
// timer_mode==2 stress-ng fingerprinting), so there is a single copy of the JS-faithful
// lazy-map construction. See lazy_map.h for the JS geometry constants and the struct.

// Prime ONE set (mapping A) -> sweep JS lazy cluster c -> probe. Mirrors
// probe_set_native but sweeps the mmap JS-faithful victim instead of a Clusters_t.
// EV = eviction-strategy sweep active iff any of A/D/C is non-identity (>1).
static inline int ev_active(int A, int D, int C) { return A > 1 || D > 1 || C > 1; }

// One prime -> victim sweep -> probe. The victim sweep is either the JS pointer chase
// (sweep_lazy_once, default) or the Rowhammer.js eviction-strategy pattern (sweep_lazy_evict,
// when A/D/C are non-identity). Everything else mirrors probe_set_native.
static uint16_t probe_set_jsmap(l3pp_t l3, int setIdx, void *head, const LazyMap *m, int c,
                                int passes, int accessesPerLine, int sameAddr, int buddyTouch,
                                int A, int D, int C) {
    uint16_t res[4];
    l3_unmonitorall(l3);
    l3_monitor_manual(l3, setIdx, head);
    l3_bprobecount(l3, res);                                    // prime (backward warm)
    if (ev_active(A, D, C))
        sweep_lazy_evict(m, c, A, D, C);                        // eviction-strategy access pattern
    else
        sweep_lazy_once(m, c, passes, accessesPerLine, sameAddr, buddyTouch); // JS pointer chase
    l3_probecount(l3, res);                                     // measure; res[0] = misses
    return res[0];
}

static pid_t launch_chrome(int noc) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://localhost:%d/?mode=validate&label=cov_%dC_2TST_45K_2288cycles",
             SERVER_PORT, noc);
    pid_t pid = fork();
    if (pid == 0) {
        pin_to_core(BROWSER_CORE);           // inherited by chrome's child procs
        setenv("DISPLAY", ":0", 1);
        // Prober runs as root; the :0 X server's auth cookie lives in gdm's
        // Xauthority (NOT /home/ubu/.Xauthority, which is the :1 Xtigervnc cookie).
        // Without this Chrome gets "No protocol specified" / "Missing X server".
        setenv("XAUTHORITY", "/run/user/1000/gdm/Xauthority", 1);
        execlp("google-chrome", "google-chrome",
               "--no-sandbox",  // prober runs as root (hugepages/pagemap); Chrome zygote aborts otherwise.
                                // Disables only seccomp/namespace syscall isolation, not Site Isolation or
                                // V8 allocation -> no effect on the LLC cache signal we measure.
               "--user-data-dir=" CHROME_PROFILE,
               "--no-first-run", "--no-default-browser-check",
               "--new-window", url, (char *)NULL);
        perror("execlp google-chrome");
        _exit(127);
    }
    return pid;
}

// -----------------------------------------------------------------------------
// BROWSER experiment (default): C is the prober; Chrome hammers one lazy cluster
// at a time (driven via Flask), and we prime -> spin-window -> probe each Mastik set.
// -----------------------------------------------------------------------------
static int run_browser_experiment(int noc, int iterIdx, int spinUs) {
    uint64_t spinCycles = (uint64_t)((double)spinUs * 1e-6 * TSC_HZ);
    // Mastik "groups" now track NoC (only used for the informational dominant-group print).
    int mastikGroups = noc;

    pin_to_core(PROBER_CORE);

    // 1. Load the real Mastik mapping (eviction sets) from the BIN file.
    l3pp_t l3 = NULL;
    void **e_sets = NULL;
    if (load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, HUGEPAGE_PATH_A, MAPPING_FILE_A)) {
        fprintf(stderr, "Failed to load mapping from %s / %s\n", HUGEPAGE_PATH_A, MAPPING_FILE_A);
        return 1;
    }
    int numSets = l3_getSets(l3);
    int assoc   = l3_getAssociativity(l3);
    if (numSets % noc != 0) {
        fprintf(stderr, "numSets (%d) not divisible by NoC (%d)\n", numSets, noc);
        return 1;
    }
    int setsPerGroup = numSets / mastikGroups;
    printf("[cov] numSets=%d assoc=%d NoC=%d iter=%d setsPerGroup=%d\n",
           numSets, assoc, noc, iterIdx, setsPerGroup);

    // Physical addresses (for bits 8-11 labelling, apples-to-apples with clusters).
    int paCount = 0;
    uint64_t *pas = load_physical_mapping(MAPPING_FILE_A, &paCount);

    printf("[cov] spin window = %d us (%lu TSC cycles @ %.2f GHz)\n",
           spinUs, (unsigned long)spinCycles, TSC_HZ / 1e9);

    // 2. Launch the browser victim (the "start request for lazy mapping"). The browser
    //    builds the mapping on load and never acks, so we wait a fixed period for the
    //    page to load + build before driving it.
    ctl_set(CTL_IDLE);                   // coordinator starts idle
    pid_t chrome = launch_chrome(noc);
    printf("[cov] launched chrome (pid %d); waiting %ds for page load + mapping build...\n",
           chrome, BUILD_WAIT_S);
    sleep(BUILD_WAIT_S);
    printf("[cov] scanning all %d clusters, %d sets each.\n", noc, numSets);

    // 3. Per-cluster scan. The browser sweeps the current cluster continuously; between
    //    clusters we realign (set the new cluster + RAMP wait) so the browser has
    //    switched and is hammering steadily before we start probing.
    uint16_t *matrix   = calloc((size_t)noc * numSets, sizeof(uint16_t));
    uint16_t *baseline = calloc((size_t)BASELINE_ROWS * numSets, sizeof(uint16_t));

    for (int c = 0; c < noc; c++) {
        ctl_set(c);                      // request: sweep cluster c
        usleep(RAMP_MS * 1000);          // realign timings before scanning
        printf("[cov] scanning sets while JS sweeps cluster %d ...\n", c);
        scan_all_sets(l3, e_sets, numSets, &matrix[(size_t)c * numSets], spinCycles);
        int g = get_active_group(&matrix[(size_t)c * numSets], setsPerGroup, numSets, assoc);
        printf("[cov] cluster %d -> dominant group %d\n", c, g);
    }

    // 4. Baseline: browser idle (sweeps nothing), same spin window, for the noise floor.
    ctl_set(CTL_IDLE);
    for (int b = 0; b < BASELINE_ROWS; b++) {
        usleep(RAMP_MS * 1000);
        printf("[cov] baseline scan %d/%d (JS idle)...\n", b + 1, BASELINE_ROWS);
        scan_all_sets(l3, e_sets, numSets, &baseline[(size_t)b * numSets], spinCycles);
    }

    // 5. Stop the browser and tear down chrome.
    ctl_set(CTL_STOP);
    if (chrome > 0) kill(chrome, SIGTERM);

    // 6. Write outputs under data/coverage/NoC{nn}/{iter}.csv (+ a shared labels file).
    char dir[256], missPath[300];
    snprintf(dir, sizeof(dir), "%s/NoC%02d", DATA_DIR, noc);
    mkdir_p(dir);
    snprintf(missPath, sizeof(missPath), "%s/%03d.csv", dir, iterIdx);

    FILE *fm = fopen(missPath, "w");
    if (fm) {
        for (int i = 0; i < numSets; i++) fprintf(fm, "%sS%d", i ? "," : "", i);
        fprintf(fm, "\n");
        for (int c = 0; c < noc; c++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", matrix[(size_t)c * numSets + i]);
            fprintf(fm, "\n");
        }
        for (int b = 0; b < BASELINE_ROWS; b++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", baseline[(size_t)b * numSets + i]);
            fprintf(fm, "\n");
        }
        fclose(fm);
        printf("[cov] wrote %s (%d clusters + %d baseline x %d sets)\n",
               missPath, noc, BASELINE_ROWS, numSets);
    } else {
        perror("fopen miss matrix");
    }

    // Labels depend only on mapping_A.bin (not NoC/iter), so a single shared file suffices;
    // rewritten idempotently each run.
    char labelsPath[300];
    snprintf(labelsPath, sizeof(labelsPath), "%s/set_labels.csv", DATA_DIR);
    FILE *fl = fopen(labelsPath, "w");
    if (fl && pas) {
        fprintf(fl, "set_idx,pa,bits8_11\n");
        for (int i = 0; i < numSets; i++) {
            uint64_t pa = pas[(size_t)i * assoc];      // first way's physical address
            fprintf(fl, "%d,0x%lx,%lu\n", i, pa, (pa >> 8) & 0xF);
        }
        fclose(fl);
        printf("[cov] wrote %s\n", labelsPath);
    }

    free(matrix); free(baseline); free(pas);
    l3_release(l3);
    return 0;
}

// -----------------------------------------------------------------------------
// NATIVE experiment: everything serial in ONE process, no browser/Flask/sync.
// Mapping A (real Mastik eviction sets) is the prober; mapping B is the lazy
// victim -- we build the SAME coarse clusters the JS builds (eviction_sets_to_Clusters,
// address bits 6-11 for NoC<=64) and, per cluster, prime->sweep-once->probe every
// Mastik set. This is the tightest, fairest C analog of the browser coverage scan;
// it answers whether the "coverage drops as NoC grows" effect is intrinsic to the
// lazy clustering rather than a browser/JS artifact.
// -----------------------------------------------------------------------------
static int run_native_experiment(int noc, int iterIdx, int shuffle) {
    pin_to_core(PROBER_CORE);
    // Deterministic per-iteration shuffle (reproducible A/B across reruns of the same iter).
    if (shuffle) srand((unsigned)(iterIdx + 1));

    // 1. Mapping A: the prober's real Mastik eviction sets (used for prime/probe).
    l3pp_t l3A = NULL;
    void **e_setsA = NULL;
    if (load_mapping_and_eSetsFrom_BIN_file(&l3A, &e_setsA, HUGEPAGE_PATH_A, MAPPING_FILE_A)) {
        fprintf(stderr, "Failed to load mapping A from %s / %s\n", HUGEPAGE_PATH_A, MAPPING_FILE_A);
        return 1;
    }
    int numSets = l3_getSets(l3A);
    int assoc   = l3_getAssociativity(l3A);
    if (numSets % noc != 0) {
        fprintf(stderr, "numSets (%d) not divisible by NoC (%d)\n", numSets, noc);
        return 1;
    }

    // 2. Mapping B: the lazy victim. Build the coarse clusters exactly as the JS/browser
    //    path does (address-based bits 6-11 for NoC<=64) via eviction_sets_to_Clusters.
    l3pp_t l3B = NULL;
    void **e_setsB = NULL;
    if (load_mapping_and_eSetsFrom_BIN_file(&l3B, &e_setsB, HUGEPAGE_PATH_B, MAPPING_FILE_B)) {
        fprintf(stderr, "Failed to load mapping B from %s / %s\n", HUGEPAGE_PATH_B, MAPPING_FILE_B);
        return 1;
    }
    Clusters_t *clusters = eviction_sets_to_Clusters(&e_setsB, l3_getSets(l3B), noc);
    if (!clusters) {
        fprintf(stderr, "Failed to build clusters from mapping B\n");
        return 1;
    }

    // Optional: shuffle each cluster's line order ONCE before measuring, to break the
    // HW-prefetcher stream (native analog of JS shuffle(pages)). A/B control for the
    // "sweep evicts the next cluster" artifact.
    if (shuffle) {
        for (int c = 0; c < noc; c++) shuffle_cluster_nodes(clusters, c, assoc);
        printf("[cov-native] clusters shuffled internally (prefetch-defeating).\n");
    }

    int setsPerGroup = numSets / noc;
    printf("[cov-native] numSets=%d assoc=%d NoC=%d iter=%d setsPerGroup=%d\n",
           numSets, assoc, noc, iterIdx, setsPerGroup);

    // Physical addresses of mapping A (for bits 8-11 labelling, as in browser mode).
    int paCount = 0;
    uint64_t *pas = load_physical_mapping(MAPPING_FILE_A, &paCount);

    printf("[cov-native] scanning all %d clusters, %d sets each (serial, sweep-once).\n",
           noc, numSets);

    // 3. Per-cluster scan: prime -> sweep cluster c once -> probe, for every Mastik set.
    uint16_t *matrix   = calloc((size_t)noc * numSets, sizeof(uint16_t));
    uint16_t *baseline = calloc((size_t)BASELINE_ROWS * numSets, sizeof(uint16_t));

    for (int c = 0; c < noc; c++) {
        uint16_t *row = &matrix[(size_t)c * numSets];
        for (int i = 0; i < numSets; i++) {
            row[i] = e_setsA[i] ? probe_set_native(l3A, i, e_setsA[i], clusters, c, assoc) : 0;
        }
        int g = get_active_group(row, setsPerGroup, numSets, assoc);
        printf("[cov-native] cluster %d -> dominant group %d\n", c, g);
    }

    // 4. Baseline: prime -> inert busy-wait (~one sweep) -> probe, with NO victim sweep.
    // The busy-wait (spin_cycles: near-zero memory footprint, so it does NOT itself evict the
    // primed set) gives each baseline probe the SAME prime->probe time window as a real row,
    // so the noise floor is measured over a comparable interval instead of an instantaneous
    // prime->probe. Calibrate the window to the min of a few timed cluster sweeps.
    uint64_t sweepCycles = UINT64_MAX;
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = rdtscp64();
        sweep_cluster_once(clusters, 0, assoc);
        uint64_t dt = rdtscp64() - t0;
        if (dt < sweepCycles) sweepCycles = dt;
    }
    printf("[cov-native] baseline busy-wait = %lu cycles (~one cluster sweep)\n",
           (unsigned long)sweepCycles);
    for (int b = 0; b < BASELINE_ROWS; b++) {
        uint16_t *row = &baseline[(size_t)b * numSets];
        for (int i = 0; i < numSets; i++)
            row[i] = e_setsA[i] ? probe_set(l3A, i, e_setsA[i], sweepCycles) : 0;
    }

    // 5. Write outputs. Shuffled and unshuffled runs go to separate trees so an A/B
    //    comparison never overwrites the other:
    //      unshuffled -> data/coverage/native/NoC{nn}/{iter}.csv
    //      shuffled   -> data/coverage/native_shuffled/NoC{nn}/{iter}.csv
    const char *baseDir = shuffle ? NATIVE_DATA_DIR "_shuffled" : NATIVE_DATA_DIR;
    char dir[256], missPath[300];
    snprintf(dir, sizeof(dir), "%s/NoC%02d", baseDir, noc);
    mkdir_p(dir);
    snprintf(missPath, sizeof(missPath), "%s/%03d.csv", dir, iterIdx);

    FILE *fm = fopen(missPath, "w");
    if (fm) {
        for (int i = 0; i < numSets; i++) fprintf(fm, "%sS%d", i ? "," : "", i);
        fprintf(fm, "\n");
        for (int c = 0; c < noc; c++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", matrix[(size_t)c * numSets + i]);
            fprintf(fm, "\n");
        }
        for (int b = 0; b < BASELINE_ROWS; b++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", baseline[(size_t)b * numSets + i]);
            fprintf(fm, "\n");
        }
        fclose(fm);
        printf("[cov-native] wrote %s (%d clusters + %d baseline x %d sets)\n",
               missPath, noc, BASELINE_ROWS, numSets);
    } else {
        perror("fopen miss matrix");
    }

    // Labels depend only on mapping_A.bin; a single shared file suffices (rewritten each run).
    char labelsPath[300];
    snprintf(labelsPath, sizeof(labelsPath), "%s/set_labels.csv", baseDir);
    FILE *fl = fopen(labelsPath, "w");
    if (fl && pas) {
        fprintf(fl, "set_idx,pa,bits8_11\n");
        for (int i = 0; i < numSets; i++) {
            uint64_t pa = pas[(size_t)i * assoc];      // first way's physical address
            fprintf(fl, "%d,0x%lx,%lu\n", i, pa, (pa >> 8) & 0xF);
        }
        fclose(fl);
        printf("[cov-native] wrote %s\n", labelsPath);
    }

    free(matrix); free(baseline); free(pas);
    free_Clusters(clusters);
    l3_release(l3A);
    l3_release(l3B);
    return 0;
}

// -----------------------------------------------------------------------------
// JSMAP experiment: like run_native_experiment, but the victim is a C port of the
// JS main.js LazyMapping (fresh mmap buffer, bits 6-11 partition, shuffled pages)
// instead of a saved Mastik mapping. mapping_A is still loaded, ONLY for prime/probe.
// Answers whether the "sweep evicts the next cluster" artifact survives when the
// native victim is byte-for-byte the same construction the browser sweeps.
// -----------------------------------------------------------------------------
static int run_native_jsmap_experiment(int noc, int iterIdx, int shuffle,
                                       int passes, int accessesPerLine, int sameAddr,
                                       int buddyTouch, int A, int D, int C) {
    pin_to_core(PROBER_CORE);
    // Deterministic per-iteration page shuffle (reproducible across reruns of the same iter).
    if (shuffle) srand((unsigned)(iterIdx + 1));

    // 1. Mapping A: the prober's real Mastik eviction sets (used for prime/probe only).
    l3pp_t l3A = NULL;
    void **e_setsA = NULL;
    if (load_mapping_and_eSetsFrom_BIN_file(&l3A, &e_setsA, HUGEPAGE_PATH_A, MAPPING_FILE_A)) {
        fprintf(stderr, "Failed to load mapping A from %s / %s\n", HUGEPAGE_PATH_A, MAPPING_FILE_A);
        return 1;
    }
    int numSets = l3_getSets(l3A);
    int assoc   = l3_getAssociativity(l3A);
    if (numSets % noc != 0) {
        fprintf(stderr, "numSets (%d) not divisible by NoC (%d)\n", numSets, noc);
        return 1;
    }
    // The JS victim hard-codes its geometry; it must match the Mastik geometry we probe.
    if (numSets != JS_LLC_SETS || assoc != JS_LLC_WAYS) {
        fprintf(stderr, "JS geometry mismatch: Mastik numSets=%d assoc=%d, JS expects %d/%d\n",
                numSets, assoc, JS_LLC_SETS, JS_LLC_WAYS);
        return 1;
    }

    // 2. Build the JS-faithful lazy-map victim in a fresh page-aligned mmap buffer.
    LazyMap map;
    if (build_lazy_mapping(&map, noc, JS_LLC_SETS, JS_LLC_WAYS, shuffle)) {
        fprintf(stderr, "Failed to build JS lazy mapping\n");
        return 1;
    }
    printf("[cov-jsmap] JS victim built (mmap %zu MB, pages %s, passes=%d, accesses/line=%d, mode=%s).\n",
           map.bytes / (1024 * 1024), shuffle ? "shuffled" : "in-order", passes, accessesPerLine,
           sameAddr ? "same-addr" : "words");

    int setsPerGroup = numSets / noc;
    printf("[cov-jsmap] numSets=%d assoc=%d NoC=%d iter=%d setsPerGroup=%d\n",
           numSets, assoc, noc, iterIdx, setsPerGroup);

    int paCount = 0;
    uint64_t *pas = load_physical_mapping(MAPPING_FILE_A, &paCount);

    printf("[cov-jsmap] scanning all %d clusters, %d sets each (serial, JS sweep-once).\n",
           noc, numSets);

    // 3. Per-cluster scan: prime -> sweep JS cluster c once -> probe, for every Mastik set.
    uint16_t *matrix   = calloc((size_t)noc * numSets, sizeof(uint16_t));
    uint16_t *baseline = calloc((size_t)BASELINE_ROWS * numSets, sizeof(uint16_t));

    for (int c = 0; c < noc; c++) {
        uint16_t *row = &matrix[(size_t)c * numSets];
        for (int i = 0; i < numSets; i++) {
            row[i] = e_setsA[i] ? probe_set_jsmap(l3A, i, e_setsA[i], &map, c, passes, accessesPerLine, sameAddr, buddyTouch, A, D, C) : 0;
        }
        int g = get_active_group(row, setsPerGroup, numSets, assoc);
        printf("[cov-jsmap] cluster %d -> dominant group %d\n", c, g);
    }

    // 4. Baseline: prime -> inert busy-wait (~one sweep) -> probe, with NO victim sweep.
    // The busy-wait (spin_cycles: near-zero memory footprint, so it does NOT itself evict the
    // primed set) gives each baseline probe the SAME prime->probe time window as a real row,
    // so the noise floor is measured over a comparable interval instead of an instantaneous
    // prime->probe. Calibrate the window to the min of a few timed JS sweeps.
    uint64_t sweepCycles = UINT64_MAX;
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = rdtscp64();
        if (ev_active(A, D, C)) sweep_lazy_evict(&map, 0, A, D, C);
        else                    sweep_lazy_once(&map, 0, passes, accessesPerLine, sameAddr, buddyTouch);
        uint64_t dt = rdtscp64() - t0;
        if (dt < sweepCycles) sweepCycles = dt;
    }
    printf("[cov-jsmap] baseline busy-wait = %lu cycles (~one JS sweep)\n",
           (unsigned long)sweepCycles);
    for (int b = 0; b < BASELINE_ROWS; b++) {
        uint16_t *row = &baseline[(size_t)b * numSets];
        for (int i = 0; i < numSets; i++)
            row[i] = e_setsA[i] ? probe_set(l3A, i, e_setsA[i], sweepCycles) : 0;
    }

    // 5. Write outputs. Every jsmap run gets its OWN knob-tagged tree so it never collides with
    // the mapping_B real-eviction-set trees (native / native_shuffled) or with another knob combo:
    //   shuffle=1 -> data/coverage/native_jsmap_shuffled_p{P}a{A}[_same][_buddy][_evA{A}D{D}C{C}]
    //   shuffle=0 -> data/coverage/native_jsmap_p{P}a{A}[_same][_buddy][_evA{A}D{D}C{C}]
    // "_same" = same-exact-address access pattern (vs different words); "_buddy" = 128B adjacent-line
    // reinforcement diagnostic; "_evA{A}D{D}C{C}" = Rowhammer.js eviction-strategy sweep (replaces the
    // pointer chase; passes/accesses/same/buddy are inert in that mode).
    const char *root = shuffle ? DATA_DIR "/native_jsmap_shuffled" : DATA_DIR "/native_jsmap";
    char evsuf[32] = "";
    if (ev_active(A, D, C)) snprintf(evsuf, sizeof(evsuf), "_evA%dD%dC%d", A, D, C);
    char baseDir[160];
    snprintf(baseDir, sizeof(baseDir), "%s_p%da%d%s%s%s", root, passes, accessesPerLine,
             sameAddr ? "_same" : "", buddyTouch ? "_buddy" : "", evsuf);
    char dir[256], missPath[300];
    snprintf(dir, sizeof(dir), "%s/NoC%02d", baseDir, noc);
    mkdir_p(dir);
    snprintf(missPath, sizeof(missPath), "%s/%03d.csv", dir, iterIdx);

    FILE *fm = fopen(missPath, "w");
    if (fm) {
        for (int i = 0; i < numSets; i++) fprintf(fm, "%sS%d", i ? "," : "", i);
        fprintf(fm, "\n");
        for (int c = 0; c < noc; c++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", matrix[(size_t)c * numSets + i]);
            fprintf(fm, "\n");
        }
        for (int b = 0; b < BASELINE_ROWS; b++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", baseline[(size_t)b * numSets + i]);
            fprintf(fm, "\n");
        }
        fclose(fm);
        printf("[cov-jsmap] wrote %s (%d clusters + %d baseline x %d sets)\n",
               missPath, noc, BASELINE_ROWS, numSets);
    } else {
        perror("fopen miss matrix");
    }

    char labelsPath[300];
    snprintf(labelsPath, sizeof(labelsPath), "%s/set_labels.csv", baseDir);
    FILE *fl = fopen(labelsPath, "w");
    if (fl && pas) {
        fprintf(fl, "set_idx,pa,bits8_11\n");
        for (int i = 0; i < numSets; i++) {
            uint64_t pa = pas[(size_t)i * assoc];      // first way's physical address
            fprintf(fl, "%d,0x%lx,%lu\n", i, pa, (pa >> 8) & 0xF);
        }
        fclose(fl);
        printf("[cov-jsmap] wrote %s\n", labelsPath);
    }

    free(matrix); free(baseline); free(pas);
    free_lazy_mapping(&map);
    l3_release(l3A);
    return 0;
}

int main(int argc, char **argv) {
    // Args: NoC, iteration index (output naming), mode {browser (default), native, jsmap}.
    //   browser: argv[4] = optional spin-window override (us).
    //   native : argv[4] = "shuffle" to shuffle each cluster's line order before
    //            measuring (prefetch-defeating A/B); default = unshuffled.
    //   jsmap  : argv[4] = "shuffle" for the JS-faithful (page-shuffled) victim ->
    //            data/coverage/native_shuffled; default (unshuffled pages) ->
    //            data/coverage/native_jsmap. argv[5]=passes (full sweeps/probe; default 1),
    //            argv[6]=accesses per line (default 3), argv[7]="same" to repeat the EXACT
    //            same address (vs different words; default "words"), argv[8]="buddy" to also
    //            demand-access each line's 128B buddy (reinforcement diagnostic; default off),
    //            argv[9..11]=eviction-strategy params A D C (default 1 1 1 = off): when any >1,
    //            the victim uses the Rowhammer.js sliding-window sweep (sweep_lazy_evict) instead
    //            of the pointer chase, and passes/accesses/same/buddy become inert.
    //            Every jsmap run writes its own knob-tagged tree, e.g.
    //            native_jsmap_shuffled_p1a3_buddy or native_jsmap_shuffled_p1a1_evA2D4C1.
    int noc     = (argc > 1) ? atoi(argv[1]) : DEFAULT_NOC;
    int iterIdx = (argc > 2) ? atoi(argv[2]) : 0;
    const char *mode = (argc > 3) ? argv[3] : "browser";
    if (!is_pow2_le64(noc)) {
        fprintf(stderr, "NoC must be a power of two in [1,64], got %d\n", noc);
        return 1;
    }

    if (strcmp(mode, "native") == 0) {
        int shuffle = (argc > 4) && strcmp(argv[4], "shuffle") == 0;
        return run_native_experiment(noc, iterIdx, shuffle);
    }
    if (strcmp(mode, "jsmap") == 0) {
        int shuffle = (argc > 4) && strcmp(argv[4], "shuffle") == 0;
        // Replacement-policy knobs: argv[5]=passes, argv[6]=accesses/line, argv[7]=pattern
        // ("same" = repeat the exact same address; anything else = different words in the line).
        int passes          = (argc > 5) ? atoi(argv[5]) : 1;
        int accessesPerLine = (argc > 6) ? atoi(argv[6]) : 3;
        int sameAddr        = (argc > 7) && strcmp(argv[7], "same") == 0;
        int buddyTouch      = (argc > 8) && strcmp(argv[8], "buddy") == 0;
        // Eviction-strategy params (argv[9..11], default 1 = identity/off): A=window repeats,
        // D=sliding window size, C=step. Any >1 selects the sweep_lazy_evict access pattern.
        int A = (argc > 9)  ? atoi(argv[9])  : 1;
        int D = (argc > 10) ? atoi(argv[10]) : 1;
        int C = (argc > 11) ? atoi(argv[11]) : 1;
        if (passes < 1) passes = 1;
        if (accessesPerLine < 1) accessesPerLine = 1;
        if (A < 1) A = 1;
        if (D < 1) D = 1;
        if (C < 1) C = 1;
        // The 16-word cap only applies to the "words" pattern; same-address can repeat freely.
        if (!sameAddr && accessesPerLine > JS_ELEMS_PER_LINE) accessesPerLine = JS_ELEMS_PER_LINE;
        return run_native_jsmap_experiment(noc, iterIdx, shuffle, passes, accessesPerLine, sameAddr, buddyTouch, A, D, C);
    }
    if (strcmp(mode, "browser") != 0) {
        fprintf(stderr, "Unknown mode '%s' (use 'browser', 'native', or 'jsmap')\n", mode);
        return 1;
    }

    // Prime->probe spin window (us): must exceed one full JS cluster sweep so the
    // continuous hammer deposits a whole eviction set. Auto-scaled ~1/NoC; argv[4] overrides.
    int spinUs = (argc > 4) ? atoi(argv[4]) : (SPIN_US_AT_16 * 16 / noc);
    return run_browser_experiment(noc, iterIdx, spinUs);
}
