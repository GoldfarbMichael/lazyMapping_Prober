# Spillover Rate Analysis

Script: `stable/python/spillover_analysis.py`. Figures: `stable/python/spillover_figures/`.
Formal definition: `Metrics_Definitions.md`.

**Spillover rate** = fraction of the **non-target** lines that the victim evicted.
0 = perfectly specific, 1 = evicted everything everywhere. It reduces to
`mean(off-diagonal of the cluster matrix) / 12`.

Where coverage and cleansing measure what the victim achieved *inside* its own cluster, spillover
measures what it damaged *outside* it. Together they are a true-positive / false-positive pair.

## 0. Method and caveats

Two conventions differ from the other metrics, both because **lower is better**:

- **Worst case over samples is `max`**, not `min`. Reporting the min would show the flattering sample.
- **Spillover is degenerate alone** — a victim that does nothing scores a perfect 0. Coverage is
  therefore printed in every table, and §4 plots the two axes against each other. No spillover
  number in this document should be quoted without its coverage.

Other notes:
- Reported **raw** (includes the idle noise floor) and **baseline-subtracted** (victim-caused only).
  `noise_share = raw − subtracted`. All cross-condition comparisons use the subtracted form.
- n = 2 samples per tree.
- Values are small in absolute terms (mostly 10⁻³–10⁻²) and span ~1000× across conditions, so
  figures use a **log** axis; the ratios are the signal, not the absolute values.
- Untagged tree name ⇒ `pref0x0`. Masks: `0x0` all on, `0x2` L2 adjacent-line off, `0xf` all off.

**Sanity** (52 cells): 0 values outside [0,1]; 0 cases of `spill_raw_max < spill_raw`; 0 cases of
`spill_sub > spill_raw` — the last holds by construction, since subtracting a non-negative baseline
and clipping at zero can only lower the value.

---

## 1. Lazy-map baseline

**Why:** establish the spatial specificity of the JS-faithful victim before comparing anything else.

**Axes:** table over sweep × prefetcher mask × NoC; raw and subtracted spillover, noise share,
coverage.

**Trends:** the lazy map is **uniformly specific** — subtracted spillover stays in 0.002–0.016
across every mask and NoC, with no dramatic structure. Prefetcher mask barely moves it
(`p3a1` at NoC=64: 0.0112 / 0.0080 / 0.0104 for `0x0` / `0x2` / `0xf`).

**Exceptions:** the **raw** figures are dominated by the idle floor at low NoC. At NoC=2, `p1a1_0x0`
reads 0.0449 raw but 0.0032 subtracted — **93% of apparent spillover is noise**; `p3a1_0x0` is 86%.
The share falls to ~25% by NoC=64. Any use of the raw form at low NoC would be measuring the
noise floor, not the victim.

**Results:** table in script output §1.

**Conclusion:** the lazy map does not have a spillover problem of its own. That matters because it
is the metric where the lazy map might have been expected to suffer — statistical filling could
have scattered evictions — and it does not.

---

## 2. Real Mastik e-sets — the prefetcher effect

**Why:** real e-sets have guaranteed membership and were expected to be the specificity reference.
Sweeping the full prefetcher ladder tests whether that advantage is intrinsic or conditional.

**Axes:** table over sweep order (contiguous / shuffled) × mask × NoC.

**Trends:** the advantage is **entirely conditional on prefetchers being off**, and the effect is
enormous.

| NoC | cont `0x0` | cont `0x2` | cont `0xf` | shuf `0x0` | shuf `0xf` |
|---|---|---|---|---|---|
| 2 | **0.4927** | — | — | 0.0485 | — |
| 8 | 0.1248 | 0.1477 | **0.0021** | 0.0413 | **0.0002** |
| 16 | 0.0606 | 0.0602 | 0.0007 | 0.0341 | 0.0003 |
| 64 | 0.0140 | 0.0066 | 0.0004 | 0.0202 | 0.0003 |

With prefetchers on, a contiguous sweep at NoC=2 evicts **49% of everything outside its target**.
Turning all prefetchers off drops the same configuration by 60–300×.

**Exceptions — two, both informative:**

1. **`0x2` does not help.** At NoC=8 contiguous, disabling only the L2 adjacent-line prefetcher
   gives 0.1477 — slightly *worse* than leaving everything on (0.1248). Only `0xf` collapses it
   (0.0021). This is consistent with the L2 **streamer** being the culprit rather than adjacent-line:
   a contiguous sweep is exactly the access pattern a streamer locks onto, and it runs ahead across
   cluster boundaries. Consistent with, not proven by, this data.
2. **Shuffling substitutes for disabling.** At NoC=2 with prefetchers on, shuffling the sweep order
   over the *same* sets drops spillover from 0.4927 to 0.0485 — 10×. This is the first direct
   measurement of why the victim traversal is shuffled.

**Results:**

![Spillover vs NoC](../../stable/python/spillover_figures/spillover_trend.png)

**Conclusion:** prefetchers, not membership, are the dominant source of spatial non-specificity.
This **reverses** the reading from coverage and cleansing, where real e-sets looked strictly
superior: at `pref0x0`, `native_shuffled` spills 0.0202 at NoC=64 against bidir's 0.0064 — the lazy
map is 3× *more* specific there.

