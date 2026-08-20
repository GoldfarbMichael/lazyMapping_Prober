# Correlation Analysis — self-eviction ↔ coverage

Script: `stable/python/correlation_analysis.py`. Figures: `stable/python/correlation_figures/`.
Third phase, after `SelfEviction_Analysis.md` and `Coverage_Analysis.md`. Both were characterised
independently first, so this is a test against two finished datasets rather than a story built
config-by-config.

## 0. Method and caveats

**The two metrics, per cluster.**
- self-eviction: `M_self/nodeCount`, mean over 50 iterations, from `selfevict_shuffled_pref<MASK>`.
  No prober present.
- coverage: `diag(min over samples of raw)[c] / 12`, from the coverage trees. n=2 samples.

**Sign convention — this is the whole question.** Both metrics count eviction, measured from
opposite sides of the same sets.
- **positive** correlation ⇒ clusters that self-evict little also fail to evict the prober
  (*shared pressure*: the victim isn't establishing residency there at all).
- **negative** correlation ⇒ the victim's self-eviction consumes its ability to evict the prober
  (*competition*: the two compete for the same ways).

**Scope.** Single-directional 1-pass victim only; bidir excluded from this phase. Per-cluster
correlation only at NoC ≥ 16 (16/32/64 points); NoC 2/4/8 give too few points.

**Matched conditions.** Self-eviction exists at masks 0x0/0x1/0x2/0xf. Coverage `p1a1` (the same
1-pass pattern) exists only at 0x0 and 0x2 — so those two are genuinely matched. **There is no
`p1a1` coverage tree at `0xf`**; the only `0xf` coverage is 3-pass `p3a1`. §3 is therefore
pattern-mismatched and is read only as far as §1 licenses it.

**Caveats, stated once.**
- **Regression dilution**: coverage is n=2, so per-cluster `min_raw` is noisy. Noise in one
  variable biases correlation toward zero — every r here is a **lower bound**.
- **Partial tautology**: both metrics count evictions in the same sets. Some positive correlation
  is mechanical. The non-trivial claim is *which specific clusters* are weak, not that the
  magnitudes track.
- **Protocol mismatch**: self-eviction runs `WARMPASSES=10` warm sweeps then measures one, with no
  prober; coverage runs one victim sweep per probe with the Mastik prober active. Same access
  pattern, different repetition count and different concurrent pressure.
- **Not simultaneous**: separate runs at different times.
- **Multiple comparisons**: many (mask, NoC) cells; individual p-values are not to be
  over-interpreted in isolation.

---

## 1. Pass-count validation (prerequisite for §3)

**Why:** §3's `0xf` comparison pits 1-pass self-eviction against 3-pass coverage. Before reading it,
measure whether pass count moves the *spatial* profile — testable at `0x0`, where both `p1a1` and
`p3a1` exist at every NoC.

**Axes:** per-cluster coverage under `p1a1` (x) vs `p3a1` (y), one panel per NoC, `y = x` reference.

**Trends:** pass count preserves the spatial pattern where a pattern exists.

| NoC | p1a1 mean | p3a1 mean | Pearson r | Spearman ρ |
|---|---|---|---|---|
| 4 | 0.965 | 0.968 | +0.943 | +1.000 |
| 8 | 0.965 | 0.964 | +0.269 | +0.333 |
| 16 | 0.961 | 0.964 | −0.012 | 0.000 |
| 32 | 0.837 | 0.891 | **+0.947** | +0.731 |
| 64 | 0.580 | 0.673 | **+0.810** | +0.508 |

**Exceptions:** at NoC 8/16 the correlation is ~0 — but so is the dynamic range. At NoC=16 all 16
per-cluster values lie in 0.947–0.968 (a spread of 0.02); there is no spatial structure to preserve,
so r≈0 reflects noise, not disagreement.

**Results:**

![Pass-count validation](../../stable/python/correlation_figures/pass_count_validation.png)

**Conclusion:** at NoC 32 and 64 — the only NoC values where either metric has real spatial
structure — 3-pass coverage reproduces the 1-pass group membership (r = 0.95, 0.81). §3's `0xf`
result is licensed **at NoC 32/64 only**, and carries no weight at NoC=16.

---

## 2. Matched per-cluster correlation (1-pass on both sides)

**Why:** the primary test, in conditions where victim access pattern and prefetcher mask agree
on both sides.

**Axes:** per-cluster self-eviction (x) vs per-cluster coverage (y); one panel per (mask, NoC).

**Trends:** every matched cell is **positive**. Correlation is strongest at NoC=64.

| mask | NoC | self-evict mean | coverage mean | Pearson r | Spearman ρ |
|---|---|---|---|---|---|
| 0x0 | 16 | 0.325 | 0.961 | +0.404 | +0.519 |
| 0x0 | 32 | 0.329 | 0.837 | +0.086 | +0.264 |
| 0x0 | 64 | 0.436 | 0.580 | **+0.875** | +0.556 |
| 0x2 | 16 | 0.215 | 0.261 | +0.456 | −0.074 |
| 0x2 | 32 | 0.442 | 0.490 | **+0.956** | +0.720 |
| 0x2 | 64 | 0.455 | 0.567 | **+0.953** | +0.664 |

**Exceptions — and they matter for how the r values should be read:**
- The scatter shows the NoC 32/64 data is **two separated blobs**, not a continuous relationship.
  A Pearson r of +0.95 there is measuring the *gap between two groups*; within the high group there
  is no visible slope. This is why §4 (group overlap) is the statistic to trust, not r.
- `0x0` at NoC=32 (r=+0.086) is the informative exception: coverage is clearly bimodal there, but
  self-eviction is **not** — the split exists in only one of the two metrics. So the correspondence
  is not universal.
- NoC=16 in both masks has negligible dynamic range in coverage; those r values are noise.

**Results:**

![Matched scatter](../../stable/python/correlation_figures/scatter_matched.png)

**Conclusion:** where both metrics have spatial structure, they agree, and the sign is positive —
consistent with shared pressure, not with competition for ways. The one condition where coverage is
structured and self-eviction is not (`0x0`, NoC=32) shows the coupling is not automatic.

---

## 3. Mask `0xf` — pattern-mismatched

**Why:** `0xf` (all prefetchers off) is the regime the coverage work concentrated on, and the
condition where the period-4 pattern was first noticed. No matched coverage tree exists.

**Axes:** as §2. Coverage from `p3a1_pref0xf` (3-pass), self-eviction 1-pass.

**Trends:**

| mask | NoC | self-evict mean | coverage mean | Pearson r | Spearman ρ |
|---|---|---|---|---|---|
| 0xf | 16 | 0.245 | 0.420 | −0.009 | −0.374 |
| 0xf | 32 | 0.417 | 0.684 | **+0.943** | +0.713 |
| 0xf | 64 | 0.433 | 0.825 | **+0.949** | +0.587 |

**Exceptions:** the NoC=16 row is exactly where §1 withheld its licence, and it shows nothing — the
two facts are consistent, but neither supports the other. NoC=16 here is uninformative, not
negative evidence.

**Results:**

![0xf scatter](../../stable/python/correlation_figures/scatter_0xf.png)

Per-cluster profiles at NoC=64, self-eviction above, coverage below, shared x-axis:

![0xf profiles](../../stable/python/correlation_figures/profiles_noc64_pref0xf.png)

The period-4 dropouts fall at the same cluster indices in both panels.

**Conclusion:** at NoC 32/64, `0xf` behaves like the matched conditions — strong positive
correspondence. Given §1, this is a fair reading of the spatial pattern, but a `p1a1_pref0xf`
coverage run would remove the caveat entirely and is the obvious next collection.

---

## 4. Group overlap — the load-bearing result

**Why:** §2/§3's Pearson r are inflated by bimodality. This tests the actual claim directly: are the
*same clusters* in the bottom quartile of both metrics? Set membership, no distributional assumption.

**Axes:** bottom-quartile cluster set of each metric; intersection size, Jaccard, and a
hypergeometric upper-tail p (probability of that much overlap by chance).

**Trends:** at NoC=64 the overlap is near-total in every mask, matched or not.

| mask | NoC | bottom k | overlap | Jaccard | p (hypergeom) | matched |
|---|---|---|---|---|---|---|
| 0x0 | 16 | 4 | 2 | 0.333 | 2.5e−01 | yes |
| 0x0 | 32 | 8 | 2 | 0.143 | 6.7e−01 | yes |
| 0x0 | 64 | 16 | **15** | 0.882 | **1.6e−12** | yes |
| 0x2 | 16 | 4 | 0 | 0.000 | 1.0e+00 | yes |
| 0x2 | 32 | 8 | 5 | 0.455 | 1.2e−02 | yes |
| 0x2 | 64 | 16 | **16** | 1.000 | **2.1e−15** | yes |
| 0xf | 16 | 4 | 0 | 0.000 | 1.0e+00 | NO |
| 0xf | 32 | 8 | 3 | 0.231 | 3.1e−01 | NO |
| 0xf | 64 | 16 | **16** | 1.000 | **2.1e−15** | NO |

Checked directly against the period-4 index set `{0,4,…,28, 33,37,…,61}` from
`SelfEviction_Analysis.md` §3: **self-eviction's bottom-16 is exactly that set at all three masks**;
coverage's bottom-16 matches it exactly at `0x2` and `0xf`, and misses by one cluster at `0x0`.

**Exceptions:** the effect is essentially **NoC=64-specific**. At NoC=16 overlap is at or below
chance everywhere; at NoC=32 only `0x2` is even nominally significant (p=0.012, and that is one cell
among nine). NoC=64 is exactly the resolution at which cluster index equals the line's offset within
its 4KB page.

**Results:** table above.

**Conclusion:** at NoC=64 the two independently-measured datasets identify the same 16 clusters as
weak, at p ≈ 1e−12 to 1e−15. This is not a magnitude correlation that bimodality could manufacture —
it is set identity, and it holds regardless of prefetcher mask.

---

## 5. Aggregate across NoC (underpowered)

**Why:** completeness — does the per-cluster relationship also hold between the NoC-level means?

**Axes:** mean self-eviction vs mean coverage, one point per NoC (4–6 points per condition).

**Trends:**

| mask | points | Pearson r | Spearman ρ |
|---|---|---|---|
| 0x0 | 6 (NoC 2–64) | **−0.553** | +0.029 |
| 0x2 | 4 (NoC 8–64) | +0.987 | +1.000 |
| 0xf | 4 (NoC 8–64) | +0.974 | +1.000 |

**Exceptions:** `0x0` is **negative** at the aggregate level while its per-cluster correlation at
NoC=64 is strongly positive (+0.875). That sign reversal across levels of aggregation is a
Simpson's-paradox pattern: the within-NoC and between-NoC relationships are different things, and
4–6 points cannot distinguish a real reversal from noise.

**Results:** table above.

**Conclusion:** no weight should be placed on this section. It is reported because it was computed,
and because the `0x0` reversal is a caution against quoting a single "self-eviction vs coverage"
correlation without saying at which level it was measured.

---

## Bottom line

Self-eviction and coverage are **positively** related per cluster: clusters that self-evict little
are the same clusters that fail to evict the prober. The relationship is real but narrow — it lives
almost entirely at NoC=64, where the cluster index is exactly the line's offset within its 4KB page.

The strongest form of the result is set identity, not correlation: at NoC=64, the bottom-quartile
clusters of the two independently-collected datasets coincide 15/16, 16/16, 16/16 across masks
`0x0`, `0x2`, `0xf` (p ≈ 1e−12 to 1e−15), and that set is exactly the period-4 pattern from
`SelfEviction_Analysis.md` §3. Prefetcher state does not move it.

The positive sign rules against the "competition for ways" reading — the victim's self-eviction is
not what costs it coverage. Both measurements instead point to the same underlying fact: in those
16 clusters the single-directional sweep fails to establish residency at all, which shows up as
little self-eviction *and* little prober eviction.

That said, this cannot be a hard membership limit: `Coverage_Analysis.md` §5 shows the bidirectional
sweep reaching ~0.95 coverage uniformly across all 64 clusters, including these. The clusters *can*
be evicted; the 1-pass sweep just does not do it.

**Open threads.** (1) Collect `p1a1_pref0xf` coverage to remove §3's pattern-mismatch caveat.
(2) Coverage at n=2 dilutes every r reported here — the true correlations are stronger than shown.
(3) Bidir was excluded from this phase by scope; whether the correspondence survives under bidir
(coverage flattens completely, self-eviction only partially) is the natural next question, and the
data for it already exists.
