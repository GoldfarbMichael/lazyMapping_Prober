# Self-Eviction Analysis

Data: `stable/data/coverage/selfevict_shuffled_pref0x{0,1,2,f}` (default sweep) and
`selfevict_shuffled_bidirR{1,2,4}_pref0x{0,1,2,f}` (bidir sweep). Script:
`stable/python/self_eviction_analysis.py`.

Prefetcher configs (MSR 0x1a4, bit=1 disables): `0x0`=all on, `0x1`=L2 HW prefetcher off,
`0x2`=L2 adjacent-line prefetcher off, `0xf`=all off.

Self-eviction measures whether a lazy-map cluster's own lines evict each other under pure
repeated self-touching, with **no victim present** — it isolates capacity/replacement effects
of the cluster itself from anything caused by attacker-victim interaction. Two normalizations
are tracked throughout: `self_frac_node = M_self/nodeCount` and `self_frac_cold = M_self/M_cold`.

---

## 1. Sanity check — `M_cold / nodeCount`

**Why:** `M_cold` is a counter/methodology calibration, not part of the experimental treatment
— it should equal ~1.0 regardless of config, NoC, sweep type or R, since the cold pass is
hardcoded single-directional even in bidir mode. This has to hold before `M_self` can be
trusted at all.

**Axes:** rows = prefetcher config × NoC, value = mean `cold_ratio` (default sweep shown;
bidir R=1/2/4 confirmed to match within noise, see Results).

**Trends:** splits cleanly by whether the L2 adjacent-line prefetcher is active — ~0.79-0.82
for `0x0`/`0x1` (adjacent-line on), ~0.997-0.999 for `0x2`/`0xf` (adjacent-line off), at every
NoC except 64, where all four configs converge to ~0.999. Mechanistically consistent: the
adjacent-line prefetcher pulls in the next line during the cold sweep itself, so fewer than
`nodeCount` lines register as demand-load misses.

**Exceptions:** `M_cold` is *not* the flat, condition-independent ~1.0 constant it was designed
to be — it's prefetcher-state dependent. This is itself the main finding of this table.

**Results:**

| pref_config | NoC2 | NoC4 | NoC8 | NoC16 | NoC32 | NoC64 |
|---|---|---|---|---|---|---|
| 0x0 | 0.815 | 0.805 | 0.792 | 0.791 | 0.796 | 0.999 |
| 0x1 | 0.802 | 0.800 | 0.796 | 0.799 | 0.799 | 0.999 |
| 0x2 | 0.996 | 0.997 | 0.998 | 0.998 | 0.997 | 0.999 |
| 0xf | 0.999 | 0.999 | 0.999 | 0.999 | 0.999 | 0.999 |

Confirmed condition-independent: bidir R=1/2/4 `cold_ratio` matches the default-sweep row
above within ±0.01 at every (config, NoC) cell — i.e. the fix (cold pass always
single-directional) holds under bidir too.

**Conclusion:** `M_self` must be interpreted per-prefetcher-config, not against one shared
baseline. Use `self_frac_cold` alongside `self_frac_node` when comparing across configs — they
diverge more for `0x0`/`0x1` than for `0x2`/`0xf`.

---

## 2. Core metric — self-eviction fraction vs NoC (default sweep)

**Why:** the primary trend — how self-eviction fraction varies with NoC, per prefetcher
config, with no victim present.

**Axes:** x = NoC (2-64, log2), y = self-eviction fraction (two panels: `/nodeCount`,
`/M_cold`), one line per prefetcher config, mean ± 95% CI over iterations.

**Trends:** U-shaped, not monotonic — decreases from NoC=2 to a minimum, then rises sharply.
The reversal point depends on the prefetcher split from §1: `0x0`/`0x1` bottom out at NoC=16
and reverse gradually, sharply by NoC=64; `0x2`/`0xf` bottom out at NoC=16 and reverse a full
step earlier, already at NoC=32.

**Exceptions:** the U-shape itself contradicts a naive "self-eviction rises with NoC"
expectation — a single monotonic-trend statement is not accurate to this data.

**Results:**

| pref_config | NoC2 | NoC4 | NoC8 | NoC16 | NoC32 | NoC64 |
|---|---|---|---|---|---|---|
| 0x0 | 0.408 | 0.382 | 0.336 | 0.325 | 0.329 | 0.436 |
| 0x1 | 0.425 | 0.389 | 0.360 | 0.324 | 0.315 | 0.418 |
| 0x2 | 0.396 | 0.296 | 0.233 | 0.215 | 0.442 | 0.455 |
| 0xf | 0.350 | 0.275 | 0.252 | 0.245 | 0.417 | 0.433 |

![Self-eviction fraction vs NoC, default sweep, both normalizations](../../stable/python/self_eviction_figures/core_metric_trend.png)

**Conclusion:** any downstream use of "self-eviction vs NoC" must be qualified by prefetcher
config; the reversal point (NoC=32 vs NoC=64) is itself a prefetcher-dependent result, not
noise.

---

## 3. Cluster heterogeneity

**Why:** is self-eviction spatially uniform within a (config, NoC) cell, or concentrated in
specific clusters? This explains the *mechanism* behind the U-shape's upturn in §2.

**Axes:** x = `cluster/NoC` (normalized position — raw cluster index is not comparable across
NoC, see `lazy_map.c:51,71`), y = per-cluster mean `self_frac_node`, color = NoC, one panel
per prefetcher config.

