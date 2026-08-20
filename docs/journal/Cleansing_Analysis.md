# Cleansing Rate Analysis

Script: `stable/python/cleansing_analysis.py`. Figures: `stable/python/cleansing_figures/`.
Formal definition: `Metrics_Definitions.md`.

**Cleansing rate** = fraction of a cluster's own sets that were evicted **in full** (all 12 ways).
Strict companion to coverage: an 11-of-12-way set scores 0.92 under coverage and **0** under
cleansing. The threshold is applied per set and per sample *before* any cluster aggregation, so it
cannot be recovered from a set-averaged matrix — it needs the raw 16384 columns.

## 0. Method and caveats

- `cleansing_min` / `cleansing_mean` = worst-case vs mean-case sample. Cleansing is
  higher-is-better, so its worst case is the **min** over samples (same convention as coverage).
- **n = 2 samples** for every tree here, so the min-over-2 is a fragile worst case; `cleansing_mean`
  is reported alongside and the two track within ~0.02–0.03 throughout.
- Untagged tree name ⇒ `pref0x0` (all prefetchers on). Masks: `0x0` all on, `0x2` L2 adjacent-line
  off, `0xf` all off.
- `_same` is inert for `p*a1` trees (`lazy_map.c:135-141`), so `p3a1_same ≡ p3a1`.

**Sanity** (52 cells): 0 values outside [0,1]; 0 cases of `cleansing_min > cleansing_mean`;
0 cases of `cleansing_min > coverage_min` — the last is guaranteed by construction
(`1[x≥W] ≤ x/W`), so a violation would mean a pipeline bug, not a finding.

---

## 1. Lazy-map baseline across the prefetcher ladder

**Why:** establish how the strict metric behaves for the JS-faithful victim, and how much the
all-or-nothing threshold changes the picture coverage gave.

**Axes:** x = NoC (log2), y = cleansing rate, one line per prefetcher mask, two panels
(`p3a1`, `p1a1`).

**Trends:** the prefetcher state does not merely shift the curve — it inverts it, far more sharply
than coverage showed.

| p3a1 | NoC2 | NoC4 | NoC8 | NoC16 | NoC32 | NoC64 |
|---|---|---|---|---|---|---|
| `0x0` | 0.795 | 0.848 | **0.823** | 0.819 | 0.690 | 0.399 |
| `0x2` | — | — | 0.058 | **0.019** | 0.303 | 0.430 |
| `0xf` | — | — | **0.025** | 0.026 | 0.402 | 0.575 |

At NoC=8 the same victim scores 0.823 with prefetchers on and 0.025 with them off — a **33×
swing from prefetcher state alone**. With prefetchers off at NoC 8/16, essentially *no* set is ever
completed: the sweep evicts roughly half the ways across many sets and finishes almost none.
`p1a1` behaves the same way (0.832 at `0x0` NoC=8 vs 0.028 at `0x2`).

**Exceptions:** the coverage→cleansing gap is itself mask-dependent — 0.11–0.15 at `0x0` but
0.24–0.41 at `0x2`/`0xf`. So prefetchers-off does not just lower the score; it changes the *shape*
of the eviction distribution, spreading partial evictions across more sets rather than completing
fewer.

**Results:**

![Lazy-map baseline cleansing](../../stable/python/cleansing_figures/baseline_trend.png)

**Conclusion:** the low-NoC collapse reported from coverage (0.43) is far more severe than coverage
implied (0.025), but it is **specific to prefetchers-off**. Any statement of the form "the baseline
fails at low NoC" must name the prefetcher mask.

---

## 2. Real Mastik eviction sets — guaranteed membership

**Why:** the lazy map fills sets statistically; real Mastik e-sets have a guaranteed 12 lines per
set. Comparing them separates "not enough lines present" from "lines present but not evicted".

**Axes:** x = NoC, y = cleansing rate, one line per mask, two panels (contiguous vs shuffled sweep
order), with the lazy `p3a1_0xf` baseline overlaid.

**Trends:** real e-sets cleanse 0.54–0.996 everywhere — an order of magnitude above the lazy map at
low NoC (0.980 vs 0.025 at NoC=8, `0x0` vs `0xf` aside).

| NoC | cont `0x0` | cont `0xf` | shuf `0x0` | shuf `0xf` | lazy `p3a1_0xf` |
|---|---|---|---|---|---|
| 8 | 0.993 | 0.882 | 0.980 | 0.701 | 0.025 |
| 16 | 0.983 | 0.867 | 0.927 | 0.793 | 0.026 |
| 32 | 0.964 | 0.862 | 0.760 | 0.749 | 0.402 |
| 64 | 0.796 | 0.834 | 0.543 | 0.752 | 0.575 |

**Exceptions:** the two victim types have **opposite NoC dependence**. Real e-sets *decline* with
NoC (contiguous `0x0`: 0.993 → 0.796); the lazy map *rises* (0.025 → 0.575). Also, at NoC=64
scattered, prefetchers-off is now the *better* configuration (0.752 vs 0.543) — the flip again,
and it reaches the real e-sets too, so it is not a lazy-map artifact.

**Results:**

![Real e-set cleansing](../../stable/python/cleansing_figures/native_trend.png)

