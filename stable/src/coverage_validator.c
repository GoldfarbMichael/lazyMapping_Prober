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

#include <mastik/l3.h>
#include <mastik/low.h>   // rdtscp64
#include "utils.h"        // virt_to_phys, load_physical_mapping
#include "mastikElite.h"  // pin_to_core, load_mapping_and_eSetsFrom_BIN_file

#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define MAPPING_FILE_A  "mapping_A.bin"

#define PROBER_CORE   0
#define BROWSER_CORE  1

#define MASTIK_GROUPS 16          // Mastik partitions all sets into 16 groups
#define DEFAULT_NOC   16          // start by validating NoC = 16

#define REPS          3           // probes per set, take the median
#define DELAY_CYCLES  150000ULL   // ~40us @3.6GHz: let JS sweep its cluster between prime/probe
#define RAMP_MS       400         // wait after switching cluster so JS is hammering it

#define CTL_IDLE  (-1)
#define CTL_STOP  (-2)

#define CHROME_PROFILE "/tmp/chrome-validate"
#define SERVER_PORT 8080

// ---- tiny HTTP POST /ctl/set {"cluster":N} (raw socket, no libcurl) ----
static void ctl_set(int cluster) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect /ctl/set"); close(fd); return;
    }
    char body[64];
    int blen = snprintf(body, sizeof(body), "{\"cluster\":%d}", cluster);
    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "POST /ctl/set HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n%s", SERVER_PORT, blen, body);
    if (write(fd, req, rlen) < 0) perror("write /ctl/set");
    char buf[256];
    while (read(fd, buf, sizeof(buf)) > 0) { /* drain + wait for response */ }
    close(fd);
}

static inline void spin_cycles(uint64_t cycles) {
    uint64_t t0 = rdtscp64();
    while (rdtscp64() - t0 < cycles) { /* busy wait */ }
}

static int cmp_u16(const void *a, const void *b) {
    return (int)(*(const uint16_t *)a) - (int)(*(const uint16_t *)b);
}

// Address-by-address combined prime+probe of ONE set (get_transTable pattern):
// monitor just this set, warm-prime it, then probe after a delay during which
// JS (other core) may evict it. Returns the median miss count over REPS.
static uint16_t probe_set(l3pp_t l3, int setIdx, void *head) {
    uint16_t res[4];
    uint16_t vals[REPS];
    l3_unmonitorall(l3);
    l3_monitor_manual(l3, setIdx, head);
    l3_bprobecount(l3, res);                 // warm prime (backward fill)
    for (int r = 0; r < REPS; r++) {
        spin_cycles(DELAY_CYCLES);           // let the JS hammer evict
        l3_bprobecount(l3, res);             // probe + re-prime; res[0] = misses
        vals[r] = res[0];
    }
    qsort(vals, REPS, sizeof(uint16_t), cmp_u16);
    return vals[REPS / 2];
}

// Scan every set once while the currently-set cluster is hammered.
static void scan_all_sets(l3pp_t l3, void **e_sets, int numSets, uint16_t *out) {
    for (int i = 0; i < numSets; i++) {
        out[i] = e_sets[i] ? probe_set(l3, i, e_sets[i]) : 0;
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
        execlp("google-chrome", "google-chrome",
               "--user-data-dir=" CHROME_PROFILE,
               "--no-first-run", "--no-default-browser-check",
               "--new-window", url, (char *)NULL);
        perror("execlp google-chrome");
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv) {
    int noc = (argc > 1) ? atoi(argv[1]) : DEFAULT_NOC;

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
    int setsPerGroup = numSets / MASTIK_GROUPS;
    printf("[cov] numSets=%d assoc=%d NoC=%d MastikGroups=%d setsPerGroup=%d\n",
           numSets, assoc, noc, MASTIK_GROUPS, setsPerGroup);

    // Physical addresses (for bits 8-11 labelling, apples-to-apples with clusters).
    int paCount = 0;
    uint64_t *pas = load_physical_mapping(MAPPING_FILE_A, &paCount);

    // 2. Launch the browser victim.
    pid_t chrome = launch_chrome(noc);
    printf("[cov] launched chrome (pid %d); waiting for page to load...\n", chrome);
    sleep(6);

    // 3. Per-cluster scan.
    uint16_t *matrix   = calloc((size_t)noc * numSets, sizeof(uint16_t));
    uint16_t *baseline = calloc(numSets, sizeof(uint16_t));

    for (int c = 0; c < noc; c++) {
        ctl_set(c);
        usleep(RAMP_MS * 1000);
        printf("[cov] scanning sets while JS hammers cluster %d ...\n", c);
        scan_all_sets(l3, e_sets, numSets, &matrix[(size_t)c * numSets]);
        int g = get_active_group(&matrix[(size_t)c * numSets], setsPerGroup, numSets, assoc);
        printf("[cov] cluster %d -> dominant Mastik group %d\n", c, g);
    }

    // 4. Baseline (JS idle) for the noise floor.
    ctl_set(CTL_IDLE);
    usleep(RAMP_MS * 1000);
    printf("[cov] baseline scan (JS idle)...\n");
    scan_all_sets(l3, e_sets, numSets, baseline);

    // 5. Stop JS and tear down chrome.
    ctl_set(CTL_STOP);
    if (chrome > 0) kill(chrome, SIGTERM);

    // 6. Write outputs.
    FILE *fm = fopen("coverage_miss_matrix.csv", "w");
    if (fm) {
        for (int i = 0; i < numSets; i++) fprintf(fm, "%sS%d", i ? "," : "", i);
        fprintf(fm, "\n");
        for (int c = 0; c < noc; c++) {
            for (int i = 0; i < numSets; i++)
                fprintf(fm, "%s%u", i ? "," : "", matrix[(size_t)c * numSets + i]);
            fprintf(fm, "\n");
        }
        for (int i = 0; i < numSets; i++) fprintf(fm, "%s%u", i ? "," : "", baseline[i]);
        fprintf(fm, "\n");
        fclose(fm);
        printf("[cov] wrote coverage_miss_matrix.csv (%d clusters + baseline x %d sets)\n", noc, numSets);
    }

    FILE *fl = fopen("coverage_set_labels.csv", "w");
    if (fl && pas) {
        fprintf(fl, "set_idx,pa,bits8_11\n");
        for (int i = 0; i < numSets; i++) {
            uint64_t pa = pas[(size_t)i * assoc];      // first way's physical address
            fprintf(fl, "%d,0x%lx,%lu\n", i, pa, (pa >> 8) & 0xF);
        }
        fclose(fl);
        printf("[cov] wrote coverage_set_labels.csv\n");
    }

    free(matrix); free(baseline); free(pas);
    l3_release(l3);
    return 0;
}
