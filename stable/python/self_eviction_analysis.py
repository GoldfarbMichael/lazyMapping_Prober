"""Self-eviction analysis: default sweep vs bidirectional sweep, across prefetcher configs.

Data schema per CSV row: cluster, nodeCount, M_cold, M_self.
M_cold  = misses on the first (always single-directional) cold pass -> calibration only.
M_self  = misses on the measured pass, after warmPasses-1 warm-up sweeps using the sweep
          under test (default: single-directional; bidir: sweep_lazy_bidir with R oscillations).
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
FIGDIR = Path(__file__).resolve().parent / "self_eviction_figures"
FIGDIR.mkdir(exist_ok=True)

NOC_VALUES = [2, 4, 8, 16, 32, 64]
PREF_CONFIGS = ["0x0", "0x1", "0x2", "0xf"]
BIDIR_R_VALUES = [1, 2, 4]  # R=8 was not collected (user decision)

DEFAULT_DIR_RE = re.compile(r"^selfevict_shuffled_pref0x([0-9a-f]+)$")
BIDIR_DIR_RE = re.compile(r"^selfevict_shuffled_bidirR(\d+)_pref0x([0-9a-f]+)$")


def load_tree(dir_path, sweep_type, pref_config, r_value):
    """Load every NoC<NN>/<iii>.csv file under one experiment-tree directory into long rows."""
    rows = []
    for noc in NOC_VALUES:
        noc_dir = dir_path / f"NoC{noc:02d}"
        if not noc_dir.is_dir():
            continue
        for csv_path in sorted(noc_dir.glob("*.csv")):
            iteration = int(csv_path.stem)
            df = pd.read_csv(csv_path)
            df["sweep_type"] = sweep_type
            df["pref_config"] = pref_config
            df["R"] = r_value
            df["noc"] = noc
            df["iteration"] = iteration
            rows.append(df)
    if not rows:
        return pd.DataFrame()
    return pd.concat(rows, ignore_index=True)


def load_all():
    frames = []
    for entry in sorted(DATA.iterdir()):
        if not entry.is_dir():
            continue
        m = DEFAULT_DIR_RE.match(entry.name)
        if m:
            cfg = "0x" + m.group(1)
            if cfg not in PREF_CONFIGS:
                continue
            frames.append(load_tree(entry, "default", cfg, np.nan))
            continue
        m = BIDIR_DIR_RE.match(entry.name)
        if m:
            r = int(m.group(1))
            cfg = "0x" + m.group(2)
            if cfg not in PREF_CONFIGS or r not in BIDIR_R_VALUES:
                continue
            frames.append(load_tree(entry, "bidir", cfg, r))
            continue
        # anything else (0x0_old, native_*, etc.) is out of scope for this script -- skip.
    data = pd.concat(frames, ignore_index=True)
    data["cold_ratio"] = data["M_cold"] / data["nodeCount"]
    data["self_frac_node"] = data["M_self"] / data["nodeCount"]
    data["self_frac_cold"] = data["M_self"] / data["M_cold"]
    data["cluster_frac"] = data["cluster"] / data["noc"]
    return data


def fmt_mean_ci(series):
    """Mean +/- 95% CI string, using a normal approximation (n is large: 30-50 per cell)."""
    n = series.count()
    mean = series.mean()
    ci = 1.96 * series.std(ddof=1) / np.sqrt(n) if n > 1 else float("nan")
    return f"{mean:.3f} +/- {ci:.3f}"


def condition_label(row):
    if row["sweep_type"] == "default":
        return "default"
    return f"bidirR{int(row['R'])}"


# ---------------------------------------------------------------------------
# 1. Sanity layer: M_cold/nodeCount, should be ~constant regardless of sweep type/R,
#    since the cold pass is always single-directional (see coverage_validator.c fix).
# ---------------------------------------------------------------------------
def report_sanity(data):
    print("=" * 90)
    print("1. SANITY: M_cold / nodeCount  (expect ~constant across sweep_type and R)")
    print("=" * 90)
    tmp = data.copy()
    tmp["condition"] = tmp.apply(condition_label, axis=1)
    table = (
        tmp.groupby(["condition", "pref_config", "noc"])["cold_ratio"]
        .agg(["mean", "std", "count"])
        .round(4)
    )
    print(table.to_string())
    overall = tmp.groupby("condition")["cold_ratio"].agg(["mean", "std"]).round(4)
    print("\nBy condition only (collapsed across config/NoC):")
    print(overall.to_string())
    print()


# ---------------------------------------------------------------------------
# 2. Core metric: self-eviction fraction, both normalizations, default sweep only.
# ---------------------------------------------------------------------------
def report_core_metric(data):
    print("=" * 90)
    print("2. CORE METRIC: self-eviction fraction, default sweep, per (config, NoC)")
    print("=" * 90)
    default = data[data["sweep_type"] == "default"]
    table = default.groupby(["pref_config", "noc"]).agg(
        self_frac_node=("self_frac_node", fmt_mean_ci),
        self_frac_cold=("self_frac_cold", fmt_mean_ci),
        n=("self_frac_node", "count"),
    )
    print(table.to_string())
    print()
    return default


def plot_core_metric_trend(default):
    fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharex=True, sharey=True)
    for metric, ax, title in [
        ("self_frac_node", axes[0], "M_self / nodeCount"),
        ("self_frac_cold", axes[1], "M_self / M_cold"),
    ]:
        for cfg in PREF_CONFIGS:
            sub = default[default["pref_config"] == cfg]
            g = sub.groupby("noc")[metric]
            means = g.mean().reindex(NOC_VALUES)
            n = g.count().reindex(NOC_VALUES)
            ci = 1.96 * g.std(ddof=1).reindex(NOC_VALUES) / np.sqrt(n)
            ax.errorbar(NOC_VALUES, means, yerr=ci, marker="o", label=cfg, capsize=3)
        ax.set_xscale("log", base=2)
        ax.set_xticks(NOC_VALUES)
        ax.set_xticklabels(NOC_VALUES)
        ax.set_xlabel("NoC")
        ax.set_title(title)
        ax.grid(alpha=0.3)
    axes[0].set_ylabel("self-eviction fraction")
    axes[0].legend(title="pref_config")
    fig.suptitle("Self-eviction fraction vs NoC, default sweep (mean +/- 95% CI over iterations)")
    fig.tight_layout()
    out = FIGDIR / "core_metric_trend.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 3. Cluster heterogeneity: floating value-blocks per config, on a fixed 64-tick x-axis (the
#    NoC=64 resolution -- exactly the line offset within a page, see lazy_map.c:51,71). A NoC=n
#    cluster c spans the x-range of NoC=64 clusters [c*(64/n), (c+1)*(64/n)) (top-bits
#    partition: doubling NoC subdivides every existing cluster into two, so this nesting is
#    exact, not approximate). Each block is drawn ONLY at its value's height (a thin rectangle
#    centered on self_frac_node, not a bar rising from 0) -- this is a "scatter with width"
#    (matplotlib calls the horizontal-floating-bar primitive broken_barh), not a stacked bar
#    chart, so no NoC layer visually covers another just by being taller. Coarser NoC drawn
#    first, finer NoC last, so only near-exact overlaps (similar value AND overlapping x-range)
#    have a front/back winner.
# ---------------------------------------------------------------------------
FINEST_NOC = 64  # x-axis resolution


def plot_cluster_heterogeneity(default):
    fig, axes = plt.subplots(2, 2, figsize=(13, 10), sharex=True, sharey=True)
    cmap = plt.get_cmap("viridis")
    colors = {noc: cmap(i / (len(NOC_VALUES) - 1)) for i, noc in enumerate(NOC_VALUES)}

    y_all = default["self_frac_node"]
    block_h = 0.012 * (y_all.max() - y_all.min())  # fixed thickness, data units, not a baseline bar

    for ax, cfg in zip(axes.flat, PREF_CONFIGS):
        # alternating background bands, one per finest-resolution (page-offset) column
        for i in range(FINEST_NOC):
            if i % 2 == 1:
                ax.axvspan(i, i + 1, color="0.90", zorder=0)

        sub = default[default["pref_config"] == cfg]
        per_cluster = sub.groupby(["noc", "cluster"])["self_frac_node"].mean()

        for i, noc in enumerate(NOC_VALUES):  # ascending: coarsest first (back), finest last (front)
            width = FINEST_NOC / noc
            values = per_cluster.loc[noc].reindex(range(noc)).values
            for c, v in enumerate(values):
                ax.add_patch(Rectangle((c * width, v - block_h / 2), width, block_h,
                                        facecolor=colors[noc], edgecolor="none", zorder=10 + i))

        ax.set_title(f"pref_config = {cfg}")
        ax.grid(alpha=0.2, axis="y", zorder=1)

    # add_patch() doesn't feed autoscale, so set the view limits explicitly (shared axes).
    y_margin = 0.08 * (y_all.max() - y_all.min())
    axes[0, 0].set_xlim(0, FINEST_NOC)
    axes[0, 0].set_ylim(y_all.min() - y_margin, y_all.max() + y_margin)
    handles = [Patch(facecolor=colors[noc], label=str(noc)) for noc in NOC_VALUES]
    axes[0, 0].legend(handles=handles, title="NoC (front = finer)", loc="upper left", fontsize=8)
    for ax in axes[-1, :]:
        ax.set_xlabel("line offset within page (0-63) -- exact at NoC=64; coarser NoC = wider block")
    for ax in axes[:, 0]:
        ax.set_ylabel("self_frac_node (per-cluster mean)")
    fig.suptitle("Cluster heterogeneity, default sweep -- value blocks by NoC (no baseline)")
    fig.tight_layout()
    out = FIGDIR / "cluster_heterogeneity.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 4. Prefetcher comparison: table + grouped bar chart, default sweep.
# ---------------------------------------------------------------------------
def report_prefetcher_comparison(default):
    print("=" * 90)
    print("4. PREFETCHER COMPARISON: self_frac_node by config, grouped by NoC (default sweep)")
    print("=" * 90)
    table = default.groupby(["noc", "pref_config"])["self_frac_node"].mean().unstack("pref_config")
    table = table.reindex(index=NOC_VALUES, columns=PREF_CONFIGS)
    print(table.round(4).to_string())
    print()

    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(NOC_VALUES))
    width = 0.8 / len(PREF_CONFIGS)
    for i, cfg in enumerate(PREF_CONFIGS):
        ax.bar(x + i * width, table[cfg].values, width, label=cfg)
    ax.set_xticks(x + width * (len(PREF_CONFIGS) - 1) / 2)
    ax.set_xticklabels(NOC_VALUES)
    ax.set_xlabel("NoC")
    ax.set_ylabel("self_frac_node (mean)")
    ax.set_title("Self-eviction fraction by prefetcher config, default sweep")
    ax.legend(title="pref_config")
    ax.grid(alpha=0.3, axis="y")
    fig.tight_layout()
    out = FIGDIR / "prefetcher_comparison_bar.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


# ---------------------------------------------------------------------------
# 5. Bidir vs default: signed relative delta of self_frac_node, per (config, noc, R).
# ---------------------------------------------------------------------------
def report_bidir_delta(data):
    print("=" * 90)
    print("5. BIDIR vs DEFAULT: signed %% change in self_frac_node, per (config, NoC, R)")
    print("=" * 90)
    default_mean = (
        data[data["sweep_type"] == "default"]
        .groupby(["pref_config", "noc"])["self_frac_node"].mean()
    )
    bidir_mean = (
        data[data["sweep_type"] == "bidir"]
        .groupby(["pref_config", "noc", "R"])["self_frac_node"].mean()
    )
    rows = []
    for (cfg, noc, r), bval in bidir_mean.items():
        dval = default_mean.get((cfg, noc))
        if dval is None or dval == 0:
            continue
        rows.append({
            "pref_config": cfg, "noc": noc, "R": int(r),
            "default": round(dval, 4), "bidir": round(bval, 4),
            "delta_abs": round(bval - dval, 4),
            "delta_pct": round(100 * (bval - dval) / dval, 1),
        })
    table = pd.DataFrame(rows).sort_values(["pref_config", "R", "noc"])
    print(table.to_string(index=False))
    print()
    return table


# ---------------------------------------------------------------------------
# 6. Variability: box plot of self_frac_node across iterations, per (config, NoC), default sweep.
# ---------------------------------------------------------------------------
def plot_variability(default):
    # One point per iteration (mean over that iteration's clusters), so the box shows
    # iteration-to-iteration spread -- not conflated with within-iteration cluster spread
    # (that's what the heterogeneity scatter is for).
    per_iter = default.groupby(["pref_config", "noc", "iteration"])["self_frac_node"].mean().reset_index()

    fig, axes = plt.subplots(1, len(PREF_CONFIGS), figsize=(18, 5), sharey=True)
    for ax, cfg in zip(axes, PREF_CONFIGS):
        sub = per_iter[per_iter["pref_config"] == cfg]
        box_data = [sub[sub["noc"] == noc]["self_frac_node"].values for noc in NOC_VALUES]
        ax.boxplot(box_data, tick_labels=[str(n) for n in NOC_VALUES])
        ax.set_title(f"pref_config = {cfg}")
        ax.set_xlabel("NoC")
        ax.grid(alpha=0.3, axis="y")
    axes[0].set_ylabel("self_frac_node (per-iteration mean over clusters)")
    fig.suptitle("Iteration-to-iteration variability, default sweep")
    fig.tight_layout()
    out = FIGDIR / "variability_boxplot.png"
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"[fig] {out}")


def main():
    data = load_all()
    print(f"Loaded {len(data)} rows from {DATA}")
    print(f"Conditions present: "
          f"{sorted(data.apply(condition_label, axis=1).unique())}")
    print(f"pref_configs present: {sorted(data['pref_config'].unique())}")
    print()

    report_sanity(data)
    default = report_core_metric(data)
    plot_core_metric_trend(default)
    plot_cluster_heterogeneity(default)
    report_prefetcher_comparison(default)
    report_bidir_delta(data)
    plot_variability(default)


if __name__ == "__main__":
    main()
