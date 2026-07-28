# Handoff: add a JS-style lazy-map victim to the C stress-ng fingerprinting (Chrome mock clock)

> Purpose: this file is a self-contained brief for a **fresh Claude instance** to implement the
> next experiment. Everything below is precise and current as of 2026-07-03.

## Objective

**Add** a new, selectable victim option to the existing **C stress-ng fingerprinting** pipeline
(Chrome-mock-clock timing): a **JS-style lazy mapping built in C** (mmap + bits-6-11 partition +
shuffled pages). This is **strictly additive** — the current Mastik-loaded-eviction-set path stays
fully intact and remains the **default**. The JS-style victim is a *new* option, chosen via a new
flag / victim-source parameter. Everything else (stress-ng orchestration, mock timer, 5-fold RF
classifier, CSV format) stays identical, so it is a clean A/B against the existing result.

**Do NOT swap or rewrite existing code. Only add.**

## Reasoning (why we are doing this — the evidence chain)

1. **C native + loaded Mastik eviction sets + Chrome mock clock → ~99.8% avg CV accuracy.**
   But **real-Chrome (JS) fingerprinting maxes out at ~90%.** Large, unexplained gap.
2. **Coverage on real Chrome:** coverage **drops as NoC increases**. This is backwards from the
   premise — higher NoC = finer spatial resolution, so accuracy should rise. Instead accuracy
   **peaks around NoC=4 then dips**.
3. **Coverage on the Mastik-loaded e_sets (C):** coverage does **not** drop with NoC (although
   sweeping cluster `c` also evicts the *adjacent / next* cluster — a forward hardware-prefetch
   adjacency artifact; not the concern here).
