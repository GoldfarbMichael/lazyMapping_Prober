"""Coverage analysis: prefetcher-disabled baselines, A/D/C sweeps, decoy, 24MB, bidirectional.

Raw data (per sample CSV, data/coverage/<tree>/NoC<NN>/<iii>.csv):
  header S0..S16383 (one column per LLC set), then NoC cluster-sweep rows, then 15 idle rows.
  cell[c][i] = ways evicted (0..ASSOC) in LLC set i while the victim swept lazy cluster c.

Metrics (defined here from the data semantics, not inherited from earlier analysis):
  coverage_min  = diag(min_over_samples(raw)).mean() / ASSOC   worst-case ways evicted, own cluster
  coverage_mean = diag(mean_over_samples(raw)).mean() / ASSOC  mean-case
  diag_mass     = trace(sub)/sum(sub) on baseline-subtracted matrix; random chance = 1/NoC

  spillover rate -- spatial spillover into clusters the victim did NOT intend to touch:
    per CLUSTER c: sum of evicted lines in every OTHER cluster, divided by the number of lines
                   available outside c (numSets*ASSOC*(1 - 1/NoC))
    whole cache  : mean over clusters
    Reduces algebraically to mean(off-diagonal of the matrix)/ASSOC (clusters are equal-sized).
    0 = perfectly specific, 1 = evicted everything everywhere. Computed BOTH on the raw matrix
    (includes the idle noise floor) and on the baseline-subtracted matrix (victim-caused only).
    NOTE: lower is better, so the worst case over samples is MAX, not min (opposite of coverage).
    NOTE: degenerate alone -- a victim that does nothing scores 0. Read together with coverage:
    spillover is the false-positive rate to coverage's true-positive rate.

  cleansing rate -- strict, all-or-nothing companion to coverage:
    per SET   : 1 if all ASSOC ways were evicted (cell >= 12), else 0
    per CLUSTER: mean of that indicator over the cluster's own sets
    whole cache: mean of the per-cluster rates (= cleansing_min / cleansing_mean below)
  Coverage gives partial credit (a set with 11/12 ways scores 0.92); cleansing does not (it
  scores 0). The threshold is applied per SET and per SAMPLE, BEFORE any cluster aggregation --
  it cannot be recovered from the aggregated `raw` matrix, which has already averaged sets.
  min/mean then refer to the worst-case vs mean-case sample, matching the coverage convention.
"""
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Rectangle
import numpy as np
import pandas as pd

STABLE = Path(__file__).resolve().parent.parent
DATA = STABLE / "data" / "coverage"
FIGDIR = Path(__file__).resolve().parent / "coverage_figures"
FIGDIR.mkdir(exist_ok=True)

ASSOC = 12               # LLC ways; coverage denominator
PAGE_OFFSET_BITS = 12    # 4KiB page -> cluster id lives in the page-offset bits
BASELINE_ROWS = 15       # idle rows appended after the NoC cluster-sweep rows
NOC_ALL = [2, 4, 8, 16, 32, 64]
NOC_EV = [8, 16, 32, 64]  # ev*/bidir* trees were only collected at these

# ---- tree names (verified present on disk) --------------------------------
# Untagged tree name => pref0x0 (prefetchers all on); the _pref tag was added later.
P3A1 = {  # primary baseline ladder; _same is inert at accessesPerLine=1 (lazy_map.c:135-141)
    "0x0": "native_jsmap_shuffled_p3a1_same",
    "0x2": "native_jsmap_shuffled_p3a1_pref0x2",
    "0xf": "native_jsmap_shuffled_p3a1_pref0xf",
}
P1A1 = {  # secondary; no pref0xf tree exists
    "0x0": "native_jsmap_shuffled_p1a1_same",
    "0x2": "native_jsmap_shuffled_p1a1_same_pref0x2",
}
# Real Mastik eviction sets (CoverageValidator mode "native", saved mapping_B victim): membership
# is GUARANTEED ASSOC lines per set, unlike the lazy map's statistical filling. These are the
# reachable ceiling for both metrics. Contiguous vs line-shuffled sweep order over the same sets.
NATIVE_CONT = {"0x0": "native", "0x2": "native_pref0x2", "0xf": "native_pref0xf"}
NATIVE_SHUF = {"0x0": "native_shuffled", "0x2": "native_shuffled_pref0x2",
               "0xf": "native_shuffled_pref0xf"}

EV_PREFIX = "native_jsmap_shuffled_p1a1_ev"
EV_A = [2, 3, 4]
EV_D = [2, 4, 8, 16]
EV_C = [1, 2]
BIG = "native_jsmap_shuffled_p1a1_evA3D3072C3072_pref0xf"
BIG_UNSHUF = "native_jsmap_p1a1_evA3D3072C3072_pref0xf"
DECOY_DOSES = [8, 32, 128, 256, 1024]
DECOY_TREE = "native_jsmap_shuffled_p1a1_evA3D3072C3072_dK{k}_pref0xf"
BUF24 = "native_jsmap_shuffled_p3a1_same_pref0xf_24MB"
BIDIR_R = [1, 2, 4, 8]
BIDIR_TREE = "native_jsmap_shuffled_p1a1_bidirR{r}_pref{p}"
# Config shown in the per-cluster (correlation-ready) figure. R4 rather than the marginally
# higher R8: coverage is equivalent within noise, and R4 costs half the oscillations.
PERCLUSTER_BIDIR = (4, "0xf")

_phys_cache = {}


