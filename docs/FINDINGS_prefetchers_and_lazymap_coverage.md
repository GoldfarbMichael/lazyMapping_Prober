# Findings — Self-eviction, prefetchers, and lazy-map coverage

Documentation and conclusions for the arc that began with the direct self-eviction (PMU)
experiment and ends with prefetcher-controlled coverage. Supersedes the working notes in the
(now deleted) `HANDOFF_selfeviction_correlation.md`; the still-valid facts from it are folded in
below. Companion measurement note: `docs/SELFEVICT_PMU_experiment.md`.

**Machine:** Intel i7-9700K (Coffee Lake / Skylake-client), **inclusive** LLC, 16384 sets ×
12 ways × 8 slices, no hyper-threading. All coverage/self-eviction data is browser-free
(native C, `CoverageValidator`), single core, invariant-TSC timing.

---

## 0. Definitions and knobs

- **Cluster (lazy map):** lines grouped by page-offset cache-index bits. `cluster = pa[shift..11]`
  with `shift = 12 − log2(NoC)`. So NoC=64 → bits **6–11** (bit 6 is the cluster LSB); NoC=32 →
  bits **7–11** (bit 6 is *within* a cluster); etc.
- **Coverage:** prime a Mastik set (12 ways) → victim sweeps one lazy cluster once → probe.
  Coverage = mean over sets of `min(victim_lines_reaching_the_set, 12)/12` (min over samples).
  High = the victim evicts the primed lines.
- **`M_self` (self-eviction, PMU):** per cluster, flush → warm sweep(s) → a PMU-bracketed
  (`MEM_LOAD_RETIRED.L3_MISS`, demand-only) measured sweep. `M_self/node` = fraction of the
  cluster's own lines that were evicted by the cluster's own accesses. `M_cold/node ≈ 1.0` is the
  counter-sanity check (every line misses on the cold pass). All selfevict data uses `WARMPASSES=10`.
- **Prefetcher control — MSR `0x1a4` (`MSR_MISC_FEATURE_CONTROL`), a SET bit DISABLES a prefetcher:**
  bit0 = L2 streamer, bit1 = **L2 adjacent-line**, bit2 = L1 DCU, bit3 = L1 DCU-IP.
  Masks used: `0x0` all ON · `0x2` adjacent-line OFF · `0xf` all four OFF.
  (Warning: `0x1AD` is `TURBO_RATIO_LIMIT` — do **not** confuse with `0x1A4`.)
- **Data trees** under `stable/data/coverage/`:
  - Coverage: `native` (contiguous real e-sets), `native_shuffled` (scattered real e-sets),
    `native_jsmap_shuffled_p1a1_same` (lazy victim), each also `_pref0x2` / `_pref0xf`.
    The runner (`run_coverage_native.sh`) reads `rdmsr -a 0x1a4` and tags the tree `_pref0x<val>`.
  - Self-eviction: `selfevict_shuffled_pref{0x0,0x1,0x2,0xf}`, 50 samples/NoC.
  - **Coverage trees are n=2** — effect sizes are large but firm the numbers up before publishing.
- Reproduce every table: `cd stable && python3 python/coverage_compare.py`.

---

## 1. The adjacent-line prefetcher is the "cold save" (solid)

`M_cold/node` (cold-pass miss fraction; <1 means the prefetcher already fetched the line):

```
NoC   0x0(on)  0x1(str)  0x2(adj)  0xf(off)
 8     0.791    0.796     0.998     0.999
32     0.796    0.799     0.997     0.999
64     0.999    0.999     0.999     0.999
```

The <1 plateau at NoC ≤ 32 is **entirely the L2 adjacent-line prefetcher** (`0x2` removes it,
`0x1` does nothing). At NoC=64 there was never a save, because bit 6 (the adjacent-line pairing
bit — the two 64 B lines of a 128 B pair differ only in address bit 6) is the cluster LSB, so the
buddy lands in a *different* cluster. This single bit-6/cluster-boundary fact recurs everywhere below.

Grounding: adjacent-line = 128 B pair (Intel Optimization Manual; Bertschi, *Battling the
Prefetcher: Coffee Lake*). Replacement is Skylake QLRU / non-LRU insertion (Briongos et al.,
*Reload+Refresh*, USENIX 2020; Vila et al., *Finding Eviction Sets*, S&P 2019).

---

## 2. Self-eviction vs coverage — the "sign-flip" was a cross-condition artifact (resolved)

`M_self/node` across masks:

```
NoC   0x0(on)  0x1(str)  0x2(adj)  0xf(off)
 8     0.336    0.360     0.233     0.252
16     0.325    0.324     0.215     0.245
32     0.329    0.315     0.442     0.417
64     0.436    0.418     0.455     0.433
```

