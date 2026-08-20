# Metric Definitions — Coverage, Cleansing Rate, Spillover Rate

Formal definitions of the three per-cluster metrics computed from a single eviction matrix.
Implementation: `stable/python/coverage_analysis.py` (`load_sample`, `_tree_metrics_uncached`).
Raw (non-baseline-subtracted) forms only; the subtracted variants are noted at the end.

## Setup

| symbol | meaning | value here |
|---|---|---|
| $W$ | associativity (ways per set) | 12 |
| $S$ | number of LLC sets | 16384 |
| $N$ | NoC (number of clusters) | 2…64 |
| $M$ | eviction matrix, $M \in \{0,\dots,W\}^{N \times S}$ | — |

$M[c][i]$ = number of ways evicted in LLC set $i$ while the victim swept lazy cluster $c$.
Rows are indexed by the *swept* cluster, columns by *physical set*.

**Cluster map.** $\varphi : \{0,\dots,S-1\} \to \{0,\dots,N-1\}$ assigns each set to a physical
cluster — the top $\log_2 N$ bits of the set's page offset:

$$\varphi(i) \;=\; \left\lfloor \frac{\mathrm{pa}_i}{2^{\,12-\log_2 N}} \right\rfloor \bmod N$$

This is the same partition rule the victim uses in `build_lazy_mapping` (`lazy_map.c:51,71`).
Induced partition and blocks:

$$G_g \;=\; \{\, i : \varphi(i) = g \,\}, \qquad |G_g| \;=\; n \;=\; S/N \quad \text{(equal for all } g\text{)}$$

For row $c$, the sets in $G_c$ are **intended**; all others are **unintended**.

**Aggregated matrix** $A \in \mathbb{R}^{N \times N}$ — mean ways evicted per set, by cluster:

$$A[c][g] \;=\; \frac{1}{n} \sum_{i \in G_g} M[c][i]$$

---

## 1. Coverage

Mean fraction of ways evicted in the victim's *own* cluster.

$$\mathrm{cov}[c] \;=\; \frac{A[c][c]}{W} \;=\; \frac{1}{nW} \sum_{i \in G_c} M[c][i]
\qquad\qquad
\mathrm{Cov} \;=\; \frac{1}{N}\sum_{c=0}^{N-1}\mathrm{cov}[c] \;=\; \frac{\operatorname{tr}(A)}{NW}$$

Range $[0,1]$; $1$ = every way of every own-cluster set evicted. Gives **partial credit**: a set
with 11 of 12 ways evicted contributes $11/12$.

## 2. Cleansing rate

Per-set **binary** indicator, thresholded *before* aggregation. A set counts only if it was
evicted in full.

$$\mathrm{cln}[c] \;=\; \frac{1}{n} \sum_{i \in G_c} \mathbf{1}\!\left[\, M[c][i] \ge W \,\right]
\qquad\qquad
\mathrm{Cln} \;=\; \frac{1}{N}\sum_{c=0}^{N-1}\mathrm{cln}[c]$$

Equivalently, with the cleansing matrix
$C[c][g] = \frac{1}{n}\sum_{i \in G_g}\mathbf{1}[M[c][i] \ge W]$, we have
$\mathrm{Cln} = \operatorname{tr}(C)/N$.

**Not recoverable from $A$.** $A$ has already averaged over sets, and $\mathbf{1}[\cdot]$ does not
commute with the mean — the threshold must be applied to the raw $M$.

## 3. Spillover rate

Evicted lines *outside* the target cluster, normalised by the lines *available* outside it.

$$\mathrm{spill}[c] \;=\; \frac{\displaystyle\sum_{i \notin G_c} M[c][i]}{W\,\bigl(S - |G_c|\bigr)}
\qquad\qquad
\mathrm{Spill} \;=\; \frac{1}{N}\sum_{c=0}^{N-1}\mathrm{spill}[c]$$

**Reduction.** Since $|G_g| = n$ for all $g$, the numerator is $n\sum_{g \ne c} A[c][g]$ and the
denominator is $W n (N-1)$, so