def phys_clusters(tree, noc):
    """Physical cluster index per LLC set: top log2(noc) bits of the PA's page offset.

    Same partition rule the victim uses in build_lazy_mapping (lazy_map.c:51,71).
    """
    key = (tree, noc)
    if key in _phys_cache:
        return _phys_cache[key]
    labels = DATA / tree / "set_labels.csv"
    if not labels.is_file():
        _phys_cache[key] = None
        return None
    df = pd.read_csv(labels).sort_values("set_idx")
    pa = df["pa"].apply(lambda s: int(str(s), 16)).to_numpy(dtype=np.int64)
    shift = PAGE_OFFSET_BITS - int(round(np.log2(noc)))
    out = (pa >> shift) & (noc - 1)
    _phys_cache[key] = out
    return out


def load_sample(path, noc, pc):
    """One CSV -> (raw, base, clean). Columns aggregated to physical clusters.

    raw[c][g]   = mean ways evicted per set in physical cluster g, while sweeping lazy cluster c.
    base[g]     = same, on the idle rows (noise floor).
    clean[c][g] = FRACTION of g's sets that were FULLY evicted (>= ASSOC ways) -- the cleansing
                  rate. Thresholded per set before aggregation, so it is not derivable from raw.
    """
    data = pd.read_csv(path).to_numpy(dtype=float)
    if data.shape[0] != noc + BASELINE_ROWS:
        raise ValueError(f"{path}: {data.shape[0]} data rows, expected {noc + BASELINE_ROWS}")
    cluster_rows = data[:noc]
    idle_row = data[noc:].mean(axis=0)
    full = cluster_rows >= ASSOC              # per-set binary indicator, before any averaging
    raw = np.zeros((noc, noc))
    base = np.zeros(noc)
    clean = np.zeros((noc, noc))
    for g in range(noc):
        mask = pc == g
        if mask.any():
            raw[:, g] = cluster_rows[:, mask].mean(axis=1)
            base[g] = idle_row[mask].mean()
            clean[:, g] = full[:, mask].mean(axis=1)
    return raw, base, clean


_metrics_cache = {}


def tree_metrics(tree, noc):
    """All metrics for one (tree, noc), or None if absent/empty. Memoized (CSVs are 16384-col)."""
    key = (tree, noc)
    if key in _metrics_cache:
        return _metrics_cache[key]
    _metrics_cache[key] = _tree_metrics_uncached(tree, noc)
    return _metrics_cache[key]


def _tree_metrics_uncached(tree, noc):
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    if not noc_dir.is_dir():
        return None
    paths = sorted(noc_dir.glob("*.csv"))
    if not paths:
        return None
    pc = phys_clusters(tree, noc)
    if pc is None:
        return None
    # Equal cluster sizes are what makes the spillover formula reduce to an off-diagonal mean.
    counts = np.bincount(pc, minlength=noc)
    if counts.min() != counts.max():
        raise ValueError(f"{tree} NoC{noc}: unequal cluster sizes {counts.min()}..{counts.max()}")

    raws, cleans, masses = [], [], []
    spill_raw, spill_sub = [], []
    for p in paths:
        raw, base, clean = load_sample(p, noc, pc)
        raws.append(raw)
        cleans.append(clean)
        sub = np.clip(raw - base[None, :], 0.0, None)
        total = sub.sum()
        masses.append(0.0 if total <= 0 else float(np.trace(sub) / total))
        # spillover: per-cluster off-diagonal mean / ASSOC, before and after baseline subtraction
        spill_raw.append(offdiag_row_mean(raw) / ASSOC)
        spill_sub.append(offdiag_row_mean(sub) / ASSOC)
    raws = np.array(raws)
    cleans = np.array(cleans)
    spill_raw = np.array(spill_raw)
    spill_sub = np.array(spill_sub)
    min_raw, mean_raw = raws.min(axis=0), raws.mean(axis=0)
    min_clean, mean_clean = cleans.min(axis=0), cleans.mean(axis=0)
    return {
        "n": len(paths),
        "coverage_min": float(np.diag(min_raw).mean() / ASSOC),
        "coverage_mean": float(np.diag(mean_raw).mean() / ASSOC),
        "diag_mass": float(np.mean(masses)),
        "per_cluster_min": np.diag(min_raw) / ASSOC,
        "per_cluster_mean": np.diag(mean_raw) / ASSOC,
        # cleansing rate: already a fraction in [0,1], no ASSOC division
        "cleansing_min": float(np.diag(min_clean).mean()),
        "cleansing_mean": float(np.diag(mean_clean).mean()),
        "per_cluster_cleansing_min": np.diag(min_clean),
        "per_cluster_cleansing_mean": np.diag(mean_clean),
        # spillover rate: lower is better, so worst case over samples is MAX (not min)
        "spillover_raw_mean": float(spill_raw.mean(axis=0).mean()),
        "spillover_raw_max": float(spill_raw.max(axis=0).mean()),
        "spillover_sub_mean": float(spill_sub.mean(axis=0).mean()),
        "spillover_sub_max": float(spill_sub.max(axis=0).mean()),
        "per_cluster_spillover_raw": spill_raw.mean(axis=0),
        "per_cluster_spillover_sub": spill_sub.mean(axis=0),
    }


def fmt(x, nd=3):
    return "--" if x is None else f"{x:.{nd}f}"


def offdiag_row_mean(M):
    """Per-row mean of the off-diagonal entries of a square matrix (NoC>=2)."""
    noc = M.shape[0]
    if noc < 2:
        return np.zeros(noc)
    return (M.sum(axis=1) - np.diag(M)) / (noc - 1)


