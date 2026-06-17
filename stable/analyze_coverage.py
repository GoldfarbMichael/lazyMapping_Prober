#!/usr/bin/env python3
"""
Lazy cluster x physical-cluster coverage analysis.

Cross-references the coverage validator's outputs:
  - coverage_miss_matrix.csv : header S0..S{N-1}; one row per scanned lazy CLUSTER,
                               then a final BASELINE row (browser idle). Cell = JS-induced
                               miss count (out of `assoc`) for that cluster on that set.
                               So the file has NoC + 2 lines: header + NoC clusters + baseline.
  - coverage_set_labels.csv  : set_idx, pa, bits8_11   (pa = physical address of the set)

The lazy mapping assigns a cluster to an address from translation-invariant page-offset
bits: cluster = (addr >> (12 - log2(NoC))) & (NoC - 1)  (bits 8-11 at NoC=16, bits 6-11
at NoC=64). We recompute that same field from each set's physical address `pa`, giving the
"physical cluster" a Mastik set belongs to. A correct lazy mapping => cluster row c lights
up physical-cluster column c: an identity diagonal.

Outputs a 3-panel heatmap (raw / baseline-subtracted / row-normalized), an aggregated
cluster x group CSV, and a console summary. NoC is inferred from the matrix (cluster rows).
"""

import argparse
import os
import sys

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")  # headless: write a PNG, never open a window
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

PAGE_OFFSET_BITS = 12  # cluster bit-field is [12 - log2(NoC) .. 11], within the page offset


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--matrix", default="coverage_miss_matrix.csv",
                   help="per-cluster miss matrix (last row = baseline)")
    p.add_argument("--labels", default="coverage_set_labels.csv",
                   help="set_idx,pa,... labels (pa = physical address)")
    p.add_argument("--assoc", type=int, default=12,
                   help="LLC associativity (max misses per set); color scale for panels A/B")
    p.add_argument("--noc", type=int, default=None,
                   help="NoC (number of physical-cluster columns). Default: #cluster rows "
                        "in the matrix (use this only if you scanned fewer clusters than NoC)")
    p.add_argument("--no-baseline", action="store_true",
                   help="do NOT subtract the baseline row before panels B/C")
    p.add_argument("--out", default="coverage_cluster_group_heatmap.png",
                   help="output PNG path")
    p.add_argument("--csv-out", default="coverage_cluster_group.csv",
                   help="output aggregated cluster x group CSV path")
    return p.parse_args()


def load_inputs(matrix_path, labels_path):
    if not os.path.isfile(matrix_path):
        sys.exit(f"missing matrix file: {matrix_path}")
    if not os.path.isfile(labels_path):
        sys.exit(f"missing labels file: {labels_path}")

    # Miss matrix: header row (S0..S{N-1}) is consumed by read_csv, leaving NoC cluster
    # rows + 1 baseline row.  cluster_rows = all but last; NoC is then len(cluster_rows).
    miss = pd.read_csv(matrix_path).to_numpy(dtype=float)
    if miss.shape[0] < 2:
        sys.exit("matrix needs at least one cluster row + a baseline row")
    cluster_rows = miss[:-1]
    baseline_row = miss[-1]

    labels = pd.read_csv(labels_path)
    for col in ("set_idx", "pa"):
        if col not in labels.columns:
            sys.exit(f"labels file missing column '{col}'")
    labels = labels.sort_values("set_idx").reset_index(drop=True)  # align to matrix col order
    # pa is hex text like "0x590a80000"; int(...,16) also accepts a plain/decimal fallback.
    pa = labels["pa"].apply(lambda s: int(str(s), 16)).to_numpy(dtype=np.int64)

    n_sets = cluster_rows.shape[1]
    if len(pa) != n_sets:
        sys.exit(f"set count mismatch: matrix has {n_sets} cols, labels has {len(pa)} rows")
    return cluster_rows, baseline_row, pa


def physical_cluster(pa, noc):
    """The lazy-mapping cluster each set's PA belongs to, for this NoC:
    (pa >> (12 - log2(NoC))) & (NoC - 1)  -- the same bit-field LazyMapping uses."""
    shift = PAGE_OFFSET_BITS - int(round(np.log2(noc)))
    return (pa >> shift) & (noc - 1)


def group_means(row, groups, num_groups):
    """Mean miss per physical-cluster group (0..num_groups-1) for one per-set row."""
    out = np.zeros(num_groups)
    for g in range(num_groups):
        sel = groups == g
        out[g] = row[sel].mean() if sel.any() else 0.0
    return out