The adjacent-line prefetcher (`0x2`) **masks** the self-eviction structure at NoC=32. With it ON,
`M_self` is flat across cluster parity; with it OFF the odd/even split appears (bit 7 is the NoC=32
cluster LSB):

```
NoC=32   mask            even    odd    odd−even   r(M_self, 1−cov_ON)
         0x0 all ON      0.328  0.331    +0.004        −0.086   (masked)
         0x2 adj OFF     0.201  0.682    +0.481        +0.961   (cross-condition!)
         0xf all OFF     0.198  0.637    +0.440        +0.972
```

The old handoff headline "`+0.94` at NoC=32" is **cross-condition** — it paired prefetch-OFF
`M_self` with prefetch-**ON** coverage. Measuring **both** prefetch-OFF (adjacent-line off) gives the
in-condition correlation, and the sign flips:

```
IN-CONDITION (adj-line OFF): cov_OFF & M_self_off
NoC  cov_ON  cov_OFF  d(OFF−ON)  M_self_off  r_percluster(M_self, 1−cov_off)
 8   0.965   0.275    −0.690     0.233       −0.206
16   0.961   0.261    −0.700     0.215       −0.456
32   0.836   0.490    −0.347     0.442       −0.956
64   0.580   0.567    −0.012     0.455       −0.953
```

**Conclusion:** in-condition, self-eviction *opposes* the coverage drop at both NoC=32 (−0.96) and
NoC=64 (−0.95) — **no sign flip.** Self-eviction is a *marker of set pressure*: a cluster that
overfills its sets (self-evicts) is exactly one that evicts the primed lines. The handoff's "sign
flip" open question is **retracted.** The falsified "uneven distribution" theory (fixed-set test)
stays falsified and is not used here.

### Parity polarity inverts with the prefetcher (verified)

```
NoC=32  pref-ON : cov even(bit7=0)=0.959  odd=0.714   | M_self_off even=0.201 odd=0.682
NoC=32  pref-OFF: cov even       =0.269  odd=0.711   | (odd unchanged; even collapses)
```

Odd-cluster coverage is unchanged (0.714 → 0.711); only the **even** clusters collapse
(0.959 → 0.269) when the adjacent-line prefetcher is removed. Odd clusters evict on their own
(high `M_self`, high coverage, prefetch-independent); even clusters barely self-evict and are
propped up to 0.96 by the prefetcher.

---

## 3. Prefetch-OFF coverage inverts the NoC trend (solid)

Coverage, lazy victim, prefetch-ON vs adjacent-line-OFF:

```
             NoC8   NoC16  NoC32  NoC64
jsmap ON     0.965  0.961  0.836  0.580     (falls with NoC)
jsmap 0x2    0.275  0.261  0.490  0.567     (rises with NoC)  <- inverted; converges at 64
```

The high low-NoC coverage under prefetch-ON is **manufactured by the adjacent-line prefetcher**.
It converges at NoC=64 (Δ = −0.012) because there bit 6 is cluster-defining → the buddy is
cross-cluster → the prefetcher is inert. Same bit-6 boundary as §1.

---

## 4. The scattered-eset "policy" drop is a prefetcher artifact, not replacement policy (solid, revises prior finding)

Real (guaranteed-12/set) e-sets across masks:

```
mask / victim        NoC8   NoC16  NoC32  NoC64
0x0  contiguous      0.999  0.998  0.996  0.971
0x0  scattered       0.996  0.978  0.908  0.718
0x2  scattered       0.945  0.964  0.855  0.768
0xf  scattered       0.899  0.959  0.948  0.966   <- flat; NoC-drop gone
```

**Scatter penalty (contiguous − scattered):**

```
mask     NoC8    NoC16   NoC32   NoC64
0x0     +0.003  +0.020  +0.088  +0.252
0x2     +0.050  +0.030  +0.134  +0.209
0xf     +0.086  +0.026  +0.038  +0.017   <- ~0, no NoC scaling
```

With **all** prefetchers off, scattering a full 12/set e-set costs almost nothing at any NoC.
The NoC-scaling drop of `native_shuffled` survives `0x2` (adjacent-line off) but vanishes at `0xf`,
so it is caused by a prefetcher **other than adjacent-line** — the **L2 streamer or an L1
prefetcher** (bits 0/2/3) — not by the replacement policy.

**This revises the project's earlier "policy, not membership" conclusion.** That conclusion rested
on the control *"native_shuffled has 12/set and still drops → policy."* That drop is a prefetcher
artifact, so the control is invalid: **for full eviction sets there is no replacement-policy wall.**

