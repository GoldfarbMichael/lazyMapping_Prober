# Coverage Analysis

Script: `stable/python/coverage_analysis.py`. Figures: `stable/python/coverage_figures/`.
Data: `stable/data/coverage/<tree>/NoC<NN>/<iii>.csv`.

Covers the experiments **not** previously written up: the prefetcher-disabled results and the
path that led to the bidirectional sweep. Already documented elsewhere and out of scope here:
realchrome, native, native-shuffled, JSmap shuffled/unshuffled, and A/D/C with prefetchers on.

## 0. Method and caveats

**Raw data.** Each CSV is one sample: columns `S0..S16383` (one per LLC set), then `NoC`
cluster-sweep rows, then 15 idle rows. `cell[c][i]` = ways evicted (0..12) in set `i` while the
victim swept lazy cluster `c`. Set → physical cluster = top `log2(NoC)` bits of the PA page
offset, the same rule the victim uses in `build_lazy_mapping` (`lazy_map.c:51,71`).
Per sample: aggregate set-columns to `NoC` physical-cluster columns by mean → `raw` (NoC×NoC);
same for the idle rows → `base`; `sub = clip(raw - base, 0, None)`.

**Metrics.**
- `coverage_min` = `diag(min over samples of raw).mean() / 12` — worst-case fraction of the 12
  ways the victim reliably evicted in its own cluster. Primary metric throughout.
- `coverage_mean` = same with mean instead of min.
- `diag_mass` = `trace(sub)/sum(sub)` — spatial specificity; random chance = `1/NoC`. §1 and §5 only.

**Caveats.**
- **n=2 samples for nearly every tree** (exceptions: 24MB n=5). `coverage_min` is therefore a
  min-over-2 — a fragile worst-case estimator. `coverage_mean` is reported alongside everywhere;
  the two track closely (typical gap 0.01–0.05), so conclusions here do not rest on the min alone.
- Untagged tree name ⇒ `pref0x0` (all prefetchers on); the `_pref` tag was added later.
- `_same` is **inert** for every `p*a1` tree: both branches of `sweep_lazy_once` loop
  `for (r=1; r<accessesPerLine; r++)` (`lazy_map.c:135-141`), zero iterations at
  `accessesPerLine=1`. So `p3a1_same ≡ p3a1` and the §1 prefetcher ladder is not confounded.
- `ev*` and `bidir*` trees exist only at NoC 8/16/32/64.
- Prefetcher masks (MSR 0x1a4, set bit = disabled): `0x0` all on, `0x2` L2 adjacent-line off,
  `0xf` all off.

Sanity: 0 violations of `coverage_min ≤ coverage_mean`; all `diag_mass` in [0,1]; row counts
match `NoC + 15` in every file loaded.

---

## 1. Baseline — prefetcher ladder

**Why:** establish how coverage behaves across NoC with prefetchers on vs. off, before any
access-pattern variation. This is the reference every later section is measured against.

**Axes:** x = NoC (log2), y = coverage (fraction of 12 ways), one line per prefetcher mask, two
panels (worst-case and mean-case). p3a1 is primary; p1a1 secondary (no `pref0xf` tree exists).

**Trends:** the two prefetcher states are **inverted in NoC**, and they cross between NoC 16 and 32.

| p3a1 | NoC2 | NoC4 | NoC8 | NoC16 | NoC32 | NoC64 |
|---|---|---|---|---|---|---|
| `0x0` (all on) | 0.948 | 0.968 | 0.964 | 0.964 | 0.891 | **0.673** |
| `0x2` (adj-line off) | — | — | 0.389 | 0.329 | 0.579 | 0.681 |
| `0xf` (all off) | — | — | 0.431 | 0.420 | 0.684 | **0.825** |

With prefetchers on, coverage is high and flat to NoC=16 then collapses. With them off, it
starts low and *rises* with NoC. p1a1 (secondary) shows the same shape: `0x0` 0.965→0.580 across
NoC 8→64, `0x2` 0.275→0.567.

`diag_mass` falls monotonically with NoC in every config (e.g. `0x0`: 0.990 → 0.497 across NoC
2→64) but stays far above chance (`1/NoC`, 0.5 → 0.016) everywhere — the eviction is always
strongly cluster-specific even where its magnitude is poor.

**Exceptions:** `0x2` and `0xf` track each other closely at NoC 8/16 but diverge at 32/64
(0.579 vs 0.684, 0.681 vs 0.825) — disabling *all* prefetchers is meaningfully better than
disabling only the adjacent-line one at high NoC, not equivalent.

**Results:**

![Baseline coverage vs NoC](../../stable/python/coverage_figures/baseline_trend.png)

Per-cluster at NoC=64 (the vector the correlation phase will join against):

![Per-cluster coverage at NoC=64](../../stable/python/coverage_figures/baseline_percluster_noc64.png)

