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

#include <mastik/l3.h>
#include <mastik/low.h>   // rdtscp64
#include "utils.h"        // virt_to_phys, load_physical_mapping
#include "mastikElite.h"  // pin_to_core, load_mapping_and_eSetsFrom_BIN_file

#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define MAPPING_FILE_A  "mapping_A.bin"

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

#define DATA_DIR      "data/coverage"   // outputs go under stable/data/coverage/
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

int main(int argc, char **argv) {
    // Args: NoC, iteration index (for output naming), and an optional spin override.
    int noc     = (argc > 1) ? atoi(argv[1]) : DEFAULT_NOC;
    int iterIdx = (argc > 2) ? atoi(argv[2]) : 0;
    if (!is_pow2_le64(noc)) {
        fprintf(stderr, "NoC must be a power of two in [1,64], got %d\n", noc);
        return 1;
    }
    // Prime->probe spin window (us): must exceed one full JS cluster sweep so the
    // continuous hammer deposits a whole eviction set. Auto-scaled ~1/NoC; argv[3] overrides.
    int spinUs = (argc > 3) ? atoi(argv[3]) : (SPIN_US_AT_16 * 16 / noc);
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
