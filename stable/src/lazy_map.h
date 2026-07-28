#ifndef LAZY_MAP_H
#define LAZY_MAP_H

#include <stddef.h>
#include <stdint.h>

// ---- JS-faithful lazy-map victim: a C port of the JS main.js LazyMapping ----
// Builds the victim exactly as the browser does: a fresh page-aligned mmap buffer
// partitioned by translation-invariant bits 6-11, with per-bit-value shuffled pages.
// Shared by CoverageValidator (native jsmap coverage experiment) and MastikElite
// (stress-ng fingerprinting under the Chrome mock clock, timer_mode==2).

// JS geometry / constants (main.js). Hard-coded per-CPU, as in JS (i7-9700k).
#define JS_LLC_SETS       16384          // 2^14 (2048 sets/slice x 8 slices)
#define JS_LLC_WAYS       12             // associativity
#define JS_BYTES_PER_LINE 64
#define JS_BYTES_PER_PAGE 4096
#define JS_ELEMS_PER_PAGE (JS_BYTES_PER_PAGE / 4)   // 1024 (uint32 elements per page)
#define JS_ELEMS_PER_LINE (JS_BYTES_PER_LINE / 4)   // 16
#define JS_LINES_PER_PAGE (JS_BYTES_PER_PAGE / JS_BYTES_PER_LINE)  // 64 (bit 6-11 values)

typedef struct {
    uint32_t *buf;         // mmap'd, page-aligned; node values are 32-bit ELEMENT indices
    uint32_t *heads;       // per-cluster head element index
    int      *nodeCounts;  // lines per cluster
    uint32_t **clusterNodes; // per-cluster array of node (line-head) element indices, in ring order.
                             // Retained (not just linked) so eviction-strategy sweeps can INDEX nodes
                             // (buf[clusterNodes[c][i]]) instead of only pointer-chasing. clusterNodes[c][i]
                             // links to clusterNodes[c][(i+1)%nodeCounts[c]] in buf (see build_lazy_mapping).
    int       numClusters;
    size_t    bytes;       // buffer size, for munmap
} LazyMap;

// Port of JS LazyMapping.build(). When shufflePages is 0 the pages are used in order
// (strided; prefetch A/B). Returns 0 on success.
// sizeMult scales the mmap buffer to sizeMult LLC capacities (1 = 12 MB = mean 12 lines/set).
int build_lazy_mapping(LazyMap *m, int noc, int llcSets, int llcWays, int shufflePages, int sizeMult);

void free_lazy_mapping(LazyMap *m);

// Sweep cluster c's circular list (JS main.js hammerCluster) with replacement-policy
// experiment knobs (defaults passes=1, accessesPerLine=1, sameAddr=0, buddyTouch=0 reproduce
// plain JS behavior). See lazy_map.c for the semantics of each knob.
//   buddyTouch - 1: also issue a demand access to each line's 128B buddy
//                (curr ^ JS_ELEMS_PER_LINE), injecting immediate reinforcement.
//                Default 0 = plain JS behaviour.
void sweep_lazy_once(const LazyMap *m, int c, int passes, int accessesPerLine, int sameAddr,
                     int buddyTouch);

// ---- minimal decoy scratch pool (between-subcluster-window causality test) ----
// A small, independent buffer sharing no pages with the victim/prober buffers. Used by
// sweep_lazy_evict to inject a controllable, disjoint-set "dose" of unrelated demand
// accesses between the sliding-window sweep's subcluster bursts. Because the decoy lines
// are on a completely separate allocation, they cannot alias the monitored real set by
// construction -- any resulting change in coverage can't be direct/local interference, only
// a cache-wide replacement-state effect (e.g. DRRIP-style leader-set dueling).
typedef struct {
    uint32_t *buf;      // mmap'd scratch buffer (uint32 elements, like LazyMap.buf)
    int       numLines; // distinct 64B lines available to sample from
} DecoyBuf;

// Allocate and fault in a `bytes`-sized decoy pool (faulted up front, outside any timed
// region). Returns 0 on success.
int  build_decoy(DecoyBuf *d, size_t bytes);
void free_decoy(DecoyBuf *d);

// Eviction-STRATEGY sweep of cluster c (coverage experiment only): a Rowhammer.js-style
// sliding-window access pattern over clusterNodes[c] that aims to beat the L3 scan-resistant
// insertion policy (promote our lines, demote/evict the victim's primed lines). Parameters
// (Gruss et al., Rowhammer.js, DIMVA'16): window D slides across the n nodes in steps of C,
// and each window is accessed A times:
//     for (s=0; s+D<=n; s+=C) for (a=0; a<A; a++) for (d=0; d<D; d++) touch(nodes[s+d]);
// A=1,D=1,C=1 is exactly the single linear pass (identity). Uses indexed access (JS-portable to
// a Uint32Array), NOT the pointer chase. Does not touch the timed fingerprinting sampler.
//
// decoy/decoyLines (optional; NULL/0 = off, byte-for-byte the original behavior): after
// EVERY window -- including the last, i.e. right before the caller's probe -- lfence (so the
// window's loads fully retire), touch `decoyLines` random lines from `decoy`, lfence again
// (so the decoy retires before the next window's loads can issue). Tests how much disjoint-set
// "noise" between subcluster bursts is needed to move coverage, without assuming it has to be
// large: decoyLines=0 reproduces the plain sweep_lazy_evict exactly.
void sweep_lazy_evict(const LazyMap *m, int c, int A, int D, int C,
                      const DecoyBuf *decoy, int decoyLines);

// Flush every line of cluster c from all cache levels (clflush over clusterNodes[c]).
// Used by the self-eviction PMU experiment to force a cold start before a warm/measured
// sweep pair, so the measured pass's demand L3 misses reflect only the sweep's own
// (self-)eviction, not stale residency from a prior cluster.
void flush_lazy_cluster(const LazyMap *m, int c);

#endif // LAZY_MAP_H