The `pref0xf` series is flat ~0.95 for most clusters but collapses to ~0.4 at exactly 16 of the
64 clusters: `{0,4,8,...,28, 33,37,...,61}` — period 4, with a one-step phase shift at offset 32.
This is the *same* index set, including the phase shift, that showed the period-4 low pattern in
`SelfEviction_Analysis.md` §3. Recorded here as an observation; the formal join is phase 3.

**Conclusion:** there is no single "coverage vs NoC" curve — the direction of the trend flips
with prefetcher state. The prefetchers-off regime is the one where high NoC is *favourable*, and
its aggregate weakness at low NoC is concentrated in a periodic minority of clusters rather than
spread evenly.

---

## 2. A/D/C eviction-strategy grid

**Why:** test whether a Rowhammer.js-style sliding-window access pattern (A = window repeats,
D = window size, C = step) recovers coverage, particularly in the prefetchers-off regime.

**Axes:** heatmap, rows = 24 (A,D,C) combos (A∈{2,3,4}, D∈{2,4,8,16}, C∈{1,2}), cols = NoC,
cell = `coverage_min`; two panels (`pref0x0`, `pref0xf`). Excludes A3D3072C3072 (§4).

**Trends:** the grid reproduces the §1 flip and nothing more. Best cell per NoC:

| | NoC8 | NoC16 | NoC32 | NoC64 |
|---|---|---|---|---|
| best `pref0x0` | 0.976 | 0.968 | 0.918 | 0.595 |
| best `pref0xf` | 0.354 | 0.350 | 0.631 | 0.788 |
| §1 baseline `pref0xf` | 0.431 | 0.420 | 0.684 | 0.825 |

**Exceptions:** none — the result is uniformly negative. At `pref0xf` the *best* of 24 combos is
below the plain §1 baseline at every NoC.

**Results:**

![A/D/C grid coverage](../../stable/python/coverage_figures/adc_grid.png)

**Conclusion:** no small-window A/D/C setting improves coverage in either prefetcher regime.
Within the grid, larger D (8, 16) is consistently *worse* at high NoC under `pref0xf`.

---

## 3. Buffer size — 24MB vs 12MB

**Why:** test whether doubling the victim buffer (mean lines/set 12 → 24) improves coverage.

**Axes:** table, rows = NoC, cols = buffer size; p3a1 at `pref0xf`.

**Trends:** slightly worse at low NoC, indistinguishable at high NoC.

| NoC | 12MB (n=2) | 24MB (n=5) |
|---|---|---|
| 8 | 0.431 | 0.359 |
| 16 | 0.420 | 0.357 |
| 32 | 0.684 | 0.674 |
| 64 | 0.825 | 0.838 |

**Exceptions:** the 24MB tree is the only one with n=5, so its numbers are the *better*-estimated
of the two — the low-NoC deficit is unlikely to be a sampling artifact.

**Results:** table above.

**Conclusion:** doubling the buffer does not help; it costs ~0.07 coverage at NoC 8/16 and is
neutral at 32/64.

---

## 4. A3D3072C3072 — window = one NoC=64 cluster

**Why:** the §1 flip showed that with prefetchers off, high NoC is where coverage is good. If a
large cluster is weak because its lines are swept as one long run, sweeping it as a sequence of
*subclusters* the size of a NoC=64 cluster might recover the high-NoC behaviour at lower NoC.
`D=C=3072` is exactly `nodesPerCluster` at NoC=64 with the 12MB buffer
(`1 × 16384 sets × 12 ways / 64 = 3072`), so the sliding window is precisely one NoC=64 cluster
wide with no overlap.

**Axes:** table, rows = NoC, cols = {§1 baseline, A3D3072C3072 shuffled, unshuffled}, `pref0xf`.

**Trends:** essentially neutral — it matches the baseline rather than beating it.

| NoC | baseline `p3a1_0xf` | A3D3072C3072 shuffled | unshuffled |
|---|---|---|---|
| 8 | 0.431 | 0.453 | 0.351 |
| 16 | 0.420 | 0.401 | 0.312 |
| 32 | 0.684 | 0.667 | 0.677 |
| 64 | 0.825 | 0.839 | 0.836 |

**Exceptions:** the unshuffled variant is clearly worse at NoC 8/16 (0.351, 0.312) but catches up
at 32/64 — page shuffling matters only in the low-NoC regime.

**Results:** table above.

**Conclusion:** the subcluster hypothesis does not hold. Making the sweep *spatially* resemble a
NoC=64 sweep does not import NoC=64's coverage; whatever makes high NoC good under prefetchers-off
is not simply the size of the contiguous run being swept.

### 4b. Decoy dose

**Why:** the decoy inserts `dK` unrelated lines between subcluster windows, testing whether
breaking up the access stream (rather than resizing it) is what matters.

**Axes:** x = decoy dose (log2, 8→1024), y = `coverage_min`, one line per NoC, dashed reference =
same NoC without decoy. All on A3D3072C3072, `pref0xf`.