---

## 5. What limits lazy-map coverage: membership + insertion-policy dynamics

**Membership gap (scattered real e-set − lazy victim, same mask)** — isolates the lazy victim's
*statistical* filling (mean 12/set, but a Poisson tail of under-filled sets) from everything else:

```
mask    NoC8    NoC16   NoC32   NoC64
0x0    +0.031  +0.017  +0.072  +0.139    (small: adjacent-line prefetcher masks the deficit)
0x2    +0.670  +0.703  +0.365  +0.200    (large: deficit exposed once adjacent-line is off)
```

The lazy victim's coverage deficit vs full e-sets is **membership** (under-filled sets), visible
only when the adjacent-line prefetcher (which tops up under-filled sets via buddies) is disabled.

**But membership does not explain the NoC dependence** of the lazy prefetch-off coverage — see §6.

---

## 6. OPEN: why does small-NoC lazy coverage crater with prefetchers off?

Observation (user): prefetch-off lazy coverage is *lower* at small NoC than at NoC=64, where one
might expect them to be similar.

```
NoC  cov_0x2  M_self_0x2  lines/cluster  sets/cluster  lines/set
 8   0.275    0.233         24576          2048          12
16   0.261    0.215         12288          1024          12
32   0.490    0.442          6144           512          12
64   0.567    0.455          3072           256          12
```

**This is NOT a membership-occupancy effect.** Per-set occupancy is **exactly 12 at every NoC**
by construction, and the balls-in-bins occupancy *distribution* is NoC-invariant (occupancy of a
set is the count of victim pages in its `pa[12..19]` bucket, independent of NoC). So static
membership predicts identical coverage across NoC — it does not.

Instead, coverage and `M_self` both **rise as the sweep shortens** (higher NoC). This points to the
**scan-resistant L3 insertion policy (QLRU/RRIP)** as the leading hypothesis:

- A single lazy sweep is a streaming scan. Under scan-resistant insertion, newly filled lines get a
  distant re-reference prediction and are evicted quickly.
- **Longer sweep (low NoC, 24576 lines)** ⇒ more strongly treated as a scan ⇒ the victim's own lines
  to a set are evicted before they accumulate ⇒ they neither self-evict (`M_self` low, 0.23) nor
  displace the primed lines (**coverage low, 0.28**).
- **Shorter sweep (high NoC, 3072 lines)** ⇒ less scan-like ⇒ victim lines accumulate (`M_self` high,
  0.46) and evict the primed lines (**coverage high, 0.57**).

The adjacent-line prefetcher counteracts this at low NoC by doubling the deposits (buddy lines,
in-cluster only for NoC ≤ 32), overwhelming the scan-resistance — which is exactly why prefetch-ON
low-NoC coverage is high and prefetch-OFF craters. This is a **hypothesis**, consistent with the
`M_self`↔coverage trend and the occupancy-invariance argument, but not yet proven.

### Self-eviction TRACKS coverage — total counts (confirming evidence)

`M_self` is a *marker of set pressure*, not the cause of the drop, so it moves **with** coverage,
not against it. Total self-evictions over a full sweep of all clusters (`0xf`, all prefetchers off;
total nodes swept = the whole 196,608-line buffer at every NoC):

```
NoC   M_self/node   TOTAL M_self   coverage
 8      0.252          49,492       0.28   <- fewest self-evictions AND lowest coverage
64      0.433          85,194       0.57   <- most self-evictions AND highest coverage
```

NoC=64 has ~1.7× **more** total self-eviction than NoC=8 — the opposite of the intuitive "more
policy loss ⇒ more self-eviction." This distinguishes the two models cleanly: if self-eviction
*caused* the drop (victim wasting its lines on itself) it would be highest where coverage is worst
(NoC=8); instead it is lowest there. Self-eviction and coverage are two faces of the *same*
phenomenon — the victim establishing over-capacity presence in a set — which fails at low NoC.

### Reconciling low `M_self` with low coverage (the apparent paradox)

At first this looks contradictory: low `M_self` means the victim's lines mostly *survive* (they were
retained, not evicted), yet coverage is low, meaning the victim *failed to evict* the attacker.
The resolution is that the two experiments have **different set contents**:

- **Selfevict (victim-only, no attacker):** the victim's ~12 lines have the 12-way set to
  themselves — nothing at high priority competes, so they settle in and survive → **low `M_self`**.
  They *are* durably inserted, because there is no incumbent.