$$\mathrm{spill}[c] \;=\; \frac{1}{W(N-1)}\sum_{g \ne c} A[c][g],
\qquad
\mathrm{Spill} \;=\; \frac{1}{W\,N(N-1)}\sum_{c}\sum_{g \ne c} A[c][g]$$

— the row-wise (resp. overall) **off-diagonal mean of $A$, divided by $W$**. Range $[0,1]$;
$0$ = perfectly specific, $1$ = evicted everything everywhere.

The reduction depends on equal cluster sizes; the implementation asserts
$\min_g |G_g| = \max_g |G_g|$ before computing it.

---

## Properties

**Cleansing ≤ Coverage, per cluster.** For $x \in [0,W]$ we have $\mathbf{1}[x \ge W] \le x/W$
(it is $0 \le x/W$ when $x < W$, and $1 = W/W$ at $x = W$). Averaging over $i \in G_c$ preserves
the inequality:

$$\mathrm{cln}[c] \;\le\; \mathrm{cov}[c] \quad \forall c
\qquad\Longrightarrow\qquad \mathrm{Cln} \le \mathrm{Cov}$$

This is a sanity check in the code — a violation indicates a pipeline bug, not a finding.

**Coverage and Spillover are independent axes.** Coverage reads only $\operatorname{diag}(A)$;
Spillover reads only the off-diagonal. Neither constrains the other. They form a
true-positive / false-positive pair, which is why neither is meaningful reported alone.

**Relation to diagonal mass.** $\mathrm{diag\_mass} = \operatorname{tr}(A) / \sum_{c,g} A[c][g]$
normalises the off-diagonal by *total signal*; Spillover normalises it by *available space*.
They can therefore disagree, and the disagreement is informative.

**Degenerate point.** $M \equiv 0$ yields $\mathrm{Cov} = \mathrm{Cln} = \mathrm{Spill} = 0$ —
optimal spillover, useless attacker. Spillover must always be read next to coverage.

---

## Multi-sample aggregation

For $K$ samples with matrices $M^{(1)},\dots,M^{(K)}$, compute $A^{(k)}$ and $C^{(k)}$ per sample,
then aggregate **elementwise**. The direction flips with the metric's polarity:

$$A_{\min}[c][g] = \min_k A^{(k)}[c][g], \qquad
C_{\min}[c][g] = \min_k C^{(k)}[c][g], \qquad
\mathrm{spill}_{\max}[c] = \max_k \mathrm{spill}^{(k)}[c]$$

$$\mathrm{Cov}_{\min} = \frac{\operatorname{tr}(A_{\min})}{NW}, \qquad
\mathrm{Cln}_{\min} = \frac{\operatorname{tr}(C_{\min})}{N}, \qquad
\mathrm{Spill}_{\max} = \frac{1}{N}\sum_c \mathrm{spill}_{\max}[c]$$

Coverage and cleansing are higher-is-better, so their worst case over samples is $\min$.
Spillover is lower-is-better, so its worst case is $\max$ — using $\min$ there would report the
flattering sample.

---

## Baseline-subtracted variants

Each sample's CSV carries 15 idle rows after the $N$ cluster-sweep rows. With
$b[g] = \frac{1}{n}\sum_{i \in G_g} \bar{M}_{\text{idle}}[i]$ the per-cluster idle floor
($\bar{M}_{\text{idle}}$ = elementwise mean of the idle rows), the subtracted matrix is

$$\tilde{A} \;=\; \max\!\left(A - \mathbf{1}\,b^{\top},\; 0\right)$$

Substituting $\tilde{A}$ for $A$ gives the baseline-subtracted forms. Spillover is reported both
ways: the raw form includes the idle noise floor, the subtracted form isolates victim-caused
spillover. The gap between them is large at low NoC (at NoC=2 roughly 93% of raw spillover is
noise), so cross-condition comparisons should use the subtracted form.