**Trends:** helps where the baseline is weak, does nothing where it is already strong. NoC=8
0.453→0.544 and NoC=16 0.401→0.511 (both ~+0.10); NoC=32 0.667→~0.73; NoC=64 flat at ~0.84.
Most of the gain arrives by dK=32 and plateaus.

**Exceptions:** NoC 8/16 are non-monotonic in dose (NoC=8 peaks 0.540 at dK=128, dips to 0.506 at
dK=256, 0.544 at dK=1024) — at n=2 these wiggles are within noise; the plateau, not the shape,
is the finding.

**Results:**

![Decoy dose](../../stable/python/coverage_figures/decoy_dose.png)

**Conclusion:** interrupting the access stream recovers ~0.10 coverage in the weak low-NoC regime
— more than resizing the window did (§4) — but plateaus well short of the ~0.83 that high NoC
reaches unaided. Directionally informative, not a solution.

---

## 5. Bidirectional sweep — the winner

**Why:** the preceding variants all modify *what* is swept (window size, dose, buffer). Bidir
modifies the *direction*: each pass sweeps forward then backward, R oscillations
(`sweep_lazy_bidir`).

**Axes:** x = NoC, y = `coverage_min`, one line per R∈{1,2,4,8}, two panels (`pref0x0`,
`pref0xf`), with the §1 baseline overlaid. `--` = not collected.

**Trends:** at `pref0xf`, bidir is ~0.95–0.96 at **every** NoC and **every** R — the NoC
dependence disappears entirely.

| NoC | R1 `0x0` | R2 `0x0` | R4 `0x0` | R8 `0x0` | R1 `0xf` | R2 `0xf` | R4 `0xf` | R8 `0xf` | base `p3a1_0xf` | base `p1a1_0x0` |
|---|---|---|---|---|---|---|---|---|---|---|
| 8 | 0.905 | 0.953 | — | — | 0.954 | 0.955 | 0.961 | 0.964 | 0.431 | 0.965 |
| 16 | 0.960 | 0.958 | 0.939 | — | 0.951 | 0.957 | 0.963 | 0.963 | 0.420 | 0.961 |
| 32 | 0.875 | 0.922 | 0.955 | — | 0.953 | 0.945 | 0.960 | 0.962 | 0.684 | 0.836 |
| 64 | 0.762 | 0.869 | 0.954 | 0.957 | 0.953 | 0.955 | 0.949 | 0.964 | 0.825 | 0.580 |

At `pref0x0`, R=1 still degrades with NoC (0.905→0.762) but higher R progressively removes that:
R=4 holds 0.954 at NoC=64 where the matched `p1a1_0x0` baseline is 0.580.

`diag_mass` improves too — it is not a magnitude-for-specificity trade. At NoC=64: 0.702 (R1
`0xf`) vs 0.562 baseline, against chance 0.016.

**Exceptions:** `bidirR4_pref0x0` and `bidirR8_pref0x0` are incomplete (marked `--`), so the
`pref0x0` R-progression is read from fewer points than the `pref0xf` one. The two comparisons
differ in rigour: bidir-vs-`p3a1_0xf` is **not** pass-matched (baseline passes=3, bidir passes=1
— no plain `p1a1_pref0xf` tree exists), whereas bidir-vs-`p1a1_0x0` at `pref0x0` is matched.
The within-bidir `0xf`-vs-`0x0` contrast is clean and shows the same conclusion independently.

**Results:**

![Bidirectional sweep coverage vs NoC](../../stable/python/coverage_figures/bidir_trend.png)

Per-cluster at NoC=64:

![Per-cluster coverage at NoC=64, bidir vs baseline](../../stable/python/coverage_figures/bidir_percluster_noc64.png)

Bidir is flat across all 64 clusters — per-cluster spread 0.017 (min 0.955) for R=8 versus 0.588
(min 0.375) for the baseline, a 35× reduction. The periodic 16-cluster dropout from §1 is gone.

**Conclusion:** bidir is the only variant tested that both raises coverage to ~0.96 and makes it
independent of NoC and of cluster index. It does not merely lift the average — it removes the
per-cluster structure that every other configuration exhibits.

---

## Bottom line

With prefetchers on, coverage is high at low NoC and collapses by NoC=64; with them off the trend
inverts. Resizing the swept window (§4), enlarging the buffer (§3), and the whole small-window
A/D/C grid (§2) all fail to beat the plain baseline in the prefetchers-off regime; interrupting
the stream with decoy lines (§4b) recovers ~0.10 at low NoC but plateaus.

Bidirectional sweeping (§5) reaches ~0.96 at every NoC and every R under prefetchers-off, improves
spatial specificity at the same time, and flattens the per-cluster profile that all other
configurations share.

The §1 per-cluster data at NoC=64 also shows that the prefetchers-off baseline's weakness is
carried by exactly 16 of 64 clusters in a period-4 pattern — the same index set found in
`SelfEviction_Analysis.md` §3. That link is recorded, not analysed here; it is the subject of the
correlation phase.