A caveat in the other direction: at `pref0xf`, real e-sets still beat the lazy map by 20–50×
(0.0003 vs 0.0064–0.0104). The lazy map has a genuine residual specificity deficit in that regime.
Its cause is not established here.

---

## 3. Bidirectional sweep — the R tradeoff

**Why:** bidir fixed coverage and cleansing. Spillover is the axis on which it might be paying.

**Axes:** table over R × mask × NoC, plus an R-dependence table pairing spillover with coverage.

**Trends:** **spillover grows with R while coverage does not.** At `pref0xf`:

| NoC | R1 | R2 | R4 | R8 | coverage (all R) |
|---|---|---|---|---|---|
| 8 | **0.0102** | 0.0171 | 0.0142 | 0.0147 | 0.954–0.964 |
| 16 | **0.0091** | 0.0104 | 0.0157 | 0.0142 | 0.951–0.963 |
| 32 | **0.0073** | 0.0085 | 0.0100 | 0.0115 | 0.945–0.962 |
| 64 | **0.0064** | 0.0077 | 0.0084 | 0.0101 | 0.949–0.964 |

At NoC=64, going R1→R8 costs **58% more spillover** to buy **1.2% more coverage**.

**Exceptions:** R1 is lowest at every NoC at `0xf`, and the ordering is monotone at NoC 32 and 64
but not at NoC 8/16 (R2 exceeds R4 at NoC=8). At n=2 those inversions are within noise; the trend
across NoC is the finding, not the exact ordering in any one row.

**Results:** tables in script output §3; see also the operating-point figure below.

**Conclusion:** **R=1 is the efficient operating point.** It has the lowest spillover of any bidir
setting, statistically identical coverage, and is the cheapest to run. This is an independent
argument for the same choice cost already suggested — the two now agree.

---

## 4. Operating point — coverage vs spillover

**Why:** neither metric means anything alone. This is the view that ranks configurations honestly.

**Axes:** x = subtracted spillover (log, lower better), y = coverage (higher better). One point per
NoC per victim. **Top-left is the goal.**

**Trends:** the two prefetchers-off real-e-set series occupy the top-left corner alone
(coverage 0.90–0.99 at spillover 0.0002–0.002). All prefetchers-on configurations are pushed far
right regardless of victim type. Lazy bidir sits mid-field: high coverage (~0.95) at moderate
spillover (0.006–0.016). The lazy baseline `p3a1_0xf` sits bottom-middle — poor on both axes at low
NoC.

**Exceptions:** `p3a1_0xf` has spillover comparable to bidir (0.0069 vs 0.0102 at NoC=8) while its
coverage is less than half (0.43 vs 0.95). A spillover-only ranking would place it *above* bidir —
exactly the failure mode the pairing is there to prevent.

**Results:**

![Operating point](../../stable/python/spillover_figures/operating_point.png)

**Conclusion:** real e-sets with all prefetchers off dominate every other configuration on both axes
simultaneously. Among configurations available to a JS victim, bidir R1 at `0xf` is the best
compromise.

---

## 5. Per-cluster structure at NoC=64

**Why:** the period-4 anomaly shows up in self-eviction, coverage and cleansing. Does it appear on
the spillover axis, and with which sign?

**Axes:** x = cluster index at NoC=64 (= line offset within page), y = subtracted spillover.

**Trends:** the baseline shows the pattern **inverted** — period-4 clusters spill 0.0057 versus
0.0119 for the rest (ratio 0.48). The clusters that fail to evict their own sets also spill less.
`native_shuffled` (1.00) and `bidirR4` (0.98) are flat.

**Exceptions:** none; the inversion is the expected direction if those clusters simply generate less
eviction overall, which is what `Correlation_Analysis.md` concluded from the other side.

**Results:**

![Per-cluster spillover at NoC=64](../../stable/python/spillover_figures/percluster_noc64.png)

**Conclusion:** spillover corroborates the shared-pressure reading — the anomalous clusters are
under-active in every direction at once, not trading target eviction for collateral eviction.

---

## Bottom line

1. **Prefetchers dominate spatial specificity.** A contiguous sweep with prefetchers on evicts up to
   49% of the non-target space; disabling all prefetchers drops it 60–300×. Disabling only the
   adjacent-line prefetcher (`0x2`) does not help — consistent with the L2 streamer being responsible.
2. **This reverses the earlier ranking.** Real e-sets looked strictly superior under coverage and
   cleansing; on spillover at `pref0x0` they are 3× *worse* than the lazy map with bidir. Their
   advantage is conditional on `0xf`, where it is real and large (20–50×).
3. **Shuffling is worth 10×** in spillover at NoC=2 — the first direct measurement of what the
   shuffled traversal actually buys.
4. **Bidir R=1 is the efficient point**: lowest spillover of any R, identical coverage, cheapest.
5. **The raw form is noise-dominated at low NoC** (86–93% at NoC=2). Only the subtracted form is
   comparable across conditions.

**Open:** the lazy map's residual 20–50× specificity deficit versus real e-sets at `pref0xf` is
unexplained. Real e-sets with a bidirectional sweep were never collected, which would show whether
that deficit is a property of the mapping or of the access pattern.
