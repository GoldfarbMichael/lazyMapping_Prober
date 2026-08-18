"""Correlation between per-cluster self-eviction and per-cluster coverage.

Two datasets, same (mask, NoC, cluster) key, measured from opposite sides of the same sets:

  self-eviction  data/coverage/selfevict_shuffled_pref<MASK>/NoC<NN>/<iii>.csv
                 schema cluster,nodeCount,M_cold,M_self  (one row per cluster, no prober)
                 metric: self_frac[c] = mean over samples of M_self/nodeCount

  coverage       data/coverage/<tree>/NoC<NN>/<iii>.csv
                 schema S0..S16383, then NoC cluster-sweep rows, then 15 idle rows
                 metric: cov[c] = diag(min over samples of raw)[c] / ASSOC

Sign convention: both metrics are "how much eviction happened". A POSITIVE correlation means
clusters that self-evict little also fail to evict the prober (shared-pressure/membership); a
NEGATIVE one means the victim's self-eviction steals from its ability to evict the prober
(competition/policy).

Scope: single-directional 1-pass victim only (bidir deliberately excluded from this phase).
No scipy: Spearman = Pearson on ranks; hypergeometric tail via math.comb.
"""
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Rectangle
import numpy as np
import pandas as pd

STABLE = Path(__file__).resolve().parent.parent
DATA = STABLE / "data" / "coverage"
FIGDIR = Path(__file__).resolve().parent / "correlation_figures"
FIGDIR.mkdir(exist_ok=True)

ASSOC = 12
PAGE_OFFSET_BITS = 12
BASELINE_ROWS = 15
NOC_ALL = [2, 4, 8, 16, 32, 64]
NOC_CORR = [16, 32, 64]   # per-cluster correlation floor: >=16 points

SELFEVICT = "selfevict_shuffled_pref{m}"          # masks 0x0, 0x1, 0x2, 0xf ; _old excluded
COV_P1A1 = {"0x0": "native_jsmap_shuffled_p1a1_same",
            "0x2": "native_jsmap_shuffled_p1a1_same_pref0x2"}
COV_P3A1 = {"0x0": "native_jsmap_shuffled_p3a1_same",
            "0x2": "native_jsmap_shuffled_p3a1_pref0x2",
            "0xf": "native_jsmap_shuffled_p3a1_pref0xf"}

# (mask, coverage tree, pattern-matched?) for the correlation sections
MATCHED = [("0x0", COV_P1A1["0x0"], True), ("0x2", COV_P1A1["0x2"], True)]
UNMATCHED = [("0xf", COV_P3A1["0xf"], False)]   # no p1a1 coverage tree at 0xf exists


# ---------------------------------------------------------------- loaders
_pc_cache, _cov_cache, _se_cache = {}, {}, {}


def phys_clusters(tree, noc):
    """Physical cluster per LLC set: top log2(noc) bits of the PA page offset (lazy_map.c:51,71)."""
    key = (tree, noc)
    if key not in _pc_cache:
        labels = DATA / tree / "set_labels.csv"
        if not labels.is_file():
            _pc_cache[key] = None
        else:
            df = pd.read_csv(labels).sort_values("set_idx")
            pa = df["pa"].apply(lambda s: int(str(s), 16)).to_numpy(dtype=np.int64)
            shift = PAGE_OFFSET_BITS - int(round(np.log2(noc)))
            _pc_cache[key] = (pa >> shift) & (noc - 1)
    return _pc_cache[key]


def coverage_percluster(tree, noc):
    """Per-cluster coverage = diag(min over samples of raw)/ASSOC, or None."""
    key = (tree, noc)
    if key in _cov_cache:
        return _cov_cache[key]
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    paths = sorted(noc_dir.glob("*.csv")) if noc_dir.is_dir() else []
    pc = phys_clusters(tree, noc)
    if not paths or pc is None:
        _cov_cache[key] = None
        return None
    raws = []
    for p in paths:
        data = pd.read_csv(p).to_numpy(dtype=float)
        if data.shape[0] != noc + BASELINE_ROWS:
            raise ValueError(f"{p}: {data.shape[0]} rows, expected {noc + BASELINE_ROWS}")
        cluster_rows = data[:noc]
        raw = np.zeros((noc, noc))
        for g in range(noc):
            mask = pc == g
            if mask.any():
                raw[:, g] = cluster_rows[:, mask].mean(axis=1)
        raws.append(raw)
    out = (np.diag(np.min(raws, axis=0)) / ASSOC, len(paths))
    _cov_cache[key] = out
    return out