4. **A C port of the JS lazy mapping** (built exactly like the browser's `LazyMapping`) behaves
   **like real Chrome**: coverage **drops as NoC rises**, though **less drastically** than real
   Chrome.
5. **Hypothesis to test:** the *lazy-mapping construction itself* — synthetic groups that share
   only the translation-invariant bits 6-11, which are **not** guaranteed to be real physical
   eviction sets — not the timing, is what degrades fingerprinting at higher NoC. Running the
   stress-ng fingerprinting with the **JS-style lazy map in C under the mock clock** isolates this.
   If accuracy now behaves like real Chrome (peaks ~NoC=4, then dips) instead of the flat ~99.8%,
   the lazy-mapping construction is confirmed as the cause of the real-Chrome accuracy drop.

## Current architecture (what already works — leave intact)

Repo: `/home/ubu/Desktop/Michael/lazyMapping_Prober/stable`

- **Fingerprinting entry:** `runStressNG_batches(tst_sec, batch_size, start_iteration, output_dir,
  backing_file, BIN_file, timer_mode)` in `src/mastikElite.c`.
  `timer_mode` 0 = native `rdtscp64`, 1 = Chrome mock clock, **Create a new entry --> 2 = Chrome mock clock js style mapping** (use this one).
  - Loads the Mastik mapping via `load_mapping_and_eSetsFrom_BIN_file`, then
    `eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), NoC)` builds a `Clusters_t`
    (for NoC ≤ 64: address bits 6-11 grouping).
  - Forks `stress-ng` on core 1, samples on core 0, writes
    `data/chrome_clock/<output_dir>/<stressor>/<iter>.csv`.
- **Mock-clock memorygram:** `get_spatioTemporal_memoryGram_ChromeMock(Clusters, NoC, TST_cycles,
  SST_cycles, matrix, filename)` (`src/mastikElite.c:499`). For each time slot × cluster it
  pointer-chases the cluster's circular list (`maccessMy` + `LNEXT`), counting completed accesses
  within an SST window, polling `chrome_mock_timer(...)` every `ACCESSES_TILL_TIMER_POLL` (=90)
  accesses. `matrix[s*NoC + c]` = access count.
- **Window sizing** (mock, `timer_mode==1`): `SST_cycles = 2288 * setsPerCluster * assoc`, floored
  to a 500 µs window; `TST_cycles = g_tsc_freq_hz * tst_sec`;
  `total_slots = TST_cycles / (NoC * SST_cycles)`.

## What to build (all additive — do not modify or replace existing code)

Reuse the already-tested C port of the JS lazy mapping that lives in `src/coverage_validator.c`
(these are `static` there — **copy/lift** them into `mastikElite.c` or a shared header, leaving
`coverage_validator.c` untouched):

- `typedef struct { uint32_t *buf; uint32_t *heads; int *nodeCounts; int numClusters; size_t bytes; } LazyMap;`
- `build_lazy_mapping(LazyMap*, noc, JS_LLC_SETS=16384, JS_LLC_WAYS=12, shufflePages)` — C port of
  JS `LazyMapping.build()`:
  - `bytes = 16384*12*64` (12 MB), `mmap(NULL, bytes, PROT_READ|PROT_WRITE,
    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)` (page-aligned, like V8's typed-array store).
  - node element index `= page*1024 + v*16` (v = bit-6-11 value 0..63);
    `cluster = ((v*64) >> (12 - log2(NoC))) & (NoC-1)`;
    pages Fisher-Yates shuffled per v (defeats HW prefetch); circular link `buf[node] = nextNode`.
- `sweep_lazy_once(...)` — the untimed JS `hammerCluster` (`curr = buf[curr]` 32-bit index chase).

**New code to write (all additive):**

1. A **new** sampler `get_spatioTemporal_memoryGram_ChromeMock_jsmap(LazyMap*, NoC, TST_cycles,
   SST_cycles, matrix, filename)` **alongside** the existing mock sampler (leave that one untouched).
   Same structure, but the inner sweep uses the **index chase** `curr = buf[curr]`, counting
   accesses within the `chrome_mock_timer` SST window with the same `ACCESSES_TILL_TIMER_POLL` (=90)
   batching.
2. A **new victim-source selector** in `runStressNG_batches` (an extra parameter, or a new
   flag / `timer_mode` value) that, when set, builds a `LazyMap` via `build_lazy_mapping` instead of
   a `Clusters_t` via `eviction_sets_to_Clusters`, and calls the new sampler. **The default / unset
   behavior must be byte-for-byte the current Mastik-e_set path.**
3. Write the JS-style runs to a **distinct output dir** (e.g. `data/chrome_clock_jsmap/...`) so the
   existing Mastik-e_set data is never touched.

Keep TST, SST sizing, the stress-ng battery, the CSV path/format, and the classifier pipeline
identical between the two victim sources — the only variable is the victim construction.

## Original JS reference (mirror its semantics exactly)

`JavaScript/main.js`: class `LazyMapping` (`build()` ~lines 185-222); `sweepCluster` /
`sweepClusterDynamicK` (timed quantum, counts accesses, polls clock every `K`); `hammerCluster`
(untimed single sweep). The memorygram is a time-slots × NoC-clusters matrix of access counts.
Match the per-quantum access-count semantics so the C-mock and real-JS memorygrams are comparable.

## Geometry & fixed params (i7-9700k, Coffee Lake)

- **Inclusive** LLC, 12-way, **16384 sets** (2048/slice × 8 slices), 12 MB, ~3.6 GHz TSC.
- Fingerprinting eval: 38 stress-ng classes, ~200 samples/class, 2 s traces, Random Forest,
  5-fold CV. Main independent variable = **NoC** (powers of two; lazy regime ≤ 64). Sweep the same
  NoC set used before (2, 4, 8, 16, 32, 64) so results line up.

## Caveats / already-learned (do not rediscover)

- **Mastik-e_set clusters** give full/stable coverage but the sweep also evicts the *next* cluster
  (forward-prefetch adjacency); shuffling the chase order was tried and did not change the
  NoC-coverage behavior.
- **JS-style lazy map** groups lines sharing only bits 6-11 — **not** guaranteed real physical
  eviction sets, so per-cluster eviction is probabilistic and weakens as NoC grows (fewer lines per
  cluster). This is the suspected accuracy driver.
- Replacement-policy micro-experiments (multiple passes, N accesses per line, same-address repeats)
  were explored in `coverage_validator.c`'s `jsmap` mode. On this **inclusive** LLC, repeated
  same-line / same-address hits are L1 hits and may not reach the LLC (and identical-address loads
  can be HW-coalesced). Relevant only if later trying to "strengthen" the lazy eviction — not needed
  for the first A/B.
- Build: `cd stable && make` (targets compile clean at `-O0`).

## Deliverable & validation

1. New additive sampler + `runStressNG_batches` victim-source option using the JS-style `LazyMap`
   under the Chrome mock clock, output under a distinct dir (e.g. `data/chrome_clock_jsmap/`).
2. Collect across NoC (2, 4, 8, 16, 32, 64); run the existing RF / 5-fold pipeline.
3. **Success = the accuracy-vs-NoC curve now resembles real Chrome** (peak ~NoC=4, then dips)
   rather than the flat ~99.8% of the Mastik-e_set version — which would confirm that the
   lazy-mapping construction, not the timing, explains the real-Chrome accuracy drop.