# ---------------------------------------------------------------------------
# 1. Baseline: prefetcher ladder, p3a1 primary + p1a1 secondary
# ---------------------------------------------------------------------------
def report_baseline():
    print("=" * 100)
    print("1. BASELINE -- prefetcher ladder (untagged tree name == pref0x0, all prefetchers ON)")
    print("=" * 100)
    rows = []
    for label, trees in [("p3a1", P3A1), ("p1a1", P1A1)]:
        for pref, tree in trees.items():
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                rows.append({
                    "sweep": label, "pref": pref, "noc": noc, "n": m["n"],
                    "cov_min": round(m["coverage_min"], 4),
                    "cov_mean": round(m["coverage_mean"], 4),
                    "cleanse_min": round(m["cleansing_min"], 4),
                    "cleanse_mean": round(m["cleansing_mean"], 4),
                    "diag_mass": round(m["diag_mass"], 4),
                    "chance": round(1.0 / noc, 4),
                })
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()
    return df


def report_passes_effect():
    """passes=3 vs passes=1 at pref0x2 -- the observation that started the experiment series.

    Both trees are p*a1 so _same is inert (lazy_map.c:135-141); the only difference is passes.
    """
    print("=" * 100)
    print("1b. MULTIPLE PASSES -- p3a1 vs p1a1, both pref0x2 (only difference is passes=3 vs 1)")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        a = tree_metrics(P1A1["0x2"], noc)   # passes=1
        b = tree_metrics(P3A1["0x2"], noc)   # passes=3
        if a is None or b is None:
            continue
        rows.append({
            "noc": noc,
            "p1a1_cov_min": round(a["coverage_min"], 4),
            "p3a1_cov_min": round(b["coverage_min"], 4),
            "delta_min": round(b["coverage_min"] - a["coverage_min"], 4),
            "p1a1_cov_mean": round(a["coverage_mean"], 4),
            "p3a1_cov_mean": round(b["coverage_mean"], 4),
            "delta_mean": round(b["coverage_mean"] - a["coverage_mean"], 4),
        })
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()
    return df


def plot_baseline_trend(df):
    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    for ax, metric, title in [(axes[0], "cov_min", "coverage (worst-case, min over samples)"),
                              (axes[1], "cov_mean", "coverage (mean over samples)")]:
        sub = df[df["sweep"] == "p3a1"]
        for pref in ["0x0", "0x2", "0xf"]:
            s = sub[sub["pref"] == pref].sort_values("noc")
            if s.empty:
                continue
            ax.plot(s["noc"], s[metric], marker="o", label=f"pref{pref}")
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.grid(alpha=0.3)
        ax.set_ylim(0, 1)
    axes[0].set_ylabel("coverage (fraction of 12 ways)")
    axes[0].legend(title="prefetcher mask")
    fig.suptitle("Baseline coverage vs NoC, p3a1 (0x0 = all prefetchers on, 0xf = all off)")
    fig.tight_layout()
    out = FIGDIR / "baseline_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def _value_blocks(ax, series_by_label, colors, noc_width, finest=64):
    """Floating value-blocks (no baseline), matching the self-eviction doc's visual language."""
    for i in range(finest):
        if i % 2 == 1:
            ax.axvspan(i, i + 1, color="0.90", zorder=0)
    all_vals = np.concatenate([v for v in series_by_label.values() if v is not None])
    h = 0.012 * max(all_vals.max() - all_vals.min(), 1e-6)
    for z, (label, vals) in enumerate(series_by_label.items()):
        if vals is None:
            continue
        width = finest / len(vals)
        for c, v in enumerate(vals):
            ax.add_patch(Rectangle((c * width, v - h / 2), width, h,
                                   facecolor=colors[label], edgecolor="none", zorder=10 + z))
    return all_vals


def plot_baseline_percluster():
    """Per-cluster coverage at NoC=64 -- the vector the correlation phase joins against."""
    fig, ax = plt.subplots(figsize=(14, 5))
    cmap = plt.get_cmap("viridis")
    prefs = ["0x0", "0x2", "0xf"]
    colors = {p: cmap(i / (len(prefs) - 1)) for i, p in enumerate(prefs)}
    series = {}
    for p in prefs:
        m = tree_metrics(P3A1[p], 64)
        series[p] = None if m is None else m["per_cluster_min"]
    vals = _value_blocks(ax, series, colors, 64)
    ax.set_xlim(0, 64)
    margin = 0.08 * (vals.max() - vals.min())
    ax.set_ylim(vals.min() - margin, vals.max() + margin)
    ax.set_xlabel("cluster index at NoC=64 (= line offset within page)")
    ax.set_ylabel("per-cluster coverage (worst-case)")
    ax.legend(handles=[Patch(facecolor=colors[p], label=f"pref{p}") for p in prefs],
              title="prefetcher mask", loc="center left", bbox_to_anchor=(1.005, 0.5), fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title("Per-cluster coverage at NoC=64, p3a1 baseline (correlation-ready)")
    fig.tight_layout()
    out = FIGDIR / "baseline_percluster_noc64.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 2. A/D/C eviction-strategy grid (excludes A3D3072C3072 -- that's section 4)
# ---------------------------------------------------------------------------
def report_adc_grid():
    print("=" * 100)
    print("2. A/D/C GRID -- Rowhammer.js-style sliding window, coverage_min")
    print("=" * 100)
    combos = [(a, d, c) for a in EV_A for d in EV_D for c in EV_C]
    grids = {}
    for pref, suffix in [("0x0", ""), ("0xf", "_pref0xf")]:
        mat = np.full((len(combos), len(NOC_EV)), np.nan)
        for i, (a, d, c) in enumerate(combos):
            tree = f"{EV_PREFIX}A{a}D{d}C{c}{suffix}"
            for j, noc in enumerate(NOC_EV):
                m = tree_metrics(tree, noc)
                if m is not None:
                    mat[i, j] = m["coverage_min"]
        grids[pref] = mat
        best = np.nanmax(mat, axis=0)
        print(f"pref{pref}: best coverage_min per NoC {dict(zip(NOC_EV, best.round(3)))}")
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 11), sharey=True)
    vmax = np.nanmax([g for g in grids.values()])
    labels = [f"A{a}D{d}C{c}" for a, d, c in combos]
    for ax, pref in zip(axes, ["0x0", "0xf"]):
        im = ax.imshow(grids[pref], aspect="auto", cmap="viridis", vmin=0, vmax=vmax)
        ax.set_xticks(range(len(NOC_EV)))
        ax.set_xticklabels(NOC_EV)
        ax.set_yticks(range(len(labels)))
        ax.set_yticklabels(labels, fontsize=7)
        ax.set_xlabel("NoC")
        ax.set_title(f"pref{pref} ({'all on' if pref == '0x0' else 'all off'})")
        for i in range(len(combos)):
            for j in range(len(NOC_EV)):
                v = grids[pref][i, j]
                if not np.isnan(v):
                    ax.text(j, i, f"{v:.2f}", ha="center", va="center", fontsize=6,
                            color="white" if v < vmax * 0.6 else "black")
        fig.colorbar(im, ax=ax, fraction=0.046)
    fig.suptitle("A/D/C grid coverage (worst-case). Excludes A3D3072C3072.")
    fig.tight_layout()
    out = FIGDIR / "adc_grid.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    return grids


