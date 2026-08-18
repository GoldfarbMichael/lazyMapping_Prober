"""Spillover-rate analysis.

Spillover rate = fraction of the NON-target lines that the victim evicted. 0 = perfectly
specific, 1 = evicted everything everywhere. Formal definition: docs/journal/Metrics_Definitions.md.

Raw data (per sample CSV, data/coverage/<tree>/NoC<NN>/<iii>.csv):
  header S0..S16383 (one column per LLC set), then NoC cluster-sweep rows, then 15 idle rows.
  cell[c][i] = ways evicted (0..ASSOC) in LLC set i while the victim swept lazy cluster c.

Reduces to mean(off-diagonal of the cluster-aggregated matrix)/ASSOC when clusters are equal-sized
(asserted below). Computed BOTH raw (includes the idle noise floor) and baseline-subtracted
(victim-caused only).

Two conventions that differ from coverage/cleansing:
  * lower is better, so the worst case over samples is MAX, not min;
  * degenerate alone -- a victim that does nothing scores a perfect 0. Coverage is therefore
    reported alongside everywhere: spillover is the false-positive rate to coverage's true-positive.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Rectangle
import numpy as np
import pandas as pd

STABLE = Path(__file__).resolve().parent.parent
DATA = STABLE / "data" / "coverage"
FIGDIR = Path(__file__).resolve().parent / "spillover_figures"
FIGDIR.mkdir(exist_ok=True)

ASSOC = 12
PAGE_OFFSET_BITS = 12
BASELINE_ROWS = 15
NOC_ALL = [2, 4, 8, 16, 32, 64]
NOC_EV = [8, 16, 32, 64]
PREFS = ["0x0", "0x2", "0xf"]

P3A1 = {"0x0": "native_jsmap_shuffled_p3a1_same",
        "0x2": "native_jsmap_shuffled_p3a1_pref0x2",
        "0xf": "native_jsmap_shuffled_p3a1_pref0xf"}
P1A1 = {"0x0": "native_jsmap_shuffled_p1a1_same",
        "0x2": "native_jsmap_shuffled_p1a1_same_pref0x2"}
NATIVE_CONT = {"0x0": "native", "0x2": "native_pref0x2", "0xf": "native_pref0xf"}
NATIVE_SHUF = {"0x0": "native_shuffled", "0x2": "native_shuffled_pref0x2",
               "0xf": "native_shuffled_pref0xf"}
BIDIR_R = [1, 2, 4, 8]
BIDIR_TREE = "native_jsmap_shuffled_p1a1_bidirR{r}_pref{p}"

_phys_cache, _metrics_cache = {}, {}


def phys_clusters(tree, noc):
    """Physical cluster per LLC set: top log2(noc) bits of the PA page offset (lazy_map.c:51,71)."""
    key = (tree, noc)
    if key not in _phys_cache:
        labels = DATA / tree / "set_labels.csv"
        if not labels.is_file():
            _phys_cache[key] = None
        else:
            df = pd.read_csv(labels).sort_values("set_idx")
            pa = df["pa"].apply(lambda s: int(str(s), 16)).to_numpy(dtype=np.int64)
            shift = PAGE_OFFSET_BITS - int(round(np.log2(noc)))
            _phys_cache[key] = (pa >> shift) & (noc - 1)
    return _phys_cache[key]


def offdiag_row_mean(M):
    """Per-row mean of the off-diagonal entries (NoC >= 2)."""
    noc = M.shape[0]
    if noc < 2:
        return np.zeros(noc)
    return (M.sum(axis=1) - np.diag(M)) / (noc - 1)


def load_sample(path, noc, pc):
    """One CSV -> (raw NoC x NoC, base NoC). Columns aggregated to physical clusters by mean."""
    data = pd.read_csv(path).to_numpy(dtype=float)
    if data.shape[0] != noc + BASELINE_ROWS:
        raise ValueError(f"{path}: {data.shape[0]} data rows, expected {noc + BASELINE_ROWS}")
    rows = data[:noc]
    idle = data[noc:].mean(axis=0)
    raw = np.zeros((noc, noc))
    base = np.zeros(noc)
    for g in range(noc):
        mask = pc == g
        if mask.any():
            raw[:, g] = rows[:, mask].mean(axis=1)
            base[g] = idle[mask].mean()
    return raw, base


def tree_metrics(tree, noc):
    """Spillover (raw + subtracted) and coverage for one (tree, noc). Memoized."""
    key = (tree, noc)
    if key in _metrics_cache:
        return _metrics_cache[key]
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    paths = sorted(noc_dir.glob("*.csv")) if noc_dir.is_dir() else []
    pc = phys_clusters(tree, noc)
    if not paths or pc is None:
        _metrics_cache[key] = None
        return None
    # equal cluster sizes are what make the off-diagonal reduction valid
    counts = np.bincount(pc, minlength=noc)
    if counts.min() != counts.max():
        raise ValueError(f"{tree} NoC{noc}: unequal cluster sizes {counts.min()}..{counts.max()}")

    s_raw, s_sub, raws = [], [], []
    for p in paths:
        raw, base = load_sample(p, noc, pc)
        sub = np.clip(raw - base[None, :], 0.0, None)
        raws.append(raw)
        s_raw.append(offdiag_row_mean(raw) / ASSOC)
        s_sub.append(offdiag_row_mean(sub) / ASSOC)
    s_raw, s_sub, raws = np.array(s_raw), np.array(s_sub), np.array(raws)
    out = {
        "n": len(paths),
        # lower is better -> "worst" is MAX over samples
        "spill_raw": float(s_raw.mean(axis=0).mean()),
        "spill_raw_max": float(s_raw.max(axis=0).mean()),
        "spill_sub": float(s_sub.mean(axis=0).mean()),
        "spill_sub_max": float(s_sub.max(axis=0).mean()),
        "per_cluster_sub": s_sub.mean(axis=0),
        "per_cluster_raw": s_raw.mean(axis=0),
        "coverage_min": float(np.diag(raws.min(axis=0)).mean() / ASSOC),
    }
    _metrics_cache[key] = out
    return out


def fmt(x, nd=4):
    return "--" if x is None else f"{x:.{nd}f}"


def _get(tree, noc, key):
    m = tree_metrics(tree, noc)
    return None if m is None else m[key]


# ---------------------------------------------------------------------------
def sanity_checks():
    print("=" * 100)
    print("0. SANITY")
    print("=" * 100)
    checked = bad_range = bad_order = bad_sub = 0
    for trees in (P3A1, P1A1, NATIVE_CONT, NATIVE_SHUF):
        for tree in trees.values():
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                checked += 1
                if not (0.0 <= m["spill_raw"] <= 1.0 and 0.0 <= m["spill_sub"] <= 1.0):
                    bad_range += 1
                if m["spill_raw_max"] < m["spill_raw"] - 1e-12:
                    bad_order += 1
                # subtracting a non-negative baseline and clipping can only lower the value
                if m["spill_sub"] > m["spill_raw"] + 1e-12:
                    bad_sub += 1
    print(f"cells checked                        : {checked}")
    print(f"spillover outside [0,1]              : {bad_range}  (must be 0)")
    print(f"spill_raw_max < spill_raw            : {bad_order}  (must be 0)")
    print(f"spill_sub > spill_raw                : {bad_sub}  (must be 0 by construction)")
    print()


# ---------------------------------------------------------------------------
def report_baseline():
    print("=" * 100)
    print("1. LAZY MAP BASELINE -- spillover across the prefetcher ladder")
    print("   noise_share = spill_raw - spill_sub (how much apparent spillover is the idle floor)")
    print("=" * 100)
    rows = []
    for label, trees in [("p3a1", P3A1), ("p1a1", P1A1)]:
        for pref, tree in trees.items():
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                rows.append({"sweep": label, "pref": pref, "noc": noc, "n": m["n"],
                             "spill_raw": round(m["spill_raw"], 4),
                             "spill_sub": round(m["spill_sub"], 4),
                             "spill_sub_max": round(m["spill_sub_max"], 4),
                             "noise_share": round(m["spill_raw"] - m["spill_sub"], 4),
                             "coverage_min": round(m["coverage_min"], 4)})
    print(pd.DataFrame(rows).to_string(index=False))
    print()
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
def report_native():
    print("=" * 100)
    print("2. REAL MASTIK E-SETS -- spillover across the full prefetcher ladder")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        for tag, trees in [("contiguous", NATIVE_CONT), ("shuffled", NATIVE_SHUF)]:
            for pref in PREFS:
                m = tree_metrics(trees[pref], noc)
                if m is None:
                    continue
                rows.append({"noc": noc, "order": tag, "pref": pref,
                             "spill_raw": round(m["spill_raw"], 4),
                             "spill_sub": round(m["spill_sub"], 4),
                             "noise_share": round(m["spill_raw"] - m["spill_sub"], 4),
                             "coverage_min": round(m["coverage_min"], 4)})
    print(pd.DataFrame(rows).to_string(index=False))
    print()


# ---------------------------------------------------------------------------
def report_bidir():
    print("=" * 100)
    print("3. BIDIRECTIONAL SWEEP -- spillover (baseline-subtracted), vs coverage")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                rec[f"R{r}_{pref}"] = fmt(_get(BIDIR_TREE.format(r=r, p=pref), noc, "spill_sub"))
        rec["base_p3a1_0xf"] = fmt(_get(P3A1["0xf"], noc, "spill_sub"))
        rec["native_shuf_0xf"] = fmt(_get(NATIVE_SHUF["0xf"], noc, "spill_sub"))
        rows.append(rec)
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    print("--- R-dependence at pref0xf: spillover grows, coverage does not ---")
    rows = []
    for noc in NOC_EV:
        for r in BIDIR_R:
            m = tree_metrics(BIDIR_TREE.format(r=r, p="0xf"), noc)
            if m is None:
                continue
            rows.append({"noc": noc, "R": r,
                         "spill_sub": round(m["spill_sub"], 4),
                         "coverage_min": round(m["coverage_min"], 4)})
    print(pd.DataFrame(rows).to_string(index=False))
    print()


# ---------------------------------------------------------------------------
def _series():
    """(label, tree) pairs used by the trend and operating-point figures."""
    lazy = [("lazy p3a1 0xf", P3A1["0xf"]), ("lazy p1a1 0x0", P1A1["0x0"])]
    lazy += [(f"lazy bidirR{r} 0xf", BIDIR_TREE.format(r=r, p="0xf")) for r in (1, 4, 8)]
    native = [(f"native {tag} {pref}", trees[pref])
              for tag, trees in [("cont", NATIVE_CONT), ("shuf", NATIVE_SHUF)]
              for pref in PREFS]
    return lazy, native


def plot_trend():
    """Spillover vs NoC, raw and subtracted. Log y -- values span ~1000x."""
    lazy, native = _series()
    allser = lazy + native
    cmap = plt.get_cmap("viridis")
    fig, axes = plt.subplots(1, 2, figsize=(15, 5.5), sharey=True)
    for ax, key, title in [(axes[0], "spill_raw", "raw (includes idle noise floor)"),
                           (axes[1], "spill_sub", "baseline-subtracted (victim-caused)")]:
        for i, (label, tree) in enumerate(allser):
            xs = [n for n in NOC_ALL if (_get(tree, n, key) or 0) > 0]
            ys = [_get(tree, n, key) for n in xs]
            if xs:
                ax.plot(xs, ys, marker="o", markersize=4,
                        linestyle="--" if label.startswith("native") else "-",
                        color=cmap(i / max(len(allser) - 1, 1)), label=label)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.grid(alpha=0.3, which="both")
    axes[0].set_ylabel("spillover rate, log scale (lower = more specific)")
    axes[0].legend(fontsize=6.5, ncol=2)
    fig.suptitle("Spillover vs NoC (solid = lazy map, dashed = real Mastik e-sets)")
    fig.tight_layout()
    out = FIGDIR / "spillover_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def plot_operating_point():
    """Coverage vs spillover -- the pair must be read together; top-left is the goal."""
    lazy, native = _series()
    allser = lazy + native
    cmap = plt.get_cmap("viridis")
    markers = ["o", "s", "^", "v", "D", "P", "X", "*", "<", ">", "h"]
    fig, ax = plt.subplots(figsize=(9, 6.5))
    for i, (label, tree) in enumerate(allser):
        xs, ys = [], []
        for noc in NOC_ALL:
            m = tree_metrics(tree, noc)
            if m is not None and m["spill_sub"] > 0:
                xs.append(m["spill_sub"])
                ys.append(m["coverage_min"])
        if xs:
            ax.scatter(xs, ys, s=60, marker=markers[i % len(markers)],
                       color=cmap(i / max(len(allser) - 1, 1)), label=label)
    ax.set_xscale("log")
    ax.set_xlabel("spillover rate, log scale (baseline-subtracted) -- lower is better")
    ax.set_ylabel("coverage (worst-case) -- higher is better")
    ax.set_title("Operating point: coverage vs spillover\n(top-left is the goal; each point is one NoC)")
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=6.5, loc="lower left")
    fig.tight_layout()
    out = FIGDIR / "operating_point.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def plot_percluster(noc=64):
    cmap = plt.get_cmap("viridis")
    spec = [("native shuffled 0xf", NATIVE_SHUF["0xf"], cmap(0.1)),
            ("lazy bidirR4 0xf", BIDIR_TREE.format(r=4, p="0xf"), cmap(0.55)),
            ("lazy p3a1 0xf", P3A1["0xf"], cmap(0.9))]
    series, colors = {}, {}
    for label, tree, color in spec:
        m = tree_metrics(tree, noc)
        if m is not None:
            series[label] = m["per_cluster_sub"]
            colors[label] = color
    if not series:
        return
    fig, ax = plt.subplots(figsize=(14, 5))
    for i in range(noc):
        if i % 2 == 1:
            ax.axvspan(i, i + 1, color="0.90", zorder=0)
    allv = np.concatenate(list(series.values()))
    h = 0.012 * max(allv.max() - allv.min(), 1e-9)
    for z, (label, vals) in enumerate(series.items()):
        for c, v in enumerate(vals):
            ax.add_patch(Rectangle((c, v - h / 2), 1, h, facecolor=colors[label],
                                   edgecolor="none", zorder=10 + z))
    ax.set_xlim(0, noc)
    m_ = 0.08 * max(allv.max() - allv.min(), 1e-9)
    ax.set_ylim(max(0.0, allv.min() - m_), allv.max() + m_)
    ax.set_xlabel(f"cluster index at NoC={noc} (= line offset within page)")
    ax.set_ylabel("spillover rate (baseline-subtracted)")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="center left", bbox_to_anchor=(1.005, 0.5), fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title(f"Per-cluster spillover at NoC={noc} (baseline-subtracted, all pref0xf)")
    fig.tight_layout()
    out = FIGDIR / f"percluster_noc{noc}.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def main():
    sanity_checks()
    report_baseline()
    report_native()
    report_bidir()
    plot_trend()
    plot_operating_point()
    plot_percluster(64)


if __name__ == "__main__":
    main()
