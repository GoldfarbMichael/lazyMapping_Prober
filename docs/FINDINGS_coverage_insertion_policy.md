# Findings — Lazy-map LLC coverage: why it drops with NoC, and the insertion-policy limit

**Project:** Spatio-Temporal LLC side channel (Lazy Mapping). **Machine:** i7-9700k
(Coffee Lake), LLC = 16384 sets × 12 ways, 8 slices. **Scope of this note:** the
*coverage* diagnostics on the native C path — why lazy-map (jsmap) LLC coverage
collapses as NoC grows, what mechanism causes it, whether an access-pattern
("eviction strategy") fix recovers it, and the resulting theory that this coverage
loss is what drives the fingerprinting-accuracy drop.

---

## 0. The question

Lazy-map (jsmap) coverage — the fraction of a diagonal cluster's primed victim
ways that the victim sweep actually evicts — falls sharply as NoC grows, while the
real-Mastik-eviction-set path stays near 1.0. We wanted to know **why**, and
whether it can be fixed **without full LLC mapping** (staying in the Lazy regime:
4 KB pages, partition by translation-invariant page-offset bits 6–11 only).

**Coverage metric** (from `coverage_analysis.ipynb`, reproduced in
`python/coverage_compare.py`): prime a real Mastik eviction set (mapping A), let the
victim sweep one lazy cluster once, then probe. The raw miss count per set = how
many of the 12 primed ways the sweep evicted. `Coverage = (min-over-samples of the
mean diagonal raw miss) / 12` — i.e. average ways-evicted per diagonal set.

---

## 1. Diagnosis — it is the replacement/insertion policy, not membership

We built a **control ladder** of three victims, holding everything fixed except the
one variable that separates the hypotheses:

| tree | victim | per-set membership | sweep order |
|---|---|---|---|
| `native` | real Mastik e-sets | **guaranteed 12/set** | **contiguous** |
| `native_shuffled` | real Mastik e-sets | **guaranteed 12/set** | **scattered** (line-shuffled) |
| `native_jsmap_shuffled_p1a3` | lazy (jsmap) victim | statistical (~12 mean, variance) | scattered (page-shuffled) |

**Coverage vs NoC** (2 samples each):

| tree \ NoC | 8 | 16 | 32 | 64 |
|---|--|--|--|--|
| `native` (contiguous, 12/set) | 0.999 | 0.998 | 0.996 | **0.971** |
| `native_shuffled` (scattered, 12/set) | 0.996 | 0.978 | 0.908 | **0.718** |
| `native_jsmap_shuffled_p1a3` (lazy) | 0.969 | 0.958 | 0.892 | **0.506** |

**Conclusion: the drop is caused by scattered access under the replacement policy,
not by missing collisions.**

- `native` vs `native_shuffled` hold membership fixed at a *proven* 12 real
  collisions/set and differ **only** in traversal order. Scattering alone drops
  NoC=64 from 0.971 → 0.718. So order/policy — not membership — is the primary
  cause.
- `native_shuffled` (guaranteed 12) vs `jsmap` (statistical) at NoC=64: 0.718 vs
  0.506. So statistical membership costs only ~0.21 *on top of* the scatter effect;
  it is the **secondary** contributor, not the main one.
- **Oversampling the lazy buffer is therefore ruled out** — adding more colliding
  lines to sets that already have 12 and still don't fully evict changes nothing.

**Ways-evicted distribution @NoC=64** (per diagonal set; bimodal {0,12} would mean
membership, a spread/low mode means policy):

| tree | mean ways | %≥12 (fully evicted) | %≤2 (dead) |
|---|--|--|--|
| `native` | 11.70 | 82% | 0% |
| `native_shuffled` | 9.44 | 64% | 10% |
| `native_jsmap_shuffled_p1a3` | 6.43 | 30% | 27% |

`native_shuffled` sets *provably* contain 12 collisions yet a large fraction evict
only partially — a **policy signature**, not a membership hole.

---

## 2. Mechanism — scan-resistant insertion → attacker lines self-evict