# ---------------------------------------------------------------------------
# 3. Buffer size: 24MB vs 12MB (both p3a1, prefetchers all off)
# ---------------------------------------------------------------------------
def report_buffer():
    print("=" * 100)
    print("3. BUFFER SIZE -- p3a1 pref0xf, 12MB vs 24MB")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        a = tree_metrics(P3A1["0xf"], noc)
        b = tree_metrics(BUF24, noc)
        if a is None and b is None:
            continue
        rows.append({
            "noc": noc,
            "12MB_cov_min": fmt(a["coverage_min"]) if a else "--",
            "12MB_cov_mean": fmt(a["coverage_mean"]) if a else "--",
            "12MB_n": a["n"] if a else 0,
            "24MB_cov_min": fmt(b["coverage_min"]) if b else "--",
            "24MB_cov_mean": fmt(b["coverage_mean"]) if b else "--",
            "24MB_n": b["n"] if b else 0,
        })
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()
    return df


# ---------------------------------------------------------------------------
# 4. A3D3072C3072: window == exactly one NoC=64 cluster (12*16384/64 = 3072 nodes)
# ---------------------------------------------------------------------------
def report_big_window():
    print("=" * 100)
    print("4. A3D3072C3072 -- sliding window = one NoC=64 cluster (3072 nodes), pref0xf")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        base = tree_metrics(P3A1["0xf"], noc)
        shuf = tree_metrics(BIG, noc)
        unsh = tree_metrics(BIG_UNSHUF, noc)
        rows.append({
            "noc": noc,
            "baseline_p3a1_0xf": fmt(base["coverage_min"]) if base else "--",
            "A3D3072C3072_shuffled": fmt(shuf["coverage_min"]) if shuf else "--",
            "A3D3072C3072_unshuffled": fmt(unsh["coverage_min"]) if unsh else "--",
            "shuf_cov_mean": fmt(shuf["coverage_mean"]) if shuf else "--",
        })
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()
    return df


def report_decoy():
    print("=" * 100)
    print("4b. DECOY -- dose on top of A3D3072C3072, pref0xf")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        m0 = tree_metrics(BIG, noc)
        rec["dK0"] = fmt(m0["coverage_min"]) if m0 else "--"
        for k in DECOY_DOSES:
            m = tree_metrics(DECOY_TREE.format(k=k), noc)
            rec[f"dK{k}"] = fmt(m["coverage_min"]) if m else "--"
        rows.append(rec)
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()

    fig, ax = plt.subplots(figsize=(9, 5))
    cmap = plt.get_cmap("viridis")
    for i, noc in enumerate(NOC_EV):
        xs, ys = [], []
        for k in DECOY_DOSES:
            m = tree_metrics(DECOY_TREE.format(k=k), noc)
            if m is not None:
                xs.append(k)
                ys.append(m["coverage_min"])
        color = cmap(i / (len(NOC_EV) - 1))
        if xs:
            ax.plot(xs, ys, marker="o", color=color, label=f"NoC={noc}")
        m0 = tree_metrics(BIG, noc)
        if m0 is not None:
            ax.axhline(m0["coverage_min"], color=color, linestyle="--", alpha=0.5, linewidth=1)
    ax.set_xscale("log", base=2)
    ax.set_xticks(DECOY_DOSES)
    ax.set_xticklabels(DECOY_DOSES)
    ax.set_xlabel("decoy dose (lines between subcluster windows)")
    ax.set_ylabel("coverage (worst-case)")
    ax.set_title("Decoy dose on A3D3072C3072, pref0xf (dashed = same NoC without decoy)")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    out = FIGDIR / "decoy_dose.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    return df


