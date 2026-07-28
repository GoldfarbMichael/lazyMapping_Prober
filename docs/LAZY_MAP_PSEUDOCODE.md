# Lazy Mapping Pseudocode

Reference: [stable/src/lazy_map.c](../stable/src/lazy_map.c)

Two algorithms are documented:

1. **JS-parity Lazy Mapping construction** — `build_lazy_mapping()`
2. **A/D/C sliding-window eviction sweep** — `sweep_lazy_evict()`

Both operate on a single page-aligned `mmap` buffer of `LLC_SETS * LLC_WAYS * BYTES_PER_LINE`
bytes. The buffer is a separate virtual allocation; contention still lands on the real physical
LLC sets because the LLC is physically indexed and lazy clusters share the translation-invariant
cache-index bits 6–11 (within the page offset, so identical VA→PA).

---

## 1) JS-parity Lazy Mapping (`build_lazy_mapping`)

Partitions the buffer into `NoC` clusters keyed on translation-invariant bits 6–11, one circular
pointer-chase list per cluster. This is the C port of the browser `LazyMapping.build()`.

```
INPUT:  noc          number of clusters (NoC)   -- expected power of 2
        llcSets      LLC sets
        llcWays      LLC ways (associativity)
        shufflePages 1 = Fisher-Yates shuffle pages per bit-value (JS default)
                     0 = pages used in order (strided; prefetch A/B)

CONSTANTS (from geometry):
        BYTES_PER_LINE, BYTES_PER_PAGE, LINES_PER_PAGE,
        ELEMS_PER_PAGE, ELEMS_PER_LINE

DERIVED:
        bytes            = llcSets * llcWays * BYTES_PER_LINE
        numPages         = bytes / BYTES_PER_PAGE
        shiftRight       = 12 - log2(noc)          -- select which index bits pick the cluster
        andTarget        = noc - 1                 -- mask to noc buckets
        evSetsPerBitValue= numPages / llcWays      -- nominal eviction sets per set-index-bit value (per line-slot v)
        nodesPerCluster  = (llcSets * llcWays) / noc

ALLOCATE:
        buf          <- mmap(bytes, PRIVATE|ANONYMOUS)     -- fail -> return error
        clusterNodes[noc][nodesPerCluster]                 -- RETAINED (indexed sweep uses it)
        fill[noc]    <- 0                                  -- per-cluster write cursor

# --- assign nodes to clusters ---
# v = line-slot within a page (0..63). Its byte offset v*64 populates set-index bits 6-11
#     (NOT the line offset, bits 0-5, which never touch the set index).
FOR each line-slot v in 0 .. LINES_PER_PAGE-1:
        # cluster chosen from the page-resident set-index bits (6-11) only -- these survive VA->PA
        cluster = ((v * BYTES_PER_LINE) >> shiftRight) & andTarget

        pages <- [0, 1, ..., numPages-1]
        IF shufflePages: Fisher-Yates shuffle(pages)

        FOR s in 0 .. evSetsPerBitValue-1:          # each group of llcWays pages = one nominal eviction set
            FOR w in 0 .. llcWays-1:
                page = pages[s*llcWays + w]
                node = page*ELEMS_PER_PAGE + v*ELEMS_PER_LINE   # word index into buf
                clusterNodes[cluster][ fill[cluster]++ ] = node

# --- link each cluster into a circular pointer-chase list ---
FOR each cluster c in 0 .. noc-1:
        len = fill[c]
        FOR i in 0 .. len-1:
            buf[ clusterNodes[c][i] ] = clusterNodes[c][ (i+1) mod len ]   # store next-node index
        heads[c]      = clusterNodes[c][0]
        nodeCounts[c] = len

# clusterNodes is kept alive for the indexed A/D/C sweep; freed in free_lazy_mapping().
RETURN success
```

**Key invariants**

- The cluster id depends only on `v`, the **line-slot within a page** (0–63), i.e. on set-index
  **bits 6–11** — the page-resident portion of the set index, which survives VA→PA translation.
  This is *not* the line offset (bits 0–5), which never affects the set index. Page number `p`
  only spreads nodes across the higher set-index bits (12+) / slices; it never changes cluster
  membership.
- Because bits 12+ of the set index are left free by `v`, same-`v` lines scatter across many
  physical sets on any LLC with >64 sets. A group of `llcWays` same-`v` pages is therefore a
  **nominal** eviction set (the Lazy-Mapping assumption), not a physically guaranteed one.
- Each cluster holds exactly `nodesPerCluster = (llcSets*llcWays)/noc` nodes.
- Two representations coexist: a **circular linked list** (`heads`, for the pointer-chase
  `sweep_lazy_once`) and the **flat index array** `clusterNodes[c]` (for the indexed
  `sweep_lazy_evict`).

---

## 2) A/D/C sliding-window eviction sweep (`sweep_lazy_evict`)

Rowhammer.js-style sliding-window access pattern over one cluster's retained node array. Uses
**indexed access** `buf[nodes[idx]]` (not the pointer chase) so the memory pattern matches exactly
what a JS `Uint32Array` victim would execute. `A = D = C = 1` degenerates to a single linear pass.

```
INPUT:  m      lazy map
        c      cluster id
        A      # times to hammer each window position   (Accesses / repeats)
        D      window length in nodes                    (Distance / window size)
        C      slide step in nodes                        (Count / stride)

nodes = m.clusterNodes[c]          # flat index array for cluster c
n     = m.nodeCounts[c]            # nodes in this cluster
buf   = m.buf

# clamp parameters into valid range
A = max(A, 1)
C = max(C, 1)
D = clamp(D, 1, n)                 # window cannot exceed the cluster

sink = 0
FOR s = 0; s + D <= n; s += C:     # slide a window of D nodes, stepping by C
    FOR a = 0 .. A-1:              # hammer the whole window A times
        FOR d = 0 .. D-1:          # touch each of the D lines in the window
            sink += buf[ nodes[s + d] ]

g_lazy_sink += sink                # consumed globally to defeat dead-code elimination
```

**Semantics of A, D, C**

| Param | Meaning | Effect |
|-------|---------|--------|
| `A` | repeats per window | Reinforces insertion/retention of the window's lines before sliding on (adaptive-insertion pressure). |
| `D` | window length (nodes) | How many distinct lines are co-resident under pressure at once. `D >= llcWays` is needed to force an eviction within a set. |
| `C` | slide stride (nodes) | Overlap between successive windows: `C < D` overlaps, `C = D` tiles disjointly, `C = 1` maximally overlaps. |

**Loop structure**: outer = window start `s` advancing by `C`; middle = `A` hammer passes;
inner = linear scan of the `D`-node window. Total accesses ≈ `A * D * floor((n - D)/C + 1)`.

**Degenerate cases**

- `A = D = C = 1` → one access per node, single linear pass (identity sweep).
- `D = n`, `C >= 1` → single window covering the whole cluster, hammered `A` times.
- `C = D` → non-overlapping tiles of the cluster.