def selfevict_percluster(mask, noc):
    """Per-cluster mean M_self/nodeCount over samples, or None."""
    key = (mask, noc)
    if key in _se_cache:
        return _se_cache[key]
    noc_dir = DATA / SELFEVICT.format(m=mask) / f"NoC{noc:02d}"
    paths = sorted(noc_dir.glob("*.csv")) if noc_dir.is_dir() else []
    if not paths:
        _se_cache[key] = None
        return None
    fracs = []
    for p in paths:
        df = pd.read_csv(p).sort_values("cluster")
        node = df["nodeCount"].to_numpy(dtype=float)
        node = np.where(node == 0, np.nan, node)
        fracs.append(df["M_self"].to_numpy(dtype=float) / node)
    out = (np.nanmean(fracs, axis=0), len(paths))
    _se_cache[key] = out
    return out


# ---------------------------------------------------------------- statistics
def pearson(x, y):
    if len(x) < 3 or np.std(x) < 1e-12 or np.std(y) < 1e-12:
        return float("nan")
    return float(np.corrcoef(x, y)[0, 1])


def _rank(a):
    """Average ranks, ties shared (needed for a correct Spearman on tied data)."""
    a = np.asarray(a, dtype=float)
    order = np.argsort(a, kind="mergesort")
    ranks = np.empty(len(a), dtype=float)
    ranks[order] = np.arange(len(a), dtype=float)
    # average tied groups
    _, inv, counts = np.unique(a, return_inverse=True, return_counts=True)
    for g in np.flatnonzero(counts > 1):
        m = inv == g
        ranks[m] = ranks[m].mean()
    return ranks


def spearman(x, y):
    """Spearman rho = Pearson on ranks (no scipy)."""
    if len(x) < 3:
        return float("nan")
    return pearson(_rank(x), _rank(y))


def hypergeom_sf(k, N, K, n):
    """P(X >= k) for drawing n from N with K successes -- upper tail, exact via math.comb."""
    total = math.comb(N, n)
    if total == 0:
        return float("nan")
    hi = min(K, n)
    return float(sum(math.comb(K, i) * math.comb(N - K, n - i) for i in range(k, hi + 1)) / total)


def bottom_set(vals, frac=0.25):
    """Indices of the bottom `frac` of values (ties broken by index; deterministic)."""
    k = max(1, int(round(len(vals) * frac)))
    return set(np.argsort(vals, kind="mergesort")[:k].tolist()), k


def overlap_stats(a, b, frac=0.25):
    """Overlap of the bottom-frac sets of two per-cluster vectors."""
    sa, k = bottom_set(a, frac)
    sb, _ = bottom_set(b, frac)
    inter = len(sa & sb)
    union = len(sa | sb)
    jac = inter / union if union else float("nan")
    p = hypergeom_sf(inter, len(a), k, k)
    return inter, k, jac, p


def pair(mask, cov_tree, noc):
    """Aligned (self_frac, coverage) per-cluster vectors for one condition, or None."""
    se = selfevict_percluster(mask, noc)
    cv = coverage_percluster(cov_tree, noc)
    if se is None or cv is None:
        return None
    se_v, se_n = se
    cv_v, cv_n = cv
    assert len(se_v) == noc, f"selfevict {mask} NoC{noc}: len={len(se_v)} != {noc}"
    assert len(cv_v) == noc, f"coverage {cov_tree} NoC{noc}: len={len(cv_v)} != {noc}"
    return se_v, cv_v, se_n, cv_n