# ---------------------------------------------------------------------------
# 5. Bidirectional sweep
# ---------------------------------------------------------------------------
def report_bidir():
    print("=" * 100)
    print("5. BIDIRECTIONAL SWEEP -- coverage_min (-- = not collected)")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                rec[f"R{r}_{pref}"] = fmt(m["coverage_min"]) if m else "--"
        rec["base_p3a1_0xf"] = fmt((tree_metrics(P3A1["0xf"], noc) or {}).get("coverage_min"))
        rec["base_p1a1_0x0"] = fmt((tree_metrics(P1A1["0x0"], noc) or {}).get("coverage_min"))
        rows.append(rec)
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()

    print("--- diagonal mass (spatial specificity; random chance = 1/NoC) ---")
    mrows = []
    for noc in NOC_EV:
        rec = {"noc": noc, "chance": round(1.0 / noc, 4)}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                rec[f"R{r}_{pref}"] = fmt(m["diag_mass"]) if m else "--"
        rec["base_p3a1_0xf"] = fmt((tree_metrics(P3A1["0xf"], noc) or {}).get("diag_mass"))
        mrows.append(rec)
    print(pd.DataFrame(mrows).to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    cmap = plt.get_cmap("viridis")
    for ax, pref in zip(axes, ["0x0", "0xf"]):
        for i, r in enumerate(BIDIR_R):
            xs, ys = [], []
            for noc in NOC_EV:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                if m is not None:
                    xs.append(noc)
                    ys.append(m["coverage_min"])
            if xs:
                ax.plot(xs, ys, marker="o", color=cmap(i / (len(BIDIR_R) - 1)), label=f"R={r}")
        bx, by = [], []
        for noc in NOC_EV:
            b = tree_metrics(P3A1["0xf"] if pref == "0xf" else P1A1["0x0"], noc)
            if b is not None:
                bx.append(noc)
                by.append(b["coverage_min"])
        if bx:
            ax.plot(bx, by, marker="s", color="crimson", linestyle="--",
                    label="baseline " + ("p3a1_0xf" if pref == "0xf" else "p1a1_0x0"))
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_EV)
        ax.set_xticklabels(NOC_EV)
        ax.set_xlabel("NoC")
        ax.set_title(f"pref{pref} ({'all on' if pref == '0x0' else 'all off'})")
        ax.grid(alpha=0.3)
        ax.set_ylim(0, 1)
        ax.legend(fontsize=8)
    axes[0].set_ylabel("coverage (worst-case)")
    fig.suptitle("Bidirectional sweep coverage vs NoC")
    fig.tight_layout()
    out = FIGDIR / "bidir_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    return df


def plot_bidir_percluster():
    """Per-cluster coverage at NoC=64: bidir (PERCLUSTER_BIDIR) vs baseline -- correlation-ready."""
    r, pref = PERCLUSTER_BIDIR
    m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), 64)
    if m is None:
        return
    best_label = f"bidirR{r}_{pref}"
    best_vec = m["per_cluster_min"]
    base = tree_metrics(P3A1["0xf"], 64)

    fig, ax = plt.subplots(figsize=(14, 5))
    series = {best_label: best_vec}
    colors = {best_label: plt.get_cmap("viridis")(0.75)}
    if base is not None:
        series["baseline p3a1_0xf"] = base["per_cluster_min"]
        colors["baseline p3a1_0xf"] = plt.get_cmap("viridis")(0.1)
    vals = _value_blocks(ax, series, colors, 64)
    ax.set_xlim(0, 64)
    margin = 0.08 * (vals.max() - vals.min())
    ax.set_ylim(vals.min() - margin, vals.max() + margin)
    ax.set_xlabel("cluster index at NoC=64 (= line offset within page)")
    ax.set_ylabel("per-cluster coverage (worst-case)")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="lower right", fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title(f"Per-cluster coverage at NoC=64: {best_label} vs baseline (correlation-ready)")
    fig.tight_layout()
    out = FIGDIR / "bidir_percluster_noc64.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    print(f"[info] per-cluster figure config: {best_label} "
          f"(coverage_min={m['coverage_min']:.3f}, spread={best_vec.max() - best_vec.min():.3f})")


