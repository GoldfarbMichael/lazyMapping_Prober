#!/usr/bin/env python3
"""
coverage_compare.py -- Phase-1 diagnostic for the lazy-map coverage drop.

Reuses the EXACT metric logic from coverage_analysis.ipynb, but runs it across
the three control trees at once so we can separate the two candidate mechanisms:

  native                 : real Mastik e-sets, guaranteed 12 collisions/set, CONTIGUOUS sweep
  native_shuffled        : real Mastik e-sets, guaranteed 12 collisions/set, SCATTERED sweep
  native_shuffled_p1a3   : jsmap lazy victim,  statistical (~12 mean),        SCATTERED sweep

Decision rule:
  native_shuffled ~= native ~= 1.0 flat  -> scatter/policy irrelevant -> MEMBERSHIP-limited
  native_shuffled drops with NoC         -> scatter+prefetch matters   -> POLICY-limited

Read-only; no sudo, no rebuild. Run from stable/:  python3 python/coverage_compare.py
"""
import re
import sys
from pathlib import Path
import numpy as np
import pandas as pd

STABLE = Path(__file__).resolve().parent.parent
DATA = STABLE / "data" / "coverage"
ASSOC = 12
PAGE_OFFSET_BITS = 12
NOC_VALUES = [8, 16, 32, 64]
MAX_SAMPLES = 2        # -1 = use every sample per NoC; else use the first MAX_SAMPLES

def select_csvs(noc_dir):
    """Sorted CSVs for one NoC dir, truncated to MAX_SAMPLES (or all if -1)."""
    csvs = sorted(noc_dir.glob("*.csv"))
    return csvs if MAX_SAMPLES < 0 else csvs[:MAX_SAMPLES]

# Reference trees (control ladder): contiguous real e-sets -> scattered real e-sets ->
# scattered lazy baseline. The eviction-strategy runs are compared against these.
BASELINE = "native_jsmap_shuffled_p1a1_same"          # lazy chase, no eviction strategy (EV off)
REF_TREES = ["native", "native_shuffled", BASELINE, "chrome"]  # + real Chrome (mock-clock) coverage
# Eviction-strategy runs are auto-discovered: BASELINE + "_evA{A}D{D}C{C}".
PREFIX_EV = "native_jsmap_shuffled_p1a1" 
EV_GLOB = f"{PREFIX_EV}_ev*"

