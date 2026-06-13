# Lazy Mapping + Cluster Sweep — Pseudocode (JS-style)

Mirrors the browser implementation in `main.js`. Two parts:
**(A) construction** of the spatial clusters, **(B) the timed sweep** that produces a
memorygram cell.

---

## Parameters

```
# Experiment knobs (parsed from URL label {workload}_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles)
NoC                 # number of clusters (power of two, 1..64)
TST_ms              # total trace duration  (TST seconds * 1000)
K                   # accesses between two clock polls
CYCLES_PER_ADDRESS  # est. CPU cycles to touch one address in the chase

# Per-machine hardware constants (NOT in the label)
LLC_WAYS            # associativity (e.g. 12)
LLC_SETS            # number of physical sets (e.g. 16384); multiple of 64 and of NoC
CLOCK_SPEED         # CPU clock in Hz (e.g. 3.6e9)
MIN_QUANTUM_MS = 0.5

# Fixed memory geometry
BYTES_PER_LINE = 64
BYTES_PER_PAGE = 4096
BYTES_PER_ELEM = 4              # Uint32
ELEMS_PER_LINE = 16            # 64 / 4
ELEMS_PER_PAGE = 1024          # 4096 / 4
LINES_PER_PAGE = 64           # the 64 possible values of address bits 6..11
```

### Derived timing

```
function computeQuantumMs(NoC, LLC_SETS, LLC_WAYS, CYCLES_PER_ADDRESS):
    setsPerCluster = LLC_SETS / NoC
    qCycles        = CYCLES_PER_ADDRESS * setsPerCluster * LLC_WAYS
    qMs            = (qCycles / CLOCK_SPEED) * 1000
    return max(qMs, MIN_QUANTUM_MS)          # floor at 500 us

Q = computeQuantumMs(...)                    # ms budget to sweep ONE cluster
T = floor(TST_ms / (Q * NoC))                # number of time slots (memorygram rows)
```

---

## (A) Construction — `LazyMapping(NoC, LLC_SETS, LLC_WAYS)`

Key idea: a large typed array is page-aligned by V8, so byte-offset bits 6..11 equal the
**translation-invariant** page-offset bits of the physical address. Cluster membership is
chosen from those bits alone — no virtual/physical address knowledge needed.

```
guard: NoC is a power of two and 1 <= NoC <= 64
guard: LLC_SETS % 64 == 0  and  LLC_SETS % NoC == 0

totalLines  = LLC_SETS * LLC_WAYS
bufferBytes = totalLines * BYTES_PER_LINE
buffer      = new Uint32Array(bufferBytes / BYTES_PER_ELEM)   # page-aligned (V8 large alloc)
numPages    = bufferBytes / BYTES_PER_PAGE

# C-equivalent address-based clustering: cluster = (addr >> (12 - log2 NoC)) & (NoC - 1)
log2NoC    = round(log2(NoC))
shiftRight = 12 - log2NoC
andTarget  = NoC - 1

evSetsPerBitValue = numPages / LLC_WAYS        # = LLC_SETS / 64
clusterNodes[0..NoC-1] = empty lists           # element indices, per cluster

# Build LLC_SETS synthetic eviction sets (WAYS lines each) and bucket them into clusters.
for v in 0 .. LINES_PER_PAGE-1:                # v = bits 6..11 value
    cluster = ((v * BYTES_PER_LINE) >> shiftRight) & andTarget   # page term contributes 0

    pages = shuffle([0, 1, ..., numPages-1])   # Fisher-Yates -> non-strided chase (anti-prefetch)

    for s in 0 .. evSetsPerBitValue-1:         # one synthetic eviction set
        for w in 0 .. LLC_WAYS-1:              # WAYS lines, all sharing bits 6..11 = v
            page    = pages[s*LLC_WAYS + w]
            elemIdx = page*ELEMS_PER_PAGE + v*ELEMS_PER_LINE     # head element of that line
            clusterNodes[cluster].push(elemIdx)
        evictionSetCounts[cluster] += 1

# Concatenate each cluster's lines into ONE circular pointer-chase list, stored in-buffer.
for c in 0 .. NoC-1:
    nodes = clusterNodes[c]
    for i in 0 .. len(nodes)-1:
        buffer[nodes[i]] = nodes[(i + 1) mod len(nodes)]   # node value = index of next node
    heads[c]       = nodes[0]
    nodeCounts[c]  = len(nodes)                # lines per cluster = LLC_SETS*LLC_WAYS/NoC
```

Resulting invariants (per cluster): `nodeCounts[c] = totalLines / NoC`,
`evictionSetCounts[c] = LLC_SETS / NoC`. Union of all clusters covers every buffer line
exactly once (disjoint partition).

---

## (B) Timed sweep — `sweepCluster(c, Q)`

`curr = buffer[curr]` is a single dependent load that BOTH reads the cache line (the probe)
AND yields the next node. The inner K-loop amortizes the clock poll and removes the
per-access modulo; the chase is latency-bound so loop overhead hides under memory latency.

```
function sweepCluster(c, Q):
    curr   = heads[c]
    count  = 0
    finish = now() + Q                         # now() = performance.now(), ms
    repeat:
        for k in 0 .. K-1:
            curr = buffer[curr]                # probe line + advance (one dependent load)
        count += K
    until now() >= finish
    sink = curr                                # observe curr -> prevents dead-code elimination
    return count                               # higher count = faster sweep = less contention
```

---

## Memorygram

```
function sampleMemorygram():
    map = LazyMapping(NoC, LLC_SETS, LLC_WAYS)

    spin until now() ticks                      # align trace start to a fresh timer edge

    for t in 0 .. T-1:                          # one row per time slot
        for c in 0 .. NoC-1:                    # one full sweep over all clusters = one slot
            memorygram[t][c] = map.sweepCluster(c, Q)
    return memorygram                           # T x NoC matrix -> CSV (header G0..G{NoC-1})
```