# ---------------------------------------------------------------------------
# 6. Cleansing rate on the bidirectional sweep
# ---------------------------------------------------------------------------
def report_bidir_cleansing():
    """Cleansing rate = fraction of a cluster's sets evicted in FULL (all 12 ways).

    Strict companion to coverage: coverage gives a 11/12-way set 0.92, cleansing gives it 0.
    """
    print("=" * 100)
    print("6. CLEANSING RATE -- fraction of a cluster's sets FULLY evicted (all 12 ways)")
    print("   whole-cache value = mean over clusters. min/mean = worst-case vs mean-case sample.")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                rec[f"R{r}_{pref}"] = fmt(m["cleansing_min"]) if m else "--"
        rec["base_p3a1_0xf"] = fmt((tree_metrics(P3A1["0xf"], noc) or {}).get("cleansing_min"))
        rec["base_p1a1_0x0"] = fmt((tree_metrics(P1A1["0x0"], noc) or {}).get("cleansing_min"))
        rows.append(rec)
    print("--- cleansing_min (worst-case sample) ---")
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    mrows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                rec[f"R{r}_{pref}"] = fmt(m["cleansing_mean"]) if m else "--"
        rec["base_p3a1_0xf"] = fmt((tree_metrics(P3A1["0xf"], noc) or {}).get("cleansing_mean"))
        rec["base_p1a1_0x0"] = fmt((tree_metrics(P1A1["0x0"], noc) or {}).get("cleansing_mean"))
        mrows.append(rec)
    print("--- cleansing_mean (mean-case sample) ---")
    print(pd.DataFrame(mrows).to_string(index=False))
    print()

    # coverage vs cleansing side by side: how much does the strict threshold cost?
    print("--- coverage_min vs cleansing_min (the price of the all-or-nothing threshold) ---")
    crows = []
    for noc in NOC_EV:
        for label, tree in ([(f"bidirR{r}_0xf", BIDIR_TREE.format(r=r, p="0xf")) for r in BIDIR_R]
                            + [("base_p3a1_0xf", P3A1["0xf"])]):
            m = tree_metrics(tree, noc)
            if m is None:
                continue
            crows.append({"noc": noc, "victim": label,
                          "coverage_min": round(m["coverage_min"], 4),
                          "cleansing_min": round(m["cleansing_min"], 4),
                          "gap": round(m["coverage_min"] - m["cleansing_min"], 4)})
    print(pd.DataFrame(crows).to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    cmap = plt.get_cmap("viridis")
    for ax, pref in zip(axes, ["0x0", "0xf"]):
        for i, r in enumerate(BIDIR_R):
            xs, ys = [], []
            for noc in NOC_EV:
                m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), noc)
                if m is not None:
                    xs.append(noc)
                    ys.append(m["cleansing_min"])
            if xs:
                ax.plot(xs, ys, marker="o", color=cmap(i / (len(BIDIR_R) - 1)), label=f"R={r}")
        bx, by = [], []
        for noc in NOC_EV:
            b = tree_metrics(P3A1["0xf"] if pref == "0xf" else P1A1["0x0"], noc)
            if b is not None:
                bx.append(noc)
                by.append(b["cleansing_min"])
        if bx:
            ax.plot(bx, by, marker="s", color="crimson", linestyle="--",
                    label="baseline " + ("p3a1_0xf" if pref == "0xf" else "p1a1_0x0"))
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_EV)
        ax.set_xticklabels(NOC_EV)
        ax.set_xlabel("NoC")
        ax.set_title(f"pref{pref} ({'all on' if pref == '0x0' else 'all off'})")
        ax.grid(alpha=0.3)
        ax.set_ylim(0, 1)
        ax.legend(fontsize=8)
    axes[0].set_ylabel("cleansing rate (fraction of sets fully evicted)")
    fig.suptitle("Cleansing rate vs NoC, bidirectional sweep")
    fig.tight_layout()
    out = FIGDIR / "bidir_cleansing_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def plot_bidir_cleansing_percluster():
    """Per-cluster cleansing rate at NoC=64: bidir vs baseline."""
    r, pref = PERCLUSTER_BIDIR
    m = tree_metrics(BIDIR_TREE.format(r=r, p=pref), 64)
    if m is None:
        return
    label = f"bidirR{r}_{pref}"
    base = tree_metrics(P3A1["0xf"], 64)
    fig, ax = plt.subplots(figsize=(14, 5))
    series = {label: m["per_cluster_cleansing_min"]}
    colors = {label: plt.get_cmap("viridis")(0.75)}
    if base is not None:
        series["baseline p3a1_0xf"] = base["per_cluster_cleansing_min"]
        colors["baseline p3a1_0xf"] = plt.get_cmap("viridis")(0.1)
    vals = _value_blocks(ax, series, colors, 64)
    ax.set_xlim(0, 64)
    margin = 0.08 * max(vals.max() - vals.min(), 1e-6)
    ax.set_ylim(max(0.0, vals.min() - margin), min(1.0, vals.max() + margin))
    ax.set_xlabel("cluster index at NoC=64 (= line offset within page)")
    ax.set_ylabel("cleansing rate (fraction of sets fully evicted)")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="lower right", fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title(f"Per-cluster cleansing rate at NoC=64: {label} vs baseline")
    fig.tight_layout()
    out = FIGDIR / "bidir_cleansing_percluster_noc64.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 7. Cleansing rate on the real Mastik eviction sets (native / native_shuffled)