- **Coverage (with attacker):** the set starts full of the attacker's 12 primed lines, and the
  prime **promotes** them (RRIP RRPV→0, "near"). The victim's sweep is inserted **scan-resistant**
  (RRPV→2, "distant"), so each new victim line is the first eviction candidate and is knocked out by
  the *next* victim line — the victim churns in the low-priority slot and never dislodges the
  promoted attacker lines → **low coverage**.

So the victim line **is** inserted, but only *transiently* (distant RRPV) and never *retained
against a promoted incumbent*. Your intuition — "it seems it never has been inserted" — is right in
the sense that matters: from the attacker's set it never establishes durable residency. Low `M_self`
(owns an empty set) and low coverage (cannot take a contested set) are both consequences of the same
weak, scan-resistant insertion. The adjacent-line prefetcher's extra buddy insertions add the aging
pressure needed to overcome the promoted incumbent — which is why it rescues low-NoC coverage.

(What remains open is only the *NoC magnitude*: why the effective insertion/aging pressure per set
falls with sweep length even though occupancy is a constant 12/set — see the experiments below.)

**Experiments to settle it:**
1. **jsmap at `0xf`** (all prefetchers off) — the missing cell. `0x2` still leaves the streamer/L1
   prefetchers ON, so part of the low-NoC crater could be the streamer, not pure scan-resistance.
2. **Decouple sweep length from NoC** — sub-sample a low-NoC cluster (sweep only 3072 of its lines)
   or add passes; if coverage tracks sweep *length* rather than NoC, scan-resistance is confirmed.
3. **`0x1` coverage** (streamer off only) — pins whether the streamer is the §4 artifact and a
   contributor here.

---

## 7. Conclusions

1. **No replacement-policy wall for full eviction sets** (§4). With prefetchers off, contiguous and
   scattered 12/set e-sets both evict ~fully at every NoC. The apparent "policy" drop was a prefetcher.
2. **Self-eviction is a coverage *marker*, not a cause** (§2). In-condition it opposes the drop at
   all NoC (≈ −0.95). The sign-flip puzzle was a cross-condition artifact and is retracted.
3. **Lazy-map coverage is limited by two separable things:** (a) a **membership** deficit — a Poisson
   tail of under-filled sets from statistical filling (§5); and (b) **insertion-policy dynamics** —
   scan-resistance that hurts long (low-NoC) sweeps (§6, hypothesis).
4. **The adjacent-line prefetcher masks both** by doubling deposits into (low-NoC, in-cluster) sets;
   this is why native/mock-clock results looked strong at low NoC and why that will **not port to a
   browser**, where MSRs can't be set and the streamer artifact is present.

---

## 8. How to enhance lazy-map coverage

- **Membership (dominant lever):** raise real lines/set without relying on the HW prefetcher.
  - **Buddy-touch (`JSMAP_BUDDY=1`)** = the software adjacent-line prefetcher: demand-access each
    line's 128 B buddy. Recovers the prefetcher's fill at NoC ≤ 32 (buddy in-cluster); no help at
    NoC=64 (buddy cross-cluster). No extra memory.
  - **Double the victim buffer** (12 MB → 24 MB, mean 12 → 24 lines/set). Balls-in-bins: the
    under-filled tail (<12) nearly vanishes, so coverage should rise — concentrated on the sets that
    are currently under-filled. Works at all NoC. Cost: 2× sweep time = lower temporal resolution
    (the central spatio-temporal tradeoff). Note this **reverses** the old "24 MB won't help"
    prediction, which assumed the residual was policy (now shown to be a prefetcher).
- **Insertion-policy dynamics:** if §6 confirms scan-resistance, defeat it with an access pattern
  that promotes the victim's lines (repeat/interleave within a set) rather than a single streaming
  pass — i.e. the eviction-strategy sweep (`sweep_lazy_evict`, the Rowhammer.js sliding window).
- **Methodology (important):** verify any fix at **`0xf` (all prefetchers off)** — that condition
  attributes coverage gains to *real* membership/eviction, since prefetch-ON masks membership. But
  `0xf` is an idealized upper bound: **re-validate at `0x0` and ultimately in the real browser**,
  because the browser can't disable prefetchers and the streamer artifact makes prefetch-ON ≠
  prefetch-OFF (it even *hurts* at high NoC). Use `0xf` to understand, `0x0` to deploy.

---

## 9. Slice-hash aside (unproven, low priority)

At NoC=64 the low-coverage clusters follow the empirical rule **bad ⟺ PA bit7=0 AND bit6=bit11**
(XOR of non-adjacent bits → smells like the Coffee Lake 8-slice complex-addressing hash;
cf. Maurice et al., *Reverse Engineering Intel LLC Complex Addressing*, RAID 2015). Not needed for
the coverage conclusions; would require the slice function to confirm.
