// lazy_map.c
// -----------------------------------------------------------------------------
// C port of the JS main.js LazyMapping victim. Instead of loading a saved Mastik
// mapping and clustering it, this builds the victim exactly as the browser does: a
// fresh page-aligned mmap buffer partitioned by translation-invariant bits 6-11,
// with per-bit-value shuffled pages. The mmap buffer is a SEPARATE virtual allocation;
// contention still lands on the real physical LLC sets because the LLC is physically
// indexed and lazy clusters share bits 6-11.
//
// Extracted from coverage_validator.c so both CoverageValidator (native jsmap coverage
// experiment) and MastikElite (stress-ng fingerprinting, timer_mode==2) share one copy.
// -----------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>   // mmap
#include <math.h>       // log2, round

#include "utils.h"      // maccessMy
#include "lazy_map.h"

static uint32_t g_lazy_sink;  // observed after each sweep to defeat dead-code elimination

// Fisher-Yates shuffle of an int array (JS main.js shuffle()).
static void shuffle_int(int *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    }
}

// Port of JS LazyMapping.build(). When shufflePages is 0 the pages are used in order
// (strided; prefetch A/B). Returns 0 on success.
int build_lazy_mapping(LazyMap *m, int noc, int llcSets, int llcWays, int shufflePages) {
    memset(m, 0, sizeof(*m));
    m->numClusters = noc;
    m->bytes = (size_t)llcSets * llcWays * JS_BYTES_PER_LINE;

    m->buf = mmap(NULL, m->bytes, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->buf == MAP_FAILED) { perror("mmap lazy buffer"); m->buf = NULL; return 1; }

    const int numPages         = (int)(m->bytes / JS_BYTES_PER_PAGE);
    const int shiftRight        = 12 - (int)round(log2((double)noc));
    const int andTarget         = noc - 1;
    const int evSetsPerBitValue = numPages / llcWays;
    const int nodesPerCluster   = (llcSets * llcWays) / noc;

    m->heads      = malloc((size_t)noc * sizeof(uint32_t));
    m->nodeCounts = malloc((size_t)noc * sizeof(int));
    uint32_t **clusterNodes = malloc((size_t)noc * sizeof(uint32_t *));
    int *fill = calloc((size_t)noc, sizeof(int));
    int *pages = malloc((size_t)numPages * sizeof(int));
    if (!m->heads || !m->nodeCounts || !clusterNodes || !fill || !pages) {
        perror("malloc lazy mapping"); return 1;
    }
    for (int c = 0; c < noc; c++)
        clusterNodes[c] = malloc((size_t)nodesPerCluster * sizeof(uint32_t));

    for (int v = 0; v < JS_LINES_PER_PAGE; v++) {
        int cluster = ((v * JS_BYTES_PER_LINE) >> shiftRight) & andTarget;

        for (int p = 0; p < numPages; p++) pages[p] = p;
        if (shufflePages) shuffle_int(pages, numPages);

        for (int s = 0; s < evSetsPerBitValue; s++) {
            for (int w = 0; w < llcWays; w++) {
                int page = pages[s * llcWays + w];
                uint32_t node = (uint32_t)(page * JS_ELEMS_PER_PAGE + v * JS_ELEMS_PER_LINE);
                clusterNodes[cluster][fill[cluster]++] = node;
            }
        }
    }

    for (int c = 0; c < noc; c++) {
        int len = fill[c];
        for (int i = 0; i < len; i++)
            m->buf[clusterNodes[c][i]] = clusterNodes[c][(i + 1) % len];  // circular link
        m->heads[c] = clusterNodes[c][0];
        m->nodeCounts[c] = len;
        free(clusterNodes[c]);
    }
    free(clusterNodes); free(fill); free(pages);
    return 0;
}

void free_lazy_mapping(LazyMap *m) {
    if (m->buf && m->buf != MAP_FAILED) munmap(m->buf, m->bytes);
    free(m->heads); free(m->nodeCounts);
    m->buf = NULL; m->heads = NULL; m->nodeCounts = NULL;
}

// Sweep cluster c's circular list (JS main.js hammerCluster) with replacement-policy
// experiment knobs (defaults reproduce the plain JS behavior):
//   accessesPerLine - accesses to each node per visit (offset 0 is the chase link; the
//                     extra accessesPerLine-1 hits stay on the SAME 64B line). Tests the
//                     adaptive-insertion hypothesis (Gruss'16, Qureshi'07, Jaleel'10):
//                     repeatedly hitting a line to raise its insertion/retention probability
//                     so the attacker line actually seats and evicts the victim.
//   sameAddr        - 0: extra accesses stride across the line's words (curr+1..curr+r).
//                     1: extra accesses hit the EXACT SAME address (curr) each time, via the
//                     volatile maccessMy load (prevents compiler elision; HW may still
//                     coalesce identical loads, so this can issue fewer real lookups).
//   passes          - full traversals of the cluster per probe (interleaved re-access:
//                     each line is revisited only after the rest of the cluster is touched).
void sweep_lazy_once(const LazyMap *m, int c, int passes, int accessesPerLine,
                     int sameAddr) {
    const uint32_t *buf = m->buf;
    int n = m->nodeCounts[c];
    uint32_t curr = m->heads[c];
    uint64_t sink = 0;
    for (int p = 0; p < passes; p++) {
        for (int i = 0; i < n; i++) {
            uint32_t next = buf[curr];               // 1st access (offset 0 = chase link)
            if (sameAddr) {
                for (int r = 1; r < accessesPerLine; r++)
                    maccessMy((void *)(buf + curr));  // SAME exact address, repeated
            } else {
                for (int r = 1; r < accessesPerLine; r++)
                    sink += buf[curr + r];            // different words in the SAME 64B line
            }
            curr = next;
        }
    }
    g_lazy_sink = curr + (uint32_t)sink;
}