def list_ev_trees():
    """Return [(name, A, D, C), ...] for every eviction-strategy tree on disk."""
    out = []
    for p in sorted(DATA.glob(EV_GLOB)):
        m = re.search(r"_evA(\d+)D(\d+)C(\d+)$", p.name)
        if p.is_dir() and m:
            out.append((p.name, int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out

# ---- metric functions copied verbatim from coverage_analysis.ipynb ----

def load_miss_matrix(csv_path, noc):
    data = pd.read_csv(csv_path).to_numpy(dtype=float)
    cluster_rows = data[:noc]
    baseline_rows = data[noc:]
    baseline_row = baseline_rows.mean(axis=0)
    return cluster_rows, baseline_row

def compute_aggregated_matrix(cluster_rows, phys_cluster_arr, noc):
    agg = np.zeros((noc, noc))
    for g in range(noc):
        mask = phys_cluster_arr == g
        if mask.any():
            agg[:, g] = cluster_rows[:, mask].mean(axis=1)
    return agg

def compute_baseline_vector(baseline_row, phys_cluster_arr, noc):
    base = np.zeros(noc)
    for g in range(noc):
        mask = phys_cluster_arr == g
        if mask.any():
            base[g] = baseline_row[mask].mean()
    return base

def subtract_baseline(agg_matrix, base_vector):
    return np.clip(agg_matrix - base_vector[None, :], 0.0, None)

def compute_diagonal_mass(subtracted_matrix):
    total = subtracted_matrix.sum()
    return 0.0 if total <= 0 else np.trace(subtracted_matrix) / total

# ---- helpers ----

def phys_clusters_for(tree, noc):
    labels_file = DATA / tree / "set_labels.csv"
    if not labels_file.exists():
        return None
    labels = pd.read_csv(labels_file).sort_values("set_idx").reset_index(drop=True)
    pa = labels["pa"].apply(lambda s: int(str(s), 16)).to_numpy(dtype=np.int64)
    shift = PAGE_OFFSET_BITS - int(round(np.log2(noc)))
    return (pa >> shift) & (noc - 1)

def process_tree_noc(tree, noc):
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    csvs = select_csvs(noc_dir)
    if not csvs:
        return None
    pc = phys_clusters_for(tree, noc)
    if pc is None:
        return None
    raw_list, sub_list = [], []
    diag_masses = []
    for c in csvs:
        cluster_rows, baseline_row = load_miss_matrix(c, noc)
        raw = compute_aggregated_matrix(cluster_rows, pc, noc)
        base = compute_baseline_vector(baseline_row, pc, noc)
        sub = subtract_baseline(raw, base)
        raw_list.append(raw)
        sub_list.append(sub)
        diag_masses.append(compute_diagonal_mass(sub))
    min_raw = np.min(raw_list, axis=0)              # notebook "min over samples"
    mean_raw = np.mean(raw_list, axis=0)
    min_raw_diag = np.diag(min_raw)                 # per-cluster worst-case diag miss
    return {
        "n_samples": len(csvs),
        "coverage": float(min_raw_diag.mean() / ASSOC),   # notebook "Coverage"
        "min_raw_diag_mean": float(min_raw_diag.mean()),
        "mean_raw_diag_mean": float(np.diag(mean_raw).mean()),
        "diag_mass": float(np.mean(diag_masses)),
        "per_cluster_diag": min_raw_diag,
    }

def per_set_ways_hist(tree, noc):
    """Distribution of per-diagonal-set ways-evicted (raw miss), pooled over samples.
    For each cluster c, take the raw miss of every set whose physical cluster == c."""
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    csvs = select_csvs(noc_dir)
    pc = phys_clusters_for(tree, noc)
    if pc is None or not csvs:
        return np.array([])
    vals = []
    for cpath in csvs:
        cluster_rows, _ = load_miss_matrix(cpath, noc)  # (noc x numSets)
        for c in range(noc):
            mask = pc == c
            vals.append(cluster_rows[c, mask])
    return np.concatenate(vals) if vals else np.array([])

# ---- run ----

def cov_row(tree):
    """Coverage per NoC (or None) as a dict, plus n_samples of the last present NoC."""
    row = {"n": 0}
    for noc in NOC_VALUES:
        r = process_tree_noc(tree, noc)
        row[noc] = (r["coverage"] if r else None)
        if r:
            row["n"] = r["n_samples"]
    return row

def fmt_cov(x):
    return "  --  " if x is None else f"{x:>6.3f}"

def main():
    # ---- 1. Control ladder: contiguous -> scattered real e-sets -> lazy baseline ----
    print("=" * 84)
    print("CONTROL LADDER  (Coverage = min-over-samples mean diagonal raw miss / 12)")
    print("=" * 84)
    refcov = {}
    ladder = []
    for tree in REF_TREES:
        row = cov_row(tree)
        refcov[tree] = row
        rec = {"tree": tree, "n": row["n"]}
        for n in NOC_VALUES:
            rec[f"NoC{n}"] = row[n]
        ladder.append(rec)
    cov_fmt = lambda x: "--" if pd.isna(x) else f"{x:.3f}"
    ladder_fmts = {f"NoC{n}": cov_fmt for n in NOC_VALUES}
    tree_w = max(len(r["tree"]) for r in ladder)
    ladder_fmts["tree"] = lambda s: s.ljust(tree_w)
    print(pd.DataFrame(ladder).to_string(index=False, formatters=ladder_fmts, justify="left"))
    base64 = refcov[BASELINE][64]
    ceil_shuf = refcov["native_shuffled"][64]
    ceil_cont = refcov["native"][64]

    # ---- 2. Eviction-strategy grid, ranked by NoC=64 coverage ----
    ev = list_ev_trees()
    print("\n" + "=" * 84)
    print(f"EVICTION-STRATEGY GRID (base={BASELINE}), ranked by NoC=64 coverage")
    print(f"references @NoC64:  native(contiguous)={fmt_cov(ceil_cont)}  "
          f"native_shuffled(scattered,12/set)={fmt_cov(ceil_shuf)}  lazy baseline={fmt_cov(base64)}")
    print("=" * 84)
    rows = []
    for name, A, D, C in ev:
        row = cov_row(name)
        rows.append((A, D, C, row, name))          # keep the ACTUAL dir name
    rows.sort(key=lambda t: (t[3][64] is not None, t[3][64] if t[3][64] is not None else -1),
              reverse=True)
    records = []
    for A, D, C, row, _name in rows:
        rec = {"A": A, "D": D, "C": C}
        for n in NOC_VALUES:
            rec[f"NoC{n}"] = row[n]
        rec["d64_vs_base"] = None if (row[64] is None or base64 is None) else row[64] - base64
        note = ""
        if row[64] is not None:
            if ceil_shuf is not None and row[64] >= ceil_shuf:
                note = ">= scattered ceiling"
            elif base64 is not None and row[64] > base64 + 0.02:
                note = "beats baseline"
        rec["note"] = note
        records.append(rec)
    grid_df = pd.DataFrame(records)
    cov_fmt = lambda x: "--" if pd.isna(x) else f"{x:.3f}"
    fmts = {f"NoC{n}": cov_fmt for n in NOC_VALUES}
    fmts["d64_vs_base"] = lambda x: "" if pd.isna(x) else f"{x:+.3f}"
    print(grid_df.to_string(index=False, formatters=fmts, justify="right"))

    # ---- 3. Ways-evicted histograms: best EV config vs the references, at NoC=64/32 ----
    best = rows[0] if rows else None
    bins = [0, 1, 2, 4, 6, 8, 10, 11, 12, 13]  # 12 = fully evicted; >12 clipped by hw
    for noc in [64, 32]:
        print("\n" + "=" * 84)
        print(f"WAYS-EVICTED DISTRIBUTION @NoC={noc}  (bimodal {{0,12}} => membership | "
              "unimodal ~7 => policy)")
        print("=" * 84)
        print(f"{'tree':<34}{'mean':>7}{'med':>5}{'%>=12':>7}{'%<=2':>6}   "
              "hist(<1,1-3,3-5,5-7,7-9,9-11,11,12,>12)")
        show = list(REF_TREES)
        if best is not None:
            show.append(best[4])                   # the actual best-EV dir name (no reconstruction)
        for tree in show:
            v = per_set_ways_hist(tree, noc)
            if v.size == 0:
                print(f"{tree:<34}  -- missing --")
                continue
            h, _ = np.histogram(v, bins=bins)
            hpct = (h / v.size * 100).round(0).astype(int)
            print(f"{tree:<34}{v.mean():>7.2f}{np.median(v):>5.0f}"
                  f"{(v >= 12).mean()*100:>6.0f}%{(v <= 2).mean()*100:>5.0f}%   {hpct.tolist()}")

if __name__ == "__main__":
    main()