# ---------------------------------------------------------------------------
def report_native_cleansing():
    """Cleansing on guaranteed-membership e-sets: the ceiling the lazy map is chasing.

    native          = real Mastik e-sets, contiguous sweep order
    native_shuffled = the SAME e-sets, line-shuffled sweep order (prefetch-defeating)
    Any shortfall here cannot be a membership problem -- every set has its full ASSOC lines.
    """
    print("=" * 100)
    print("7. CLEANSING RATE -- real Mastik e-sets (native = contiguous, native_shuffled = scattered)")
    print("   Membership is guaranteed (ASSOC lines/set), so this is the reachable ceiling.")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        rec = {"noc": noc}
        for tag, trees in [("cont", NATIVE_CONT), ("shuf", NATIVE_SHUF)]:
            for pref in ["0x0", "0x2", "0xf"]:
                m = tree_metrics(trees[pref], noc)
                rec[f"{tag}_{pref}"] = fmt(m["cleansing_min"]) if m else "--"
        rec["lazy_p3a1_0xf"] = fmt((tree_metrics(P3A1["0xf"], noc) or {}).get("cleansing_min"))
        rows.append(rec)
    print("--- cleansing_min ---")
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    print("--- coverage_min vs cleansing_min, real e-sets at pref0xf ---")
    crows = []
    for noc in NOC_ALL:
        for label, tree in [("native contiguous", NATIVE_CONT["0xf"]),
                            ("native shuffled", NATIVE_SHUF["0xf"]),
                            ("lazy p3a1", P3A1["0xf"])]:
            m = tree_metrics(tree, noc)
            if m is None:
                continue
            crows.append({"noc": noc, "victim": label,
                          "coverage_min": round(m["coverage_min"], 4),
                          "cleansing_min": round(m["cleansing_min"], 4),
                          "gap": round(m["coverage_min"] - m["cleansing_min"], 4)})
    print(pd.DataFrame(crows).to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    cmap = plt.get_cmap("viridis")
    prefs = ["0x0", "0x2", "0xf"]
    for ax, (tag, trees, title) in zip(axes, [
            ("cont", NATIVE_CONT, "native (contiguous sweep)"),
            ("shuf", NATIVE_SHUF, "native_shuffled (scattered sweep)")]):
        for i, pref in enumerate(prefs):
            xs, ys = [], []
            for noc in NOC_ALL:
                m = tree_metrics(trees[pref], noc)
                if m is not None:
                    xs.append(noc)
                    ys.append(m["cleansing_min"])
            if xs:
                ax.plot(xs, ys, marker="o", color=cmap(i / (len(prefs) - 1)), label=f"pref{pref}")
        lx, ly = [], []
        for noc in NOC_ALL:
            b = tree_metrics(P3A1["0xf"], noc)
            if b is not None:
                lx.append(noc)
                ly.append(b["cleansing_min"])
        if lx:
            ax.plot(lx, ly, marker="s", color="crimson", linestyle="--",
                    label="lazy p3a1_0xf (statistical)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.grid(alpha=0.3)
        ax.set_ylim(0, 1)
        ax.legend(fontsize=8)
    axes[0].set_ylabel("cleansing rate (fraction of sets fully evicted)")
    fig.suptitle("Cleansing rate vs NoC, real Mastik e-sets (guaranteed membership) vs lazy map")
    fig.tight_layout()
    out = FIGDIR / "native_cleansing_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def plot_native_cleansing_percluster():
    """Per-cluster cleansing at NoC=64: real e-sets vs lazy map, all at pref0xf.

    Decisive for the period-4 pattern: real e-sets have guaranteed membership, so if the
    pattern is ABSENT here it is a lazy-map filling artifact; if PRESENT it is addressing/hardware.
    """
    fig, ax = plt.subplots(figsize=(14, 5))
    cmap = plt.get_cmap("viridis")
    spec = [("native contiguous 0xf", NATIVE_CONT["0xf"], cmap(0.15)),
            ("native shuffled 0xf", NATIVE_SHUF["0xf"], cmap(0.55)),
            ("lazy p3a1 0xf", P3A1["0xf"], cmap(0.9))]
    series, colors = {}, {}
    for label, tree, color in spec:
        m = tree_metrics(tree, 64)
        if m is None:
            continue
        series[label] = m["per_cluster_cleansing_min"]
        colors[label] = color
    if not series:
        return
    vals = _value_blocks(ax, series, colors, 64)
    ax.set_xlim(0, 64)
    margin = 0.08 * max(vals.max() - vals.min(), 1e-6)
    ax.set_ylim(max(0.0, vals.min() - margin), min(1.0, vals.max() + margin))
    ax.set_xlabel("cluster index at NoC=64 (= line offset within page)")
    ax.set_ylabel("cleansing rate (fraction of sets fully evicted)")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="center left", bbox_to_anchor=(1.005, 0.5), fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title("Per-cluster cleansing rate at NoC=64: real e-sets vs lazy map (all pref0xf)")
    fig.tight_layout()
    out = FIGDIR / "native_cleansing_percluster_noc64.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 8. Spillover rate (spatial spillover), before and after baseline subtraction
# ---------------------------------------------------------------------------
SPILL_VICTIMS = [
    ("lazy p3a1 0xf", lambda n: P3A1["0xf"]),
    ("lazy p1a1 0x0", lambda n: P1A1["0x0"]),
    ("lazy bidirR1 0xf", lambda n: BIDIR_TREE.format(r=1, p="0xf")),
    ("lazy bidirR4 0xf", lambda n: BIDIR_TREE.format(r=4, p="0xf")),
    ("lazy bidirR8 0xf", lambda n: BIDIR_TREE.format(r=8, p="0xf")),
]

# Real e-sets across the full prefetcher ladder -- how much of the specificity advantage is the
# guaranteed membership itself, and how much is prefetcher state?
SPILL_NATIVE = [
    (f"native {tag} {pref}", trees[pref])
    for tag, trees in [("contiguous", NATIVE_CONT), ("shuffled", NATIVE_SHUF)]
    for pref in ["0x0", "0x2", "0xf"]
]


def report_spillover():
    """Spillover = fraction of NON-target lines evicted. 0 good, 1 bad. Lower-is-better, so the
    worst case over samples is MAX. Degenerate alone (do nothing -> 0), so coverage is shown
    alongside: spillover is the false-positive rate to coverage's true-positive rate.
    """
    print("=" * 100)
    print("8. SPILLOVER RATE -- spillover into clusters the victim did NOT target (0 good, 1 bad)")
    print("   = mean off-diagonal / ASSOC. 'raw' includes the idle noise floor; 'sub' is")
    print("   baseline-subtracted (victim-caused only). Shown WITH coverage: spillover alone is")
    print("   degenerate (a victim that does nothing spills 0).")
    print("=" * 100)

    def spill_rows(victims):
        rows = []
        for noc in NOC_ALL:
            for label, tree in victims:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                rows.append({
                    "noc": noc, "victim": label,
                    "spill_raw": round(m["spillover_raw_mean"], 4),
                    "spill_raw_max": round(m["spillover_raw_max"], 4),
                    "spill_sub": round(m["spillover_sub_mean"], 4),
                    "spill_sub_max": round(m["spillover_sub_max"], 4),
                    "noise_share": round(m["spillover_raw_mean"] - m["spillover_sub_mean"], 4),
                    "coverage_min": round(m["coverage_min"], 4),
                })
        return pd.DataFrame(rows)

    lazy = [(lbl, treef(0)) for lbl, treef in SPILL_VICTIMS]
    print("--- lazy map victims ---")
    print(spill_rows(lazy).to_string(index=False))
    print()
    print("--- real Mastik e-sets, full prefetcher ladder (0x0 all on .. 0xf all off) ---")
    print(spill_rows(SPILL_NATIVE).to_string(index=False))
    print()

    # trend: spillover vs NoC, raw and baseline-subtracted. Log y -- values span ~50x, so a
    # linear axis flattens every real-e-set series onto zero.
    all_series = lazy + SPILL_NATIVE
    fig, axes = plt.subplots(1, 2, figsize=(15, 5.5), sharey=True)
    cmap = plt.get_cmap("viridis")
    for ax, key, title in [(axes[0], "spillover_raw_mean", "raw (includes idle noise floor)"),
                           (axes[1], "spillover_sub_mean", "baseline-subtracted (victim-caused)")]:
        for i, (label, tree) in enumerate(all_series):
            xs, ys = [], []
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is not None and m[key] > 0:
                    xs.append(noc)
                    ys.append(m[key])
            if xs:
                style = "--" if label.startswith("native") else "-"
                ax.plot(xs, ys, marker="o", markersize=4, linestyle=style,
                        color=cmap(i / max(len(all_series) - 1, 1)), label=label)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.grid(alpha=0.3, which="both")
    axes[0].set_ylabel("spillover rate, log scale (lower = more specific)")
    axes[0].legend(fontsize=6.5, ncol=2)
    fig.suptitle("Spillover rate vs NoC (solid = lazy map, dashed = real Mastik e-sets)")
    fig.tight_layout()
    out = FIGDIR / "spillover_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")

    # coverage vs spillover: the operating-point view (want high coverage, low spillover)
    fig, ax = plt.subplots(figsize=(9, 6.5))
    markers = ["o", "s", "^", "v", "D", "P", "X", "*", "<", ">", "h"]
    for i, (label, tree) in enumerate(all_series):
        xs, ys = [], []
        for noc in NOC_ALL:
            m = tree_metrics(tree, noc)
            if m is not None and m["spillover_sub_mean"] > 0:
                xs.append(m["spillover_sub_mean"])
                ys.append(m["coverage_min"])
        if xs:
            ax.scatter(xs, ys, s=60, marker=markers[i % len(markers)],
                       color=cmap(i / max(len(all_series) - 1, 1)), label=label)
    ax.set_xscale("log")
    ax.set_xlabel("spillover rate, log scale (baseline-subtracted) -- lower is better")
    ax.set_ylabel("coverage (worst-case) -- higher is better")
    ax.set_title("Operating point: coverage vs spillover\n(top-left is the goal; each point is one NoC)")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=7, loc="lower right")
    fig.tight_layout()
    out = FIGDIR / "spillover_vs_coverage.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def plot_spillover_percluster():
    """Per-cluster spillover at NoC=64, baseline-subtracted."""
    fig, ax = plt.subplots(figsize=(14, 5))
    cmap = plt.get_cmap("viridis")
    spec = [("native shuffled 0xf", NATIVE_SHUF["0xf"], cmap(0.15)),
            ("lazy p3a1 0xf", P3A1["0xf"], cmap(0.55)),
            ("lazy bidirR4 0xf", BIDIR_TREE.format(r=4, p="0xf"), cmap(0.9))]
    series, colors = {}, {}
    for label, tree, color in spec:
        m = tree_metrics(tree, 64)
        if m is None:
            continue
        series[label] = m["per_cluster_spillover_sub"]
        colors[label] = color
    if not series:
        return
    vals = _value_blocks(ax, series, colors, 64)
    ax.set_xlim(0, 64)
    margin = 0.08 * max(vals.max() - vals.min(), 1e-6)
    ax.set_ylim(max(0.0, vals.min() - margin), vals.max() + margin)
    ax.set_xlabel("cluster index at NoC=64 (= line offset within page)")
    ax.set_ylabel("spillover rate (baseline-subtracted)")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="center left", bbox_to_anchor=(1.005, 0.5), fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title("Per-cluster spillover rate at NoC=64 (baseline-subtracted, all pref0xf)")
    fig.tight_layout()
    out = FIGDIR / "spillover_percluster_noc64.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def sanity_checks(baseline_df):
    print("=" * 100)
    print("0. SANITY")
    print("=" * 100)
    bad = baseline_df[baseline_df["cov_min"] > baseline_df["cov_mean"] + 1e-9]
    print(f"cov_min > cov_mean violations (must be 0): {len(bad)}")
    dm = baseline_df["diag_mass"]
    print(f"diag_mass within [0,1]: {bool(((dm >= 0) & (dm <= 1)).all())}")
    cl = baseline_df["cleanse_min"]
    bad_cl = baseline_df[baseline_df["cleanse_min"] > baseline_df["cleanse_mean"] + 1e-9]
    print(f"cleansing within [0,1]: {bool(((cl >= 0) & (cl <= 1)).all())}")
    print(f"cleanse_min > cleanse_mean violations (must be 0): {len(bad_cl)}")
    # cleansing is strictly stricter than coverage: a fully-evicted set scores 1 in both, a
    # partial set scores >0 in coverage but 0 in cleansing => cleansing <= coverage always.
    viol = baseline_df[baseline_df["cleanse_min"] > baseline_df["cov_min"] + 1e-9]
    print(f"cleanse_min > cov_min violations (must be 0 by construction): {len(viol)}")
    print(f"sample counts seen: {sorted(baseline_df['n'].unique())}")
    print()


def main():
    baseline_df = report_baseline()
    sanity_checks(baseline_df)
    report_passes_effect()
    plot_baseline_trend(baseline_df)
    plot_baseline_percluster()
    report_adc_grid()
    report_buffer()
    report_big_window()
    report_decoy()
    report_bidir()
    plot_bidir_percluster()
    report_bidir_cleansing()
    plot_bidir_cleansing_percluster()
    report_native_cleansing()
    plot_native_cleansing_percluster()
    report_spillover()
    plot_spillover_percluster()


if __name__ == "__main__":
    main()