Coffee Lake's L3 uses a scan-resistant adaptive insertion policy (QLRU/RRIP-like):
a newly touched line is inserted at a **distant** re-reference position. When the
victim's ~12 lines for a set are touched **once each and scattered**, each new
insert tends to evict a **previously-inserted attacker line** (the current
eviction candidate) rather than one of the primed victim lines — so the attacker
lines **replace each other** and only ~half the primed ways get evicted. (This is
the user's own framing, and it is what the `native_shuffled` histogram shows.)

Why the two extremes behave as they do:

- **Mastik / `native` wins via same-set contiguity.** `fill_eviction_sets`
  (`src/utils.c`) links each set's 12 same-set 64 B lines into a circular list;
  `sweep_cluster_once` (`src/coverage_validator.c`) chases it, touching a set's 12
  lines **back-to-back** — a clean scan that fully evicts the primed ways in one
  pass, at any NoC.
- **The lazy map cannot reproduce that on 4 KB pages.** A physical set index is
  bits 6–16 + slice; only bits 6–11 (the page offset) are translation-invariant, so
  lazy clustering mixes ~256 physical sets per line-offset and can never place
  same-set lines together without recovering physical frames (= mapping). The user
  chose to **keep 4 KB pages and accept no same-set grouping**.

Supporting observations:

- **NoC dependence** = whether the cluster spans ≥2 line-offsets. At low NoC a
  cluster spans several 64 B offsets, so the HW 128 B adjacent-line prefetcher
  re-touches same-set lines (accidental reinforcement) → coverage stays high. At
  NoC=64 a cluster is a *single* offset, no in-cluster buddy, no reinforcement →
  collapse.
- **`buddyTouch` failed for a structural reason.** Explicitly demand-accessing
  `curr ^ 16` (the 128 B buddy) at NoC=64 touches the **neighbor** cluster's set,
  so it only adds mass off-diagonal — it cannot lift the diagonal by construction.
- **Unshuffled lazy is *worse*** (coverage low even at NoC=2, ≈0.82): the ascending
  page-address stream trains the streamer prefetcher, which pollutes per-set
  eviction. Shuffling is essential for the lazy victim precisely because it defeats
  that prefetcher.
- **Immediate re-hits don't help** (`accessesPerLine`/`sameAddr` null): re-touching
  the exact same line back-to-back is an L1 hit, invisible to L3, so it can't
  promote the line at the shared level.

---

## 3. Eviction-strategy experiment (A / D / C)

**Idea:** since immediate re-hits are absorbed by L1, use a Rowhammer.js-style
**sliding-window** access pattern so a line is re-touched *after a few other lines*
— far enough to reach L3 and re-assert its priority (promote it), so it evicts the
victim instead of being evicted. Grounded in Gruss et al. *Rowhammer.js* (DIMVA'16),
Vila et al. *Theory & Practice of Finding Eviction Sets* (S&P'19), Briongos et al.
*RELOAD+REFRESH* (USENIX'20).

**Implementation:** `sweep_lazy_evict(m, c, A, D, C)` in `src/lazy_map.c` runs an
**indexed** sweep over the cluster's retained node array (JS-portable to a
`Uint32Array`, not a pointer chase):

```c
for (s = 0; s + D <= n; s += C)   // window of D lines, slides by C
  for (a = 0; a < A; a++)         // hammer the window A times
    for (d = 0; d < D; d++)       // ... over its D lines
      touch(nodes[s + d]);
```

**A/D/C semantics.** D = window size (neighbouring lines touched as a group), A =
repeats of a window before sliding, C = slide step. A=D=C=1 is exactly the single
linear pass (identity). Access-count facts:

- **Total accesses** = (⌊(n−D)/C⌋+1) × A × D.
- **Accesses to one interior line** ≈ **A × D / C** (a length-D window sliding by C
  covers a line ~D/C times, each hammering it A times). *(Not A×C.)*

| config | per-line accesses (A·D/C) | NoC64 coverage |
|---|--|--|
| A2 D2 C1 | 4 | **0.595 (best)** |
| A2 D2 C2 | 2 | 0.579 |
| A4 D8 C1 | 32 | 0.463 |
| A2 D16 C1 | 32 | 0.438 |
| A2 D16 C2 | 32 | 0.377 (worst) |

**Grid run:** A∈{2,3,4}, D∈{2,4,8,16}, C∈{1,2}, at NoC 8/16/32/64, 2 iters. Ranked
by NoC=64 coverage. References @NoC64: `native` 0.971, `native_shuffled` 0.718,
lazy baseline (p1a3 chase) 0.506.

**Results / findings:**

1. **Best config: A=2, D=2, C=1 → 0.595 at NoC=64.** The *gain* depends on which chase
   baseline you quote: **+0.089** over the `p1a3` chase (0.506), or only **+0.015** over
   the `p1a1`/`p1a1_same` chase (0.580). The `p1a1` single-access chase is itself ~0.07
   stronger than `p1a3` at NoC=64, so always state the baseline. Either way the best
   recovers at most ~0.09 of the ~0.47 gap and does **not** reach the scattered ceiling
   (0.718), let alone Mastik (0.971).
2. **Tight window wins, wide window hurts.** D=2 tops the table; D=16 falls *below*
   baseline (down to 0.377). It is **not** about volume — A4/D8/C1 touches each line
   32× and loses (0.463 < 0.506); A2/D2/C1 touches each line ~4× and wins. A line
   re-touched a *few* accesses later reinforces its priority (reaches past L1, not so
   far it's pure pollution); re-touched 8+ lines later it doesn't, and just pollutes.
3. **Help is concentrated at NoC=64.** By NoC=32 the best EV (0.878) is ~equal to or
   slightly below baseline (0.892) — NoC=32 is already near its scattered ceiling
   (0.908), so there's no room.
4. **Histogram (NoC=64):** best EV shifts the distribution modestly right — mean
   6.43→7.30, %≥12 30%→35%, %≤2 27%→19% — but stays far from `native_shuffled`
   (mean 9.44) and `native` (mean 11.70).

**Caveat (do not overclaim):** the L1-vs-L3 "why tight windows help" is the
best-supported explanation consistent with the ranking, not a cache-instrumented
measurement. The empirical fact (tight window wins, wide window hurts) is
unambiguous.

**Why the strategy under-delivers — we applied a single-set technique to a multi-set
object.** In the eviction-set literature the parameter `S` is the size of **one**
eviction set = the number of *congruent* addresses that all map to the **same** cache
set; the Rowhammer.js sliding window slides over that single set's addresses, so every
access contends on one set and its re-references *promote same-set lines* (Mastik is
the same: per-set rings, one set at a time). Our lazy **cluster** mixes ~256 physical
sets (bits 6–11 only), so even a D=2 window straddles two different sets — the
re-references never concentrate on a single set. That mismatch, not a tuning failure,
is why the pattern buys only ~0.01–0.09. Using these strategies as intended requires
same-set grouping (= mapping), which is exactly what the Lazy regime forgoes.

**Conclusion of Part 3:** an eviction-strategy access pattern gives a small, real,
JS-portable improvement (adopt **A=2, D=2, C=1** — also one of the cheapest), but it
**cannot recover the same-set contiguity** that gives Mastik its coverage. High-NoC
lazy coverage is capped well below full mapping. That cap is the **spatial-resolution
limit of pure Lazy Mapping** — itself a clean thesis result, not a failure.

---

## 4. Theory tested — coverage loss does NOT drive the accuracy drop (REJECTED)

**Theory:** the lazy/jsmap victim's lower fingerprinting accuracy is caused by the
coverage loss diagnosed above — a scattered sweep occupies each physical set less
completely, weakening the memorygram signal.

**Experiment run:** stress-ng fingerprinting with the **real Mastik e-set victim but
with clusters line-shuffled once** (`-c -s`, the exact `native_shuffled` manipulation),
under the Chrome mock clock, NoC-swept — versus contiguous Mastik.

**Result: the theory is rejected.** Shuffled-Mastik fingerprinting stayed at
**~99.9% accuracy with the plateau already at NoC=8** — essentially identical to
contiguous Mastik, **no drop**. Degrading single-pass coverage (0.97→0.72 at NoC=64)
did **not** degrade accuracy. So the fingerprinting-accuracy drop is **not** caused by
the coverage / scattered-eviction effect.

**Why (the interpretation guard was borne out):** the coverage metric is a
**single-pass** prime+probe, but the memorygram **hammers each cluster continuously**
for the quantum (many passes). That continuous re-reference is exactly what *recovers*
eviction on a scattered layout (cf. the `passes` and tight-window EV results), so a
cluster that covers poorly in one pass still reaches good **steady-state occupancy**
under hammering. Single-pass coverage is therefore the wrong proxy for the
continuously-hammered fingerprinting signal — which is precisely the caveat flagged
before the run. **Coverage and fingerprinting-accuracy are decoupled.**

**Consequence:** the accuracy drop must come from something other than scatter/coverage
(membership, victim construction, timer/clock realization, or real-browser JS runtime).
Those are open and pursued outside this document.

---

## 5. Consolidated numbers (NoC=64 unless noted)

| quantity | value |
|---|---|
| `native` coverage (contiguous, real e-sets) | 0.971 |
| `native_shuffled` coverage (scattered, guaranteed 12/set) | 0.718 |
| jsmap baseline coverage — `p1a3` chase | 0.506 |
| jsmap baseline coverage — `p1a1`/`p1a1_same` chase | 0.580 |
| jsmap **unshuffled** coverage (streamer pollution) | 0.527, and low even @NoC2 (~0.82) |
| best eviction strategy (A2 D2 C1) | 0.595 (+0.089 vs `p1a3`, +0.015 vs `p1a1`) |
| worst eviction strategy (A2 D16 C2) | 0.377 |
| gap the EV pattern closes / total gap to Mastik | ≤~0.09 / ~0.47 |

---

## 6. Code / artifacts produced this session

- `src/lazy_map.{c,h}` — `LazyMap` now **retains** the per-cluster node arrays
  (`clusterNodes`); added `sweep_lazy_evict(m,c,A,D,C)` (indexed sliding-window
  eviction strategy). Default path (`sweep_lazy_once` chase) unchanged.
- `src/coverage_validator.c` — threaded A/D/C through `probe_set_jsmap` →
  `run_native_jsmap_experiment` → `main()` (argv[9..11], default 1/1/1 = off, selects
  `sweep_lazy_evict`); output tree suffix `_evA{A}D{D}C{C}`; renamed jsmap roots to
  `native_jsmap[_shuffled]`.
- `run_coverage_native.sh` — `EV_A/EV_D/EV_C` env knobs and `SHUFFLE=0/1` toggle
  (both modes; unshuffled trees `native` / `native_jsmap_p…`).
- `python/coverage_compare.py` — control ladder + auto-discovered eviction-strategy
  grid ranked by NoC=64 coverage + ways-evicted histograms.

**Constraint honoured:** the timed fingerprinting sampler
(`get_spatioTemporal_memoryGram_ChromeMock_jsmap`) and all defaults are untouched;
every new knob defaults to the prior behaviour.

---

## 7. Open items / next steps

- **DONE — shuffled-cluster fingerprinting run (§4): theory rejected.** The
  shuffled-cluster flag was implemented on the fingerprinting path (`-c -s`,
  `shuffle_cluster_nodes` now shared in `mastikElite.c`, output
  `data/chrome_clock_shuffled/`) and run NoC-swept; accuracy stayed ~99.9% (plateau at
  NoC=8), so coverage is **not** the accuracy driver. Coverage and accuracy are decoupled.
- **New open question — locate the real accuracy drop.** Since scatter/coverage is out,
  the remaining candidates are: victim membership variance, lazy victim construction
  (4 KB pages / index chase / self-contention), timer/clock realization at high NoC, and
  real-browser JS runtime. First localize it: is the **C-port jsmap (mode 2)** accuracy
  actually below **Mastik (mode 1)** in the ML results? If not, the drop is
  real-browser-only.
- Coverage-side (still valid): the high-NoC lazy coverage cap is a real
  spatial-resolution limit of Lazy Mapping; A2/D2/C1 buys a small (~0.09) coverage gain.
  This is now decoupled from the accuracy question.