# ---------------------------------------------------------------- 1. pass-count validation
def report_pass_validation():
    """Does pass count change the SPATIAL profile? Licenses (or not) the 0xf proxy in section 3."""
    print("=" * 100)
    print("1. PASS-COUNT VALIDATION -- per-cluster coverage profile, p1a1 vs p3a1 (both mask 0x0)")
    print("   High r/rho => 3-pass preserves the 1-pass spatial pattern => p3a1_pref0xf is a")
    print("   usable proxy for the (missing) p1a1_pref0xf in section 3.")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        a = coverage_percluster(COV_P1A1["0x0"], noc)
        b = coverage_percluster(COV_P3A1["0x0"], noc)
        if a is None or b is None:
            continue
        rows.append({"noc": noc, "n_clusters": noc,
                     "p1a1_mean": round(a[0].mean(), 4), "p3a1_mean": round(b[0].mean(), 4),
                     "pearson_r": round(pearson(a[0], b[0]), 4),
                     "spearman_rho": round(spearman(a[0], b[0]), 4)})
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))
    for ax, noc in zip(axes, NOC_CORR):
        a = coverage_percluster(COV_P1A1["0x0"], noc)
        b = coverage_percluster(COV_P3A1["0x0"], noc)
        if a is None or b is None:
            continue
        ax.scatter(a[0], b[0], s=22, color=plt.get_cmap("viridis")(0.35))
        lo = min(a[0].min(), b[0].min())
        hi = max(a[0].max(), b[0].max())
        ax.plot([lo, hi], [lo, hi], "--", color="0.6", linewidth=1, label="y = x")
        ax.set_xlabel("per-cluster coverage, p1a1 (1 pass)")
        ax.set_ylabel("per-cluster coverage, p3a1 (3 passes)")
        ax.set_title(f"NoC={noc}")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle("Pass count shifts coverage but does it move the spatial pattern? (mask 0x0)")
    fig.tight_layout()
    out = FIGDIR / "pass_count_validation.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    return df


# ---------------------------------------------------------------- 2/3. correlation tables
def report_correlation(conditions, title, note):
    print("=" * 100)
    print(title)
    print(note)
    print("=" * 100)
    rows = []
    for mask, cov_tree, matched in conditions:
        for noc in NOC_CORR:
            pr = pair(mask, cov_tree, noc)
            if pr is None:
                continue
            se_v, cv_v, se_n, cv_n = pr
            rows.append({
                "mask": mask, "noc": noc, "n_clusters": noc,
                "se_n": se_n, "cov_n": cv_n,
                "self_frac_mean": round(np.nanmean(se_v), 4),
                "coverage_mean": round(cv_v.mean(), 4),
                "pearson_r": round(pearson(se_v, cv_v), 4),
                "spearman_rho": round(spearman(se_v, cv_v), 4),
                "matched": "yes" if matched else "NO (3-pass cov)",
            })
    df = pd.DataFrame(rows)
    print(df.to_string(index=False) if len(df) else "  (no data)")
    print()
    return df


def plot_scatter(conditions, fname, suptitle):
    conds = [(m, t, mt) for m, t, mt in conditions]
    fig, axes = plt.subplots(len(conds), len(NOC_CORR),
                             figsize=(5.2 * len(NOC_CORR), 4.4 * len(conds)),
                             squeeze=False)
    for i, (mask, cov_tree, matched) in enumerate(conds):
        for j, noc in enumerate(NOC_CORR):
            ax = axes[i][j]
            pr = pair(mask, cov_tree, noc)
            if pr is None:
                ax.set_visible(False)
                continue
            se_v, cv_v, _, _ = pr
            ax.scatter(se_v, cv_v, s=26, color=plt.get_cmap("viridis")(0.3 + 0.35 * i))
            ax.set_title(f"mask {mask}, NoC={noc}", fontsize=10)
            ax.set_xlabel("self-eviction  M_self/nodeCount")
            ax.set_ylabel("coverage (worst-case)")
            ax.grid(alpha=0.3)
    fig.suptitle(suptitle)
    fig.tight_layout()
    out = FIGDIR / fname
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------- 4. group overlap
def report_overlap(all_conditions):
    print("=" * 100)
    print("4. GROUP OVERLAP -- do the same clusters sit in the bottom quartile of BOTH metrics?")
    print("   Bimodality-robust: at NoC=64 a Pearson r is dominated by the 16/48 split, so this")
    print("   tests set membership directly. p = hypergeometric upper tail (chance overlap).")
    print("=" * 100)
    rows = []
    for mask, cov_tree, matched in all_conditions:
        for noc in NOC_CORR:
            pr = pair(mask, cov_tree, noc)
            if pr is None:
                continue
            se_v, cv_v, _, _ = pr
            inter, k, jac, p = overlap_stats(se_v, cv_v, frac=0.25)
            rows.append({"mask": mask, "noc": noc, "bottom_k": k,
                         "overlap": inter, "jaccard": round(jac, 3),
                         "p_hypergeom": f"{p:.2e}",
                         "matched": "yes" if matched else "NO"})
    df = pd.DataFrame(rows)
    print(df.to_string(index=False) if len(df) else "  (no data)")
    print()
    return df


