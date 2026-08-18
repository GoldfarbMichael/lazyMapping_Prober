"""Cleansing-rate analysis.

Cleansing rate = fraction of a cluster's own sets that were evicted IN FULL (all ASSOC ways).
Strict, all-or-nothing companion to coverage: coverage scores an 11/12-way set 0.92, cleansing
scores it 0. Formal definition: docs/journal/Metrics_Definitions.md.

Raw data (per sample CSV, data/coverage/<tree>/NoC<NN>/<iii>.csv):
  header S0..S16383 (one column per LLC set), then NoC cluster-sweep rows, then 15 idle rows.
  cell[c][i] = ways evicted (0..ASSOC) in LLC set i while the victim swept lazy cluster c.

The threshold is applied per SET and per SAMPLE, BEFORE any cluster aggregation -- it cannot be
recovered from a set-averaged matrix. min/mean refer to the worst-case vs mean-case sample
(cleansing is higher-is-better, so its worst case is min, matching the coverage convention).
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
FIGDIR = Path(__file__).resolve().parent / "cleansing_figures"
FIGDIR.mkdir(exist_ok=True)

ASSOC = 12
PAGE_OFFSET_BITS = 12
BASELINE_ROWS = 15
NOC_ALL = [2, 4, 8, 16, 32, 64]
NOC_EV = [8, 16, 32, 64]        # ev*/bidir* trees exist only at these
PREFS = ["0x0", "0x2", "0xf"]   # MSR 0x1a4: 0x0 all on, 0x2 adj-line off, 0xf all off

# Untagged tree name => pref0x0 (the _pref tag was added later).
P3A1 = {"0x0": "native_jsmap_shuffled_p3a1_same",
        "0x2": "native_jsmap_shuffled_p3a1_pref0x2",
        "0xf": "native_jsmap_shuffled_p3a1_pref0xf"}
P1A1 = {"0x0": "native_jsmap_shuffled_p1a1_same",
        "0x2": "native_jsmap_shuffled_p1a1_same_pref0x2"}   # no 0xf tree exists
# Real Mastik e-sets: membership is GUARANTEED ASSOC lines/set (vs the lazy map's statistical
# filling), so these are the reachable ceiling. Contiguous vs line-shuffled sweep order.
NATIVE_CONT = {"0x0": "native", "0x2": "native_pref0x2", "0xf": "native_pref0xf"}
NATIVE_SHUF = {"0x0": "native_shuffled", "0x2": "native_shuffled_pref0x2",
               "0xf": "native_shuffled_pref0xf"}
BIDIR_R = [1, 2, 4, 8]
BIDIR_TREE = "native_jsmap_shuffled_p1a1_bidirR{r}_pref{p}"
# Same victim as bidirR4_pref0x0 but with a 24MB buffer (mean 24 lines/set instead of 12).
# Both have n=10, so this is a clean buffer-size A/B: only JSMAP_BUF_MB differs.
BIDIR24 = "native_jsmap_shuffled_p1a1_bidirR4_pref0x0_24MB"
BIDIR12 = BIDIR_TREE.format(r=4, p="0x0")

# ExtraTrees accuracy_mean, 2TST/<config>/<NoC>C_2TST_90K_2288cycles, factor 1x1. Reported
# externally by the classifier, NOT recomputed here -- pasted verbatim so the coverage figures
# can be read against downstream accuracy. NoC=1 exists upstream but has no coverage tree.
ACC_MEAN = {
    "JS map Bidir sweep 24MB": {1: 0.781578947368421, 2: 0.801578947368421,
                                4: 0.8278947368421052, 8: 0.8289473684210528,
                                16: 0.8326315789473684, 32: 0.8389473684210527,
                                64: 0.881578947368421},
    "JS map Bidir sweep 12MB": {1: 0.8005263157894736, 2: 0.8563157894736844,
                                4: 0.8721052631578947, 8: 0.8994736842105263,
                                16: 0.8952631578947369, 32: 0.9089473684210526,
                                64: 0.9431578947368422},
    "JS map Regular sweep 12MB": {1: 0.7736842105263159, 2: 0.8142105263157895,
                                  4: 0.8921052631578947, 8: 0.9099999999999999,
                                  16: 0.9221052631578948, 32: 0.9510526315789474,
                                  64: 0.9578947368421054},
}
ACC_COLOR = {"JS map Bidir sweep 24MB": "crimson",
             "JS map Bidir sweep 12MB": "darkorange",
             "JS map Regular sweep 12MB": "black"}

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


def load_sample(path, noc, pc):
    """One CSV -> (clean NoC x NoC, raw NoC x NoC).

    clean[c][g] = fraction of g's sets fully evicted (>= ASSOC ways) while sweeping c.
    raw[c][g]   = mean ways evicted per set in g while sweeping c (kept for the gap section).
    """
    data = pd.read_csv(path).to_numpy(dtype=float)
    if data.shape[0] != noc + BASELINE_ROWS:
        raise ValueError(f"{path}: {data.shape[0]} data rows, expected {noc + BASELINE_ROWS}")
    rows = data[:noc]
    full = rows >= ASSOC                      # per-set binary, BEFORE any averaging
    clean = np.zeros((noc, noc))
    raw = np.zeros((noc, noc))
    for g in range(noc):
        mask = pc == g
        if mask.any():
            clean[:, g] = full[:, mask].mean(axis=1)
            raw[:, g] = rows[:, mask].mean(axis=1)
    return clean, raw


def tree_metrics(tree, noc):
    """Cleansing (and coverage, for the gap) for one (tree, noc). Memoized; CSVs are 16384-col."""
    key = (tree, noc)
    if key in _metrics_cache:
        return _metrics_cache[key]
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    paths = sorted(noc_dir.glob("*.csv")) if noc_dir.is_dir() else []
    pc = phys_clusters(tree, noc)
    if not paths or pc is None:
        _metrics_cache[key] = None
        return None
    cleans, raws = [], []
    for p in paths:
        c, r = load_sample(p, noc, pc)
        cleans.append(c)
        raws.append(r)
    cleans, raws = np.array(cleans), np.array(raws)
    min_c, mean_c = cleans.min(axis=0), cleans.mean(axis=0)
    out = {
        "n": len(paths),
        "cleansing_min": float(np.diag(min_c).mean()),
        "cleansing_mean": float(np.diag(mean_c).mean()),
        "per_cluster_min": np.diag(min_c),
        "per_cluster_mean": np.diag(mean_c),
        "coverage_min": float(np.diag(raws.min(axis=0)).mean() / ASSOC),
    }
    _metrics_cache[key] = out
    return out


def fmt(x, nd=3):
    return "--" if x is None else f"{x:.{nd}f}"


def _get(tree, noc, key):
    m = tree_metrics(tree, noc)
    return None if m is None else m[key]


# ---------------------------------------------------------------------------
def sanity_checks():
    print("=" * 100)
    print("0. SANITY")
    print("=" * 100)
    bad_range = bad_order = bad_vs_cov = checked = 0
    bidir_all = {f"R{r}_{p}": BIDIR_TREE.format(r=r, p=p) for r in BIDIR_R for p in ("0x0", "0xf")}
    bidir_all["R4_0x0_24MB"] = BIDIR24
    for trees in (P3A1, P1A1, NATIVE_CONT, NATIVE_SHUF, bidir_all):
        for tree in trees.values():
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                checked += 1
                if not (0.0 <= m["cleansing_min"] <= 1.0):
                    bad_range += 1
                if m["cleansing_min"] > m["cleansing_mean"] + 1e-9:
                    bad_order += 1
                # cleansing <= coverage by construction (1[x>=W] <= x/W); see Metrics_Definitions
                if m["cleansing_min"] > m["coverage_min"] + 1e-9:
                    bad_vs_cov += 1
    print(f"cells checked                              : {checked}")
    print(f"cleansing outside [0,1]                    : {bad_range}  (must be 0)")
    print(f"cleansing_min > cleansing_mean             : {bad_order}  (must be 0)")
    print(f"cleansing_min > coverage_min               : {bad_vs_cov}  (must be 0 by construction)")
    print()


# ---------------------------------------------------------------------------
def report_baseline():
    """Lazy-map victim across the prefetcher ladder."""
    print("=" * 100)
    print("1. LAZY MAP BASELINE -- cleansing rate across the prefetcher ladder")
    print("=" * 100)
    rows = []
    for label, trees in [("p3a1", P3A1), ("p1a1", P1A1)]:
        for pref, tree in trees.items():
            for noc in NOC_ALL:
                m = tree_metrics(tree, noc)
                if m is None:
                    continue
                rows.append({"sweep": label, "pref": pref, "noc": noc, "n": m["n"],
                             "cleanse_min": round(m["cleansing_min"], 4),
                             "cleanse_mean": round(m["cleansing_mean"], 4),
                             "coverage_min": round(m["coverage_min"], 4),
                             "gap": round(m["coverage_min"] - m["cleansing_min"], 4)})
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    cmap = plt.get_cmap("viridis")
    for ax, (label, trees) in zip(axes, [("p3a1", P3A1), ("p1a1", P1A1)]):
        for i, pref in enumerate(PREFS):
            if pref not in trees:
                continue
            xs = [n for n in NOC_ALL if _get(trees[pref], n, "cleansing_min") is not None]
            ys = [_get(trees[pref], n, "cleansing_min") for n in xs]
            if xs:
                ax.plot(xs, ys, marker="o", color=cmap(i / (len(PREFS) - 1)), label=f"pref{pref}")
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(f"{label} victim")
        ax.set_ylim(0, 1)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    axes[0].set_ylabel("cleansing rate (fraction of sets fully evicted)")
    fig.suptitle("Cleansing rate vs NoC, lazy-map victim")
    fig.tight_layout()
    out = FIGDIR / "baseline_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")
    return df


# ---------------------------------------------------------------------------
def report_native():
    """Real Mastik e-sets: guaranteed membership, so this is the reachable ceiling."""
    print("=" * 100)
    print("2. REAL MASTIK E-SETS -- the ceiling (membership guaranteed: ASSOC lines/set)")
    print("=" * 100)
    cmap = plt.get_cmap("viridis")
    native_prefs = [("0x0", cmap(0.15)), ("0xf", cmap(0.6))]   # 0x2 (adj-line off) dropped
    rows = []
    for noc in NOC_ALL:
        rec = {"noc": noc}
        for tag, trees in [("cont", NATIVE_CONT), ("shuf", NATIVE_SHUF)]:
            for pref, _ in native_prefs:
                rec[f"{tag}_{pref}"] = fmt(_get(trees[pref], noc, "cleansing_min"))
        for pref in ("0x0", "0xf"):
            rec[f"lazy_bidirR4_{pref}"] = fmt(
                _get(BIDIR_TREE.format(r=4, p=pref), noc, "cleansing_min"))
        rec["lazy_p3a1_0x0"] = fmt(_get(P3A1["0x0"], noc, "cleansing_min"))
        rows.append(rec)
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    for ax, (tag, trees, title) in zip(axes, [
            ("cont", NATIVE_CONT, "MastikElite's contiguous sets"),
            ("shuf", NATIVE_SHUF, "MastikElite's shuffled clusters")]):
        for pref, col in native_prefs:
            xs = [n for n in NOC_ALL if _get(trees[pref], n, "cleansing_min") is not None]
            ys = [_get(trees[pref], n, "cleansing_min") for n in xs]
            if xs:
                ax.plot(xs, ys, marker="o", color=col, label=f"MastikElite {pref}")
        for lazy, lbl, col, nocs in [
                (BIDIR_TREE.format(r=4, p="0x0"), "lazy bidirR4 pref0x0", "crimson", NOC_EV),
                (BIDIR_TREE.format(r=4, p="0xf"), "lazy bidirR4 pref0xf", "black", NOC_EV),
                (P3A1["0x0"], "lazy 3 passes 0x0", "darkorange", NOC_ALL)]:
            xs = [n for n in nocs if _get(lazy, n, "cleansing_min") is not None]
            ys = [_get(lazy, n, "cleansing_min") for n in xs]
            if xs:
                ax.plot(xs, ys, marker="s", linestyle="--", color=col, label=lbl)
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_ALL)
        ax.set_xticklabels(NOC_ALL)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.set_ylim(0, 1)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8, loc="lower left")
    axes[0].set_ylabel("cleansing rate (fraction of sets fully evicted)")
    fig.suptitle("Cleansing rate: real Mastik e-sets (guaranteed membership) vs lazy map")
    fig.tight_layout()
    out = FIGDIR / "native_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
def report_bidir():
    print("=" * 100)
    print("3. BIDIRECTIONAL SWEEP -- cleansing rate (-- = not collected)")
    print("=" * 100)
    rows = []
    for noc in NOC_EV:
        rec = {"noc": noc}
        for pref in ["0x0", "0xf"]:
            for r in BIDIR_R:
                rec[f"R{r}_{pref}"] = fmt(_get(BIDIR_TREE.format(r=r, p=pref), noc, "cleansing_min"))
        rec["base_p3a1_0xf"] = fmt(_get(P3A1["0xf"], noc, "cleansing_min"))
        rec["native_shuf_0xf"] = fmt(_get(NATIVE_SHUF["0xf"], noc, "cleansing_min"))
        rows.append(rec)
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
    cmap = plt.get_cmap("viridis")
    for ax, pref in zip(axes, ["0x0", "0xf"]):
        for i, r in enumerate(BIDIR_R):
            tree = BIDIR_TREE.format(r=r, p=pref)
            xs = [n for n in NOC_EV if _get(tree, n, "cleansing_min") is not None]
            ys = [_get(tree, n, "cleansing_min") for n in xs]
            if xs:
                ax.plot(xs, ys, marker="o", color=cmap(i / (len(BIDIR_R) - 1)), label=f"R={r}")
        for tree, lbl, col in [(P3A1["0xf"], "baseline p3a1_0xf", "crimson"),
                               (NATIVE_SHUF["0xf"], "native_shuf_0xf (guar. membership)", "black")]:
            xs = [n for n in NOC_EV if _get(tree, n, "cleansing_min") is not None]
            ys = [_get(tree, n, "cleansing_min") for n in xs]
            if xs:
                ax.plot(xs, ys, marker="s", linestyle="--", color=col, label=lbl)
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_EV)
        ax.set_xticklabels(NOC_EV)
        ax.set_xlabel("NoC")
        ax.set_title(f"pref{pref} ({'all on' if pref == '0x0' else 'all off'})")
        ax.set_ylim(0, 1)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=7, loc="lower left")
    axes[0].set_ylabel("cleansing rate (fraction of sets fully evicted)")
    fig.suptitle("Cleansing rate vs NoC, bidirectional sweep (vs baseline and real-e-set ceiling)")
    fig.tight_layout()
    out = FIGDIR / "bidir_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
def _value_blocks(ax, series, colors, finest=64):
    for i in range(finest):
        if i % 2 == 1:
            ax.axvspan(i, i + 1, color="0.90", zorder=0)
    allv = np.concatenate(list(series.values()))
    h = 0.012 * max(allv.max() - allv.min(), 1e-6)
    for z, (label, vals) in enumerate(series.items()):
        width = finest / len(vals)
        for c, v in enumerate(vals):
            ax.add_patch(Rectangle((c * width, v - h / 2), width, h,
                                   facecolor=colors[label], edgecolor="none", zorder=10 + z))
    return allv


def plot_percluster(noc=64):
    """Per-cluster cleansing at NoC=64: does the period-4 structure appear, and in which victims?"""
    cmap = plt.get_cmap("viridis")
    spec = [("native shuffled 0xf (guaranteed membership)", NATIVE_SHUF["0xf"], cmap(0.1)),
            ("lazy bidirR4 0xf", BIDIR_TREE.format(r=4, p="0xf"), cmap(0.55)),
            ("lazy p3a1 0xf (baseline)", P3A1["0xf"], cmap(0.9))]
    series, colors = {}, {}
    for label, tree, color in spec:
        m = tree_metrics(tree, noc)
        if m is not None:
            series[label] = m["per_cluster_min"]
            colors[label] = color
    if not series:
        return
    fig, ax = plt.subplots(figsize=(14, 5))
    vals = _value_blocks(ax, series, colors, noc)
    ax.set_xlim(0, noc)
    m_ = 0.08 * max(vals.max() - vals.min(), 1e-6)
    ax.set_ylim(max(0.0, vals.min() - m_), min(1.0, vals.max() + m_))
    ax.set_xlabel(f"cluster index at NoC={noc} (= line offset within page)")
    ax.set_ylabel("cleansing rate")
    ax.legend(handles=[Patch(facecolor=c, label=l) for l, c in colors.items()],
              loc="center left", bbox_to_anchor=(1.005, 0.5), fontsize=8)
    ax.grid(alpha=0.2, axis="y", zorder=1)
    ax.set_title(f"Per-cluster cleansing rate at NoC={noc} (all pref0xf)")
    fig.tight_layout()
    out = FIGDIR / f"percluster_noc{noc}.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def report_buffer():
    """Buffer size A/B on the SAME victim: bidirR4 pref0x0, 12MB vs 24MB.

    Only JSMAP_BUF_MB differs (mean 12 vs 24 lines per set), and both trees have n=10, so the
    min-over-samples statistics are directly comparable here -- unlike most cross-tree rows.

    Overlaid on a second y-axis: externally reported ExtraTrees accuracy (ACC_MEAN) for the two
    bidir configs plus the regular sweep, so the cleansing A/B can be read against accuracy.
    """
    print("=" * 100)
    print("3b. BUFFER SIZE -- bidirR4 pref0x0, 12MB vs 24MB (same victim, only buffer differs)")
    print("=" * 100)
    rows = []
    for noc in NOC_ALL:
        a, b = tree_metrics(BIDIR12, noc), tree_metrics(BIDIR24, noc)
        if a is None and b is None:
            continue
        rows.append({
            "noc": noc,
            "n_12MB": a["n"] if a else 0,
            "n_24MB": b["n"] if b else 0,
            "cleanse_12MB": fmt(a["cleansing_min"]) if a else "--",
            "cleanse_24MB": fmt(b["cleansing_min"]) if b else "--",
            "delta": (f"{b['cleansing_min'] - a['cleansing_min']:+.3f}" if (a and b) else "--"),
            "cov_12MB": fmt(a["coverage_min"]) if a else "--",
            "cov_24MB": fmt(b["coverage_min"]) if b else "--",
        })
    print(pd.DataFrame(rows).to_string(index=False))
    print()

    fig, ax = plt.subplots(figsize=(9, 5))
    cmap = plt.get_cmap("viridis")
    for i, (lbl, tree) in enumerate([("12MB", BIDIR12), ("24MB", BIDIR24)]):
        xs = [n for n in NOC_ALL if _get(tree, n, "cleansing_min") is not None]
        ys = [_get(tree, n, "cleansing_min") for n in xs]
        if xs:
            ax.plot(xs, ys, marker="o", color=cmap(0.2 + 0.5 * i), label=f"cleansing {lbl}")
    # Accuracy spans ~0.77-0.96, so it gets its own right-hand scale: on the 0-1 cleansing axis
    # the three curves would collapse into one indistinguishable band near the top.
    ax2 = ax.twinx()
    for lbl, acc in ACC_MEAN.items():
        xs = [n for n in NOC_ALL if n in acc]
        ax2.plot(xs, [acc[n] for n in xs], marker="^", linestyle="--",
                 color=ACC_COLOR[lbl], label=lbl)
    ax2.set_ylabel("ExtraTrees accuracy_mean")
    ax2.set_ylim(0.7, 1.0)
    ax.set_xscale("log", base=2)
    ax.set_xticks(NOC_ALL)
    ax.set_xticklabels(NOC_ALL)
    ax.set_xlabel("NoC")
    ax.set_ylabel("cleansing rate")
    ax.set_ylim(0, 1)
    ax.grid(alpha=0.3)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, fontsize=8, loc="lower left")
    ax.set_title("Buffer size A/B: bidirR4 pref0x0, 12MB vs 24MB (+ classifier accuracy)")
    fig.tight_layout()
    out = FIGDIR / "buffer_ab.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def report_period4(noc=64):
    """Quantify the period-4 structure: contrast between the low set and the rest."""
    print("=" * 100)
    print(f"4. PERIOD-4 STRUCTURE AT NoC={noc} -- contrast under cleansing vs coverage")
    print("   low set = {0,4,..,28, 33,37,..,61} (from SelfEviction_Analysis.md section 3)")
    print("=" * 100)

    def is_low(c):
        return (c % 4 == 0 and c <= 28) or ((c - 1) % 4 == 0 and c > 29)

    low = [c for c in range(noc) if is_low(c)]
    high = [c for c in range(noc) if not is_low(c)]
    rows = []
    for label, tree in [("native contiguous 0xf", NATIVE_CONT["0xf"]),
                        ("native shuffled 0xf", NATIVE_SHUF["0xf"]),
                        ("lazy p3a1 0xf", P3A1["0xf"]),
                        ("lazy p1a1 0x0", P1A1["0x0"]),
                        ("lazy bidirR4 0xf", BIDIR_TREE.format(r=4, p="0xf")),
                        ("lazy bidirR4 0x0 12MB", BIDIR12),
                        ("lazy bidirR4 0x0 24MB", BIDIR24)]:
        m = tree_metrics(tree, noc)
        if m is None:
            continue
        v = m["per_cluster_min"]
        lo, hi = v[low].mean(), v[high].mean()
        rows.append({"victim": label, "low_set": round(lo, 4), "other": round(hi, 4),
                     "contrast": f"{hi / lo:.1f}x" if lo > 1e-9 else "inf"})
    print(pd.DataFrame(rows).to_string(index=False))
    print()


def main():
    sanity_checks()
    report_baseline()
    report_native()
    report_bidir()
    report_buffer()
    plot_percluster(64)
    report_period4(64)


if __name__ == "__main__":
    main()