**Trends:** at NoC=64 (all configs) and NoC=32 (`0x2`/`0xf` only), `self_frac_node` rises
roughly monotonically across the axis — from ~0.2 near position 0 to ~0.6 (`0x0`/`0x1` NoC=64)
or ~0.6-0.78 (`0x2`/`0xf` NoC=32/64) near position 1. At NoC ≤ 16, per-cluster points are
tightly banded — no gradient.

**Exceptions:** at NoC=64, `cluster` collapses to exactly the line's offset within its 4KB
page (confirmed from the bit math in `build_lazy_mapping`, not inferred). This gradient is a
**within-page-offset effect**, not a whole-buffer effect, and it was not predicted — it's the
first place this doc departs from what the sanity/core-metric tables alone would suggest.
Cause (LLC addressing vs. some harness/allocation bias correlated with page offset) is not
established by this experiment.

**Results:**

![Cluster heterogeneity, default sweep](../../stable/python/self_eviction_figures/cluster_heterogeneity.png)

**Conclusion:** the U-shape's upturn in §2 is driven by specific page offsets, not a uniform
rise across all clusters at high NoC. Needs a targeted follow-up (what causes the page-offset
gradient) before the NoC=64 uptick is treated as a settled architectural signal elsewhere.

---

## 4. Prefetcher comparison

**Why:** isolate the prefetcher effect on self-eviction at fixed NoC, independent of the
NoC trend itself.

**Axes:** x = NoC (categorical), y = mean `self_frac_node`, bars grouped/colored by
prefetcher config.

**Trends:** `0x2`/`0xf` sit below `0x0`/`0x1` for NoC 4-16, and at or above them for NoC
32-64 — consistent with, and a re-statement of, the reversal-timing split from §2.

**Exceptions:** none beyond what §2/§3 already explain.

**Results:**

![Self-eviction fraction by prefetcher config, default sweep](../../stable/python/self_eviction_figures/prefetcher_comparison_bar.png)

**Conclusion:** the L2 adjacent-line prefetcher bit doesn't just shift the overall
self-eviction level — it shifts *where* the U-shape turns.

---

## 5. Bidir vs. default sweep

**Why:** the central question this round of data collection was for — does self-eviction
under the bidirectional sweep (no victim present) show the same signature that, in the
coverage experiment, distinguished an insertion/replacement-policy explanation from a
membership explanation for bidir's better coverage?

**Axes:** rows = config × NoC × R (1, 2, 4), columns = default `self_frac_node`, bidir
`self_frac_node`, absolute delta, relative delta (%).

**Trends:** bidir self-eviction is higher than default in **every single cell tested, no
exceptions**, and grows with R:

| R | delta_pct min | delta_pct mean | delta_pct max | delta_abs min | delta_abs mean | delta_abs max |
|---|---|---|---|---|---|---|
| 1 | 16.2% | 87.4% | 155.1% | 0.072 | 0.285 | 0.426 |
| 2 | 119.5% | 247.3% | 342.7% | 0.544 | 0.829 | 1.092 |
| 4 | 306.8% | 572.8% | 781.5% | 1.373 | 1.934 | 2.425 |

The smallest relative jump at each R is consistently at NoC=64, where the default-sweep
baseline is already elevated by the §2 U-shape (which is why absolute delta, not just
percentage, is reported — the moving baseline makes percentages alone misleading, e.g. `0x2`
at NoC=32, R=1 shows only +16.2% but that's against an already-elevated default of 0.442, not
a weak bidir effect).

**Exceptions:** R=8 was not collected (deliberate scope cut). R>1 got 30 iterations/NoC for
`0x1`/`0x2`/`0xf` vs 50 for `0x0` and for all configs at R=1 — wider CIs at R=2/R=4 reflect
this, not instability.

**Results:** full 72-row table in script output (`self_eviction_analysis.py` §5); summary
above.

**Conclusion:** directionally consistent with the insertion/replacement-policy explanation —
a policy that protects scan-order data from a single-directional access pattern would be
expected to let self-eviction rise *and* let victim-line eviction rise together once that
pattern is defeated, and that's what's observed here on the self-eviction side alone, with no
victim involved. This does not by itself rule out a capacity/associativity explanation instead
of insertion-policy specifically — it's consistent with, not proof of, the hypothesis.

---

## 6. Variability (iteration-to-iteration)

**Why:** how much to trust the point estimates in §2/§4 — are they stable across the 30-50
iterations, or noisy/outlier-driven?

**Axes:** x = NoC, y = per-iteration mean `self_frac_node` (averaged over that iteration's
clusters), one panel per prefetcher config, default sweep.

**Trends:** spread narrows from NoC=2 to a minimum around NoC=8-32 (depending on config), then
widens again at 32/64 — tracking the same U-shape as the mean.

**Exceptions:** visible outliers at NoC=64 for `0x0` and `0xf` specifically (several
iterations well above the box).

**Results:**

![Iteration-to-iteration variability, default sweep](../../stable/python/self_eviction_figures/variability_boxplot.png)

**Conclusion:** point estimates in §2/§4 are reasonably tight except at the NoC=64 tails for
`0x0`/`0xf`, where a handful of high-outlier iterations pull the mean up — worth a second look
before leaning on the exact NoC=64 value for those two configs specifically.

---

## Bottom line

Self-eviction is not simply "higher at high NoC" — it's U-shaped, prefetcher-dependent in
where it turns, and the turn itself is concentrated in specific page offsets rather than
uniform. Against that backdrop, the bidir result is clean and unambiguous: self-eviction under
bidir exceeds default at every tested condition, growing with R, which is consistent with (but
does not prove) the insertion/replacement-policy explanation for bidir's coverage result.

Open for follow-up: R=8 not collected; cause of the page-offset gradient unconfirmed.