# ---------------------------------------------------------------- 5. aggregate across NoC
def report_aggregate(all_conditions):
    print("=" * 100)
    print("5. AGGREGATE ACROSS NoC -- mean self-eviction vs mean coverage (UNDERPOWERED: 4-6 points)")
    print("=" * 100)
    rows = []
    for mask, cov_tree, matched in all_conditions:
        xs, ys, nocs = [], [], []
        for noc in NOC_ALL:
            pr = pair(mask, cov_tree, noc)
            if pr is None:
                continue
            se_v, cv_v, _, _ = pr
            xs.append(float(np.nanmean(se_v)))
            ys.append(float(cv_v.mean()))
            nocs.append(noc)
        if len(xs) >= 3:
            rows.append({"mask": mask, "n_points": len(xs), "nocs": str(nocs),
                         "pearson_r": round(pearson(np.array(xs), np.array(ys)), 4),
                         "spearman_rho": round(spearman(np.array(xs), np.array(ys)), 4),
                         "matched": "yes" if matched else "NO"})
    df = pd.DataFrame(rows)
    print(df.to_string(index=False) if len(df) else "  (insufficient points)")
    print()
    return df


def plot_profiles(mask, cov_tree, noc=64):
    """Stacked panels sharing the x-axis (NOT a dual-axis chart): self-eviction above, coverage below."""
    pr = pair(mask, cov_tree, noc)
    if pr is None:
        return
    se_v, cv_v, _, _ = pr
    fig, axes = plt.subplots(2, 1, figsize=(14, 7), sharex=True)
    for ax, vals, label, color in [
        (axes[0], se_v, "self-eviction  M_self/nodeCount", plt.get_cmap("viridis")(0.25)),
        (axes[1], cv_v, "coverage (worst-case)", plt.get_cmap("viridis")(0.7)),
    ]:
        for i in range(noc):
            if i % 2 == 1:
                ax.axvspan(i, i + 1, color="0.90", zorder=0)
        h = 0.012 * max(vals.max() - vals.min(), 1e-6)
        for c, v in enumerate(vals):
            ax.add_patch(Rectangle((c, v - h / 2), 1, h, facecolor=color,
                                   edgecolor="none", zorder=10))
        ax.set_xlim(0, noc)
        m = 0.08 * (vals.max() - vals.min())
        ax.set_ylim(vals.min() - m, vals.max() + m)
        ax.set_ylabel(label, fontsize=9)
        ax.grid(alpha=0.2, axis="y", zorder=1)
    axes[1].set_xlabel(f"cluster index at NoC={noc} (= line offset within page)")
    fig.suptitle(f"Per-cluster profiles, mask {mask} — self-eviction (top) vs coverage (bottom)")
    fig.tight_layout()
    out = FIGDIR / f"profiles_noc{noc}_pref{mask}.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def main():
    report_pass_validation()

    m = report_correlation(
        MATCHED,
        "2. MATCHED PER-CLUSTER CORRELATION (1-pass victim on BOTH sides)",
        "   Positive r => shared-pressure/membership; negative => competition/policy.\n"
        "   NOTE: coverage n=2 => per-cluster min is noisy => r is a LOWER BOUND (regression dilution).")
    plot_scatter(MATCHED, "scatter_matched.png",
                 "Matched conditions: per-cluster self-eviction vs coverage (1-pass both sides)")

    u = report_correlation(
        UNMATCHED,
        "3. MASK 0xf -- PATTERN-MISMATCHED (self-eviction 1-pass vs coverage 3-pass)",
        "   No p1a1 coverage tree exists at 0xf. Read only as licensed by section 1.")
    plot_scatter(UNMATCHED, "scatter_0xf.png",
                 "Mask 0xf: per-cluster self-eviction vs coverage (coverage 3-pass)")

    allc = MATCHED + UNMATCHED
    report_overlap(allc)
    report_aggregate(allc)
    for mask, cov_tree, _ in allc:
        plot_profiles(mask, cov_tree, 64)


if __name__ == "__main__":
    main()