**Conclusion:** the lazy map's low-NoC deficit is enormous against guaranteed membership — a factor
of ~40 at NoC=8. This is the clearest evidence yet that the statistical filling, not the probe, is
what fails there.

---

## 3. Bidirectional sweep

**Why:** bidir was the access pattern that fixed coverage. Does it fix the strict metric too, and
how does it compare to guaranteed membership?

**Axes:** x = NoC, y = cleansing rate, one line per R, two panels (`0x0`, `0xf`), with the lazy
baseline and `native_shuffled_0xf` overlaid.

**Trends:** at `pref0xf`, bidir is **flat at 0.74–0.80 across every NoC and every R**.

| NoC | R1 `0xf` | R2 `0xf` | R4 `0xf` | R8 `0xf` | baseline `0xf` | native_shuf `0xf` |
|---|---|---|---|---|---|---|
| 8 | 0.768 | 0.764 | 0.782 | 0.797 | 0.025 | 0.701 |
| 16 | 0.753 | 0.760 | 0.782 | 0.771 | 0.026 | 0.793 |
| 32 | 0.775 | 0.737 | 0.785 | 0.762 | 0.402 | 0.749 |
| 64 | 0.758 | 0.777 | 0.745 | 0.781 | 0.575 | 0.752 |

**Exceptions — the important one:** bidir **matches or exceeds** `native_shuffled_0xf` at every NoC,
and beats it outright at NoC=8 (0.78–0.80 vs 0.701). That means `native_shuffled` is **not a
ceiling**: it has guaranteed membership but runs a *plain* single-directional sweep, so it hits the
same insertion-policy limit the lazy map does. The real ceiling would be real e-sets *plus* bidir,
which was never collected.

At `pref0x0` bidir is worse and R-dependent (R1 falls to 0.465 at NoC=64 while R8 reaches 0.805) —
the flat behaviour is a prefetchers-off property.

**Results:**

![Bidir cleansing](../../stable/python/cleansing_figures/bidir_trend.png)

**Conclusion:** bidir raises the lazy map from 0.025 to ~0.78 at NoC=8 and removes the NoC
dependence entirely. Against the matched (scattered, guaranteed-membership) reference it is at or
above parity — the statistical filling is not the binding constraint once the access order is right.

---

## 4. Period-4 structure at NoC=64

**Why:** `SelfEviction_Analysis.md` §3 found 16 of 64 clusters anomalously weak in a period-4
pattern and left the cause open (addressing vs. harness). Cleansing has much higher contrast than
coverage, and real e-sets provide the control that settles it.

**Axes:** x = cluster index at NoC=64 (= line offset within its 4KB page), y = per-cluster cleansing,
floating value blocks, three victims at `pref0xf`.

**Trends:** the pattern is present only in the lazy map, and cleansing amplifies it ~7× over coverage.

| victim | period-4 set | other clusters | contrast |
|---|---|---|---|
| lazy `p3a1_0xf` | 0.050 | 0.750 | **15.1×** |
| lazy `p1a1_0x0` | 0.041 | 0.442 | **10.8×** |
| lazy `bidirR4_0xf` | 0.747 | 0.745 | 1.0× |
| native contiguous `0xf` | 0.855 | 0.827 | 1.0× |
| native shuffled `0xf` | 0.789 | 0.739 | 0.9× |

(Coverage gives the same baseline comparison only 2.2× contrast — 0.439 vs 0.953.)

Checked across all six native conditions (contiguous/shuffled × `0x0`/`0x2`/`0xf`): contrast ranges
0.88×–1.11×, i.e. **no structure at any prefetcher state**.

**Exceptions:** the pattern appears in the lazy map with prefetchers **on** as well (`p1a1_0x0`,
10.8×), so it is not prefetcher-driven either.

**Results:**

![Per-cluster cleansing at NoC=64](../../stable/python/cleansing_figures/percluster_noc64.png)

**Conclusion:** the period-4 pattern is **absent from real eviction sets at every prefetcher state**
and **present in the lazy map at every prefetcher state**. It is therefore not an LLC addressing or
slice-hash effect — which was the leading unresolved hypothesis — but a property of the lazy map's
statistical filling. It is also not simple under-filling, because `bidirR4` eliminates it on the
same lazy map (0.747 vs 0.745): if those clusters lacked lines, no access order could fix them. The
remaining explanation is an interaction — statistical filling **and** single-directional access
order fail together at those specific page offsets.

---

## Bottom line

The strict threshold changes conclusions rather than just rescaling them.

1. The lazy map's prefetchers-off low-NoC failure is near-total (0.025, not coverage's 0.43), but it
   is prefetcher-specific — the same victim scores 0.823 with prefetchers on.
2. Real eviction sets cleanse 0.54–0.996 throughout, ~40× the lazy map at NoC=8, and have the
   opposite NoC dependence.
3. Bidir reaches ~0.78 flat across all NoC and R at `0xf`, matching or beating the
   guaranteed-membership scattered reference — which is consequently not a ceiling, since it runs a
   plain sweep and inherits the same limit.
4. The period-4 anomaly is a lazy-map property, not addressing: absent in real e-sets at every
   prefetcher state, present in the lazy map at every prefetcher state, and removable by bidir.

**Open:** real e-sets with a bidirectional sweep were never collected — that is the run that would
establish the true ceiling and confirm that access order, not membership, is the binding constraint.