def main():
    args = parse_args()
    cluster_rows, baseline_row, pa = load_inputs(args.matrix, args.labels)
    n_clusters = cluster_rows.shape[0]          # inferred NoC (cluster rows in the matrix)
    noc = args.noc if args.noc else n_clusters  # number of physical-cluster columns
    num_groups = noc
    groups = physical_cluster(pa, noc)
    if groups.min() < 0 or groups.max() >= num_groups:
        sys.exit(f"computed group out of range [0,{num_groups - 1}]: "
                 f"got [{groups.min()},{groups.max()}] (check --noc)")

    # Aggregate: (n_clusters x num_groups) mean-miss-per-group, plus the baseline.
    raw = np.vstack([group_means(cluster_rows[c], groups, num_groups) for c in range(n_clusters)])
    base = group_means(baseline_row, groups, num_groups)

    if args.no_baseline:
        sub = raw.copy()
        base_label = "(baseline NOT subtracted)"
    else:
        sub = np.clip(raw - base[None, :], 0.0, None)
        base_label = "(baseline-subtracted)"

    # Row-normalized concentration view (each cluster's distribution over groups).
    row_sums = sub.sum(axis=1, keepdims=True)
    norm = np.divide(sub, row_sums, out=np.zeros_like(sub), where=row_sums > 0)

    # ---- console summary ----
    counts = np.array([(groups == g).sum() for g in range(num_groups)])
    print(f"clusters(rows)={n_clusters}  noc(groups)={noc}  assoc={args.assoc}")
    print(f"sets per physical-cluster group: min={counts.min()} max={counts.max()} "
          f"mean={counts.mean():.1f}")
    print("\nper-cluster correspondence (expected: cluster c -> group c):")
    diag_fracs = []
    for c in range(n_clusters):
        dom = int(np.argmax(norm[c])) if norm[c].sum() > 0 else -1
        share = norm[c, dom] if dom >= 0 else 0.0
        # diagonal mass: fraction of this cluster's signal landing in its own group c.
        diag = sub[c, c] / sub[c].sum() if (c < num_groups and sub[c].sum() > 0) else 0.0
        diag_fracs.append(diag)
        match = "Y" if dom == c else "n"
        print(f"  cluster {c:3d}: dominant group {dom:3d} (share {share:4.0%}) | "
              f"diag-mass {diag:4.0%} [{match}]")
    print(f"\noverall diagonal-mass fraction: {np.mean(diag_fracs):.1%} "
          f"(1.0 = every cluster's misses land exactly on its own group)")

    # ---- aggregated CSV ----
    agg = pd.DataFrame(raw, columns=[f"g{g}" for g in range(num_groups)])
    agg.insert(0, "cluster", np.arange(n_clusters))
    base_df = pd.DataFrame([["baseline", *base]], columns=["cluster"] + list(agg.columns[1:]))
    pd.concat([agg, base_df], ignore_index=True).to_csv(args.csv_out, index=False)
    print(f"\nwrote aggregated table -> {args.csv_out}")

    # ---- heatmaps: 3 SEPARATE figures so every cell value stays legible ----
    suptitle_noc = f"NoC={noc}, diag-mass {np.mean(diag_fracs):.0%}"
    panels = [
        ("A_raw", "A. raw mean miss", raw, 0, args.assoc, "viridis"),
        ("B_signal", f"B. JS signal {base_label}", sub, 0, args.assoc, "viridis"),
        ("C_norm", "C. row-normalized (concentration)", norm, 0, max(norm.max(), 1e-9), "magma"),
    ]
    base_out, ext = os.path.splitext(args.out)
    if not ext:
        ext = ".png"
    for suffix, title, data, vmin, vmax, cmap in panels:
        out_path = f"{base_out}_{suffix}{ext}"
        plot_panel(data, title, vmin, vmax, cmap, n_clusters, num_groups, suptitle_noc, out_path)
        print(f"wrote heatmap -> {out_path}")


def plot_panel(data, title, vmin, vmax, cmap, n_clusters, num_groups, suptitle_noc, out_path):
    """One full-size heatmap with every cell annotated (2 decimals, unrounded means)."""
    cell = 0.55  # inches per cell -> big enough that the numbers are readable when zoomed
    fig, ax = plt.subplots(figsize=(max(6.0, num_groups * cell + 2.5),
                                    max(5.0, n_clusters * cell + 2.0)))
    im = ax.imshow(data, aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax,
                   origin="upper", interpolation="nearest")
    ax.set_title(f"{title}   ({suptitle_noc})", fontsize=12)
    ax.set_xlabel("physical cluster  (PA bits)")
    ax.set_ylabel("lazy cluster (hammered)")
    ax.set_xticks(range(num_groups))
    ax.set_yticks(range(n_clusters))
    ax.tick_params(labelsize=7)
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)

    thr = (vmin + vmax) / 2
    for c in range(n_clusters):
        for g in range(num_groups):
            v = data[c, g]
            ax.text(g, c, f"{v:.2f}", ha="center", va="center", fontsize=5,
                    color="white" if v < thr else "black")
    # outline the expected-diagonal cells (cluster c <-> group c) for reference
    for c in range(min(n_clusters, num_groups)):
        ax.add_patch(Rectangle((c - 0.5, c - 0.5), 1, 1, fill=False,
                               edgecolor="red", lw=1.0))

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


if __name__ == "__main__":
    main()
