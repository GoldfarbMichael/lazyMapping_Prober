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
NOC_VALUES = [2,4,8, 16, 32, 64]
MAX_SAMPLES = -1        # -1 = use every sample per NoC; else use the first MAX_SAMPLES

def select_csvs(noc_dir):
    """Sorted CSVs for one NoC dir, truncated to MAX_SAMPLES (or all if -1)."""
    csvs = sorted(noc_dir.glob("*.csv"))
    return csvs if MAX_SAMPLES < 0 else csvs[:MAX_SAMPLES]

# Reference trees (control ladder): contiguous real e-sets -> scattered real e-sets ->
# scattered lazy baseline. The eviction-strategy runs are compared against these.
BASELINE = "native_jsmap_shuffled_p1a1_same"          # lazy chase, prefetch-ON (all prefetchers on = 0x0)
BASELINE_PREF_OFF = BASELINE + "_pref0x2"             # SAME victim, L2 adjacent-line prefetcher OFF (MSR 0x1a4=0x2)
NATIVE_CONT_OFF = "native_pref0x2"                    # contiguous real e-sets (guaranteed 12/set), adj-line OFF
NATIVE_SHUF_OFF = "native_shuffled_pref0x2"           # scattered  real e-sets (guaranteed 12/set), adj-line OFF
NATIVE_CONT_ALLOFF = "native_pref0xf"                 # contiguous real e-sets, ALL prefetchers OFF (0x1a4=0xf)
NATIVE_SHUF_ALLOFF = "native_shuffled_pref0xf"        # scattered  real e-sets, ALL prefetchers OFF
REF_TREES = ["native", NATIVE_CONT_OFF, NATIVE_CONT_ALLOFF,
             "native_shuffled", NATIVE_SHUF_OFF, NATIVE_SHUF_ALLOFF,
             BASELINE, BASELINE_PREF_OFF, "chrome"]  # + real Chrome (mock-clock) coverage
# Rowhammer.js eviction-strategy runs auto-discovered:
# native_jsmap_shuffled_p1a1_evA{A}D{D}C{C}[_pref0x<mask>][_<MB>MB]. The regex tolerates the
# prefetcher (_pref0x..) and buffer (_NMB) suffixes so 0xf/24MB runs are no longer skipped;
# untagged = prefetch-ON (0x0), 12 MB.
PREFIX_EV = "native_jsmap_shuffled_p1a1"
EV_GLOB = f"{PREFIX_EV}_ev*"
EV_RE = re.compile(r"_evA(\d+)D(\d+)C(\d+)(?:_pref(0x[0-9a-fA-F]+))?(?:_(\d+)MB)?$")

def list_ev_trees():
    """Return [(name, A, D, C, pref, mb), ...] for every eviction-strategy tree on disk.
    pref defaults to '0x0' (untagged = all prefetchers ON); mb defaults to 12."""
    out = []
    for p in sorted(DATA.glob(EV_GLOB)):
        if not p.is_dir():
            continue
        m = EV_RE.search(p.name)
        if not m:
            continue
        out.append((p.name, int(m.group(1)), int(m.group(2)), int(m.group(3)),
                    m.group(4) or "0x0", int(m.group(5) or 12)))
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

# ---- self-eviction (PMU) analysis ----
# selfevict trees have a DIFFERENT csv format (one row per cluster):
#   cluster,nodeCount,M_cold,M_self
# M_cold = demand L3 misses on a cold sweep (~= nodeCount, counter sanity);
# M_self = demand L3 misses on a warm second sweep (== self-evicted lines).
#
# Prefetcher sweep: each tree selfevict_shuffled_pref<MASK> was collected with MSR 0x1a4
# (MSR_MISC_FEATURE_CONTROL) = MASK, where a set bit DISABLES one prefetcher:
#   0x0 = all prefetchers ON        0x1 = L2 streamer OFF (adj-line still on)
#   0x2 = L2 adjacent-line OFF      0xf = all four OFF
# The 0x0 tree is the apples-to-apples partner for the (prefetch-ON) jsmap coverage.
SELFEVICT_TREE = "selfevict_shuffled_pref0x0"

# Prefetcher masks, in presentation order, with human labels for the 0x1a4 bits.
PREFETCH_MASKS = [
    ("0x0", "all ON"),
    ("0x1", "streamer OFF"),
    ("0x2", "adj-line OFF"),
    ("0xf", "all OFF"),
]

def selfevict_percluster(tree, noc):
    """Mean-over-samples per-cluster (M_cold/node, M_self/node) arrays, or (None,None)."""
    noc_dir = DATA / tree / f"NoC{noc:02d}"
    csvs = select_csvs(noc_dir)
    if not csvs:
        return None, None
    cold, self_ = [], []
    for c in csvs:
        df = pd.read_csv(c).sort_values("cluster")
        node = df["nodeCount"].to_numpy(dtype=float)
        node = np.where(node == 0, np.nan, node)
        cold.append(df["M_cold"].to_numpy(dtype=float) / node)
        self_.append(df["M_self"].to_numpy(dtype=float) / node)
    return np.mean(cold, axis=0), np.mean(self_, axis=0)

def report_prefetch_sweep(refcov):
    """Compare M_cold and M_self across the 0x1a4 prefetcher masks, and attribute the
    NoC=32 odd-cluster self-eviction to a specific prefetcher via the odd/even split."""
    present = [(m, lbl) for m, lbl in PREFETCH_MASKS
               if (DATA / f"selfevict_shuffled_pref{m}").is_dir()]
    if not present:
        return
    covrow = refcov.get(BASELINE, {})   # prefetch-ON coverage (for the 1-cov reference)

    def mask_table(metric_idx, title):
        print("\n" + "=" * 84)
        print(title)
        print("  masks: " + " | ".join(f"{m}={lbl}" for m, lbl in present))
        print("=" * 84)
        hdr = f"{'NoC':>5}" + "".join(f"{m:>10}" for m, _ in present)
        print(hdr)
        for noc in NOC_VALUES:
            cells = []
            for m, _ in present:
                _, per = (None, None)
                cold, self_ = selfevict_percluster(f"selfevict_shuffled_pref{m}", noc)
                arr = cold if metric_idx == 0 else self_
                cells.append("--" if arr is None else f"{arr.mean():.3f}")
            print(f"{noc:>5}" + "".join(f"{c:>10}" for c in cells))

    mask_table(0, "PREFETCHER SWEEP -- M_cold/node  (cold pass; ~1.0 means no prefetch save)")
    mask_table(1, "PREFETCHER SWEEP -- M_self/node  (warm pass; = self-evicted fraction)")

    # NoC=32 attribution: which mask un-masks the odd-cluster self-eviction?
    if 32 in NOC_VALUES:
        print("\n" + "=" * 84)
        print("NoC=32 ATTRIBUTION -- M_self by cluster parity, per mask")
        print("  (odd clusters = PA bit7 = 1; a big odd-even gap = self-eviction the mask reveals)")
        print("=" * 84)
        print(f"{'mask':>14}{'even':>9}{'odd':>9}{'odd-even':>10}{'r(M_self,1-cov_ON)':>20}")
        cov32 = covrow.get(32)
        inv32 = None
        if cov32 is not None:
            r = process_tree_noc(BASELINE, 32)
            inv32 = None if r is None else 1.0 - r["per_cluster_diag"] / ASSOC
        for m, lbl in present:
            _, self_ = selfevict_percluster(f"selfevict_shuffled_pref{m}", 32)
            if self_ is None:
                continue
            idx = np.arange(len(self_)); odd = idx % 2 == 1
            ev, od = self_[~odd].mean(), self_[odd].mean()
            rtxt = "--"
            if inv32 is not None and np.std(self_) > 1e-9 and np.std(inv32) > 1e-9:
                rtxt = f"{np.corrcoef(self_, inv32)[0,1]:+.3f}"
            print(f"{m+' '+lbl:>14}{ev:>9.3f}{od:>9.3f}{od-ev:>+10.3f}{rtxt:>20}")

def selfevict_row(tree):
    """Per-NoC mean self-eviction fraction M_self/nodeCount (and the M_cold sanity
    fraction), averaged over clusters and samples. Returns {noc: (self_frac, cold_frac, n)}."""
    out = {}
    for noc in NOC_VALUES:
        noc_dir = DATA / tree / f"NoC{noc:02d}"
        csvs = select_csvs(noc_dir)
        if not csvs:
            out[noc] = None
            continue
        self_fracs, cold_fracs = [], []
        for c in csvs:
            df = pd.read_csv(c)
            node = df["nodeCount"].to_numpy(dtype=float)
            node = np.where(node == 0, np.nan, node)
            self_fracs.append(np.nanmean(df["M_self"].to_numpy(dtype=float) / node))
            cold_fracs.append(np.nanmean(df["M_cold"].to_numpy(dtype=float) / node))
        out[noc] = (float(np.mean(self_fracs)), float(np.mean(cold_fracs)), len(csvs))
    return out

def report_selfevict(refcov):
    """Overlay the direct PMU self-eviction fraction against (1 - coverage) per NoC.
    The theory (FINDINGS sec 2) predicts M_self/node tracks 1 - coverage across NoC."""
    se = selfevict_row(SELFEVICT_TREE)
    if all(v is None for v in se.values()):
        print("\n" + "=" * 84)
        print(f"SELF-EVICTION (PMU): no data under data/coverage/{SELFEVICT_TREE}/ "
              "-- run ./run_selfevict.sh first")
        print("=" * 84)
        return
    covrow = refcov.get(BASELINE, {})
    print("\n" + "=" * 84)
    print("SELF-EVICTION (PMU, MEM_LOAD_RETIRED.L3_MISS)  vs  1 - coverage")
    print(f"  M_cold/node ~= 1.0 = counter sanity;  M_self/node = self-evicted fraction "
          f"(baseline coverage from {BASELINE})")
    print("=" * 84)
    print(f"{'NoC':>5}{'n':>4}{'M_cold/node':>13}{'M_self/node':>13}"
          f"{'1-coverage':>12}{'|diff|':>9}")
    xs, ys = [], []
    for noc in NOC_VALUES:
        v = se[noc]
        if v is None:
            print(f"{noc:>5}{'--':>4}{'--':>13}{'--':>13}{'--':>12}{'--':>9}")
            continue
        self_frac, cold_frac, n = v
        cov = covrow.get(noc)
        inv = None if cov is None else 1.0 - cov
        diff = None if inv is None else abs(self_frac - inv)
        if inv is not None:
            xs.append(inv); ys.append(self_frac)
        print(f"{noc:>5}{n:>4}{cold_frac:>13.3f}{self_frac:>13.3f}"
              f"{('--' if inv is None else f'{inv:.3f}'):>12}"
              f"{('--' if diff is None else f'{diff:.3f}'):>9}")
    if len(xs) >= 2:
        r = float(np.corrcoef(xs, ys)[0, 1])
        print(f"\n  Pearson r( 1-coverage , M_self/node ) = {r:+.3f}  "
              "(theory predicts a strong positive correlation)")

# ---- in-condition (prefetch-OFF) coverage vs self-eviction ----
# DECISIVE test (handoff sec 6.1): with the L2 adjacent-line prefetcher OFF (MSR 0x1a4=0x2), BOTH
# coverage (native_jsmap_shuffled_p1a1_same_pref0x2) and M_self (selfevict_shuffled_pref0x2) are
# measured in the SAME condition -- so their link is no longer cross-condition (removes the caveat
# on the earlier +0.94). The pref-OFF coverage tree starts at NoC=8 (lowest present); the point is
# the TREND across NoC, and the per-cluster in-condition correlation.
INCOND_SE_TREE = "selfevict_shuffled_pref0x2"

def report_incondition(refcov):
    if not (DATA / BASELINE_PREF_OFF).is_dir():
        return
    covON = refcov.get(BASELINE, {})
    print("\n" + "=" * 84)
    print("IN-CONDITION (L2 adjacent-line prefetcher OFF, 0x2):  coverage  vs  self-eviction")
    print(f"  coverage: {BASELINE_PREF_OFF}")
    print(f"  self-evict: {INCOND_SE_TREE}   (BOTH measured prefetch-OFF -> same-condition)")
    print("  cov_ON = prefetch-ON coverage (reference);  r = per-cluster corr(M_self_off, 1-cov_off)")
    print("=" * 84)
    print(f"{'NoC':>5}{'n':>4}{'cov_ON':>9}{'cov_OFF':>9}{'d(OFF-ON)':>11}"
          f"{'M_self_off':>12}{'r percluster':>14}")
    xs, ys = [], []
    for noc in NOC_VALUES:
        rr = process_tree_noc(BASELINE_PREF_OFF, noc)
        if rr is None:
            print(f"{noc:>5}{'--':>4}{'--':>9}{'--':>9}{'--':>11}{'--':>12}{'--':>14}")
            continue
        cov_off = rr["coverage"]
        inv_pc = 1.0 - rr["per_cluster_diag"] / ASSOC          # per-cluster (1 - coverage)
        _, self_off = selfevict_percluster(INCOND_SE_TREE, noc)  # per-cluster M_self/node
        r_pc, self_mean = "--", None
        if self_off is not None and len(self_off) == len(inv_pc):
            self_mean = float(np.nanmean(self_off))
            if np.nanstd(self_off) > 1e-9 and np.nanstd(inv_pc) > 1e-9:
                r_pc = f"{np.corrcoef(self_off, inv_pc)[0, 1]:+.3f}"
        cov_on = covON.get(noc)
        con = "--" if cov_on is None else f"{cov_on:.3f}"
        d = "--" if cov_on is None else f"{cov_off - cov_on:+.3f}"
        sm = "--" if self_mean is None else f"{self_mean:.3f}"
        print(f"{noc:>5}{rr['n_samples']:>4}{con:>9}{cov_off:>9.3f}{d:>11}{sm:>12}{r_pc:>14}")
        if self_mean is not None:
            xs.append(1.0 - cov_off); ys.append(self_mean)
    if len(xs) >= 2:
        r = float(np.corrcoef(xs, ys)[0, 1])
        print(f"\n  Pearson r( 1-cov_OFF , M_self_off/node ) across NoC = {r:+.3f}   (IN-CONDITION)")

# ---- native coverage vs prefetcher mask (real e-sets, guaranteed 12/set) ----
# The mask sweep on real (guaranteed-12/set) e-sets shows what each prefetcher was doing to coverage:
#   0x0 = all ON | 0x2 = L2 adjacent-line OFF | 0xf = ALL four prefetchers OFF.
# The scattered NoC-drop that survives 0x2 but VANISHES at 0xf is caused by a prefetcher OTHER than
# adjacent-line (streamer / L1-DCU / L1-DCU-IP), NOT by replacement policy: with every prefetcher off,
# guaranteed-12 scattered e-sets evict nearly as well as contiguous at every NoC. The lazy-victim gap
# that remains at a fixed mask is pure MEMBERSHIP (statistical filling -> under-filled sets).
NATIVE_PREF_MASKS = [                                  # (label, contiguous_tree, scattered_tree, lazy_tree|None)
    ("0x0 all ON", "native",            "native_shuffled",    BASELINE),
    ("0x2 adj OFF", NATIVE_CONT_OFF,     NATIVE_SHUF_OFF,      BASELINE_PREF_OFF),
    ("0xf all OFF", NATIVE_CONT_ALLOFF,  NATIVE_SHUF_ALLOFF,   None),  # lazy 0xf not collected yet
]

def report_native_prefetch_sweep(refcov):
    if not (DATA / NATIVE_SHUF_ALLOFF).is_dir():
        return
    row = lambda tree: refcov.get(tree, {})

    def line(label, tree):
        r = row(tree)
        cells = "".join((f"{r[n]:>8.3f}" if r.get(n) is not None else f"{'--':>8}") for n in NOC_VALUES)
        print(f"{label:<20}{cells}")

    print("\n" + "=" * 84)
    print("NATIVE COVERAGE vs PREFETCHER MASK  (real e-sets, guaranteed 12/set)")
    print("  0x0=all ON | 0x2=adj-line OFF | 0xf=ALL prefetchers OFF")
    print("=" * 84)
    print(f"{'mask / victim':<20}" + "".join(f"{'NoC'+str(n):>8}" for n in NOC_VALUES))
    for mask, cont, shuf, _lazy in NATIVE_PREF_MASKS:
        line(f"{mask}  contig", cont)
        line(f"{mask}  scatter", shuf)

    print("\n  SCATTER PENALTY (contiguous - scattered) per mask  [~0 => scatter is free; large => a prefetcher hurts]")
    print(f"{'mask':<20}" + "".join(f"{'NoC'+str(n):>8}" for n in NOC_VALUES))
    for mask, cont, shuf, _lazy in NATIVE_PREF_MASKS:
        rc, rs = row(cont), row(shuf)
        cells = "".join((f"{rc[n]-rs[n]:>+8.3f}" if (rc.get(n) is not None and rs.get(n) is not None)
                         else f"{'--':>8}") for n in NOC_VALUES)
        print(f"{mask:<20}{cells}")

    print("\n  MEMBERSHIP GAP (scattered real e-set - lazy victim, SAME mask)  [under-filled sets in the lazy victim]")
    print(f"{'mask':<20}" + "".join(f"{'NoC'+str(n):>8}" for n in NOC_VALUES))
    for mask, _cont, shuf, lazy in NATIVE_PREF_MASKS:
        if lazy is None:
            print(f"{mask:<20}" + "".join(f"{'--':>8}" for _ in NOC_VALUES) + "   (lazy 0xf not collected)")
            continue
        rs, rl = row(shuf), row(lazy)
        cells = "".join((f"{rs[n]-rl[n]:>+8.3f}" if (rs.get(n) is not None and rl.get(n) is not None)
                         else f"{'--':>8}") for n in NOC_VALUES)
        print(f"{mask:<20}{cells}")

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

def report_ev_grid(trees, base64, ceil_cont, ceil_shuf, title, rank_noc=64):
    """Rank Rowhammer.js eviction-strategy trees by coverage at rank_noc and print the grid.
    trees: (name, A, D, C, pref, mb) tuples. base64 may be None (-> blank delta column).
    Returns the ranked rows [(A, D, C, cov_row, name), ...] for the ways-histogram section."""
    print("\n" + "=" * 84)
    print(title)
    print(f"references @NoC64:  native(contiguous)={fmt_cov(ceil_cont)}  "
          f"native_shuffled(scattered,12/set)={fmt_cov(ceil_shuf)}  baseline={fmt_cov(base64)}")
    print("=" * 84)
    rows = [(A, D, C, cov_row(name), name) for name, A, D, C, _pref, _mb in trees]
    rows.sort(key=lambda t: (t[3][rank_noc] is not None,
                             t[3][rank_noc] if t[3][rank_noc] is not None else -1), reverse=True)
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
    if not records:
        print("  (no matching trees found on disk)")
        return rows
    grid_df = pd.DataFrame(records)
    cov_fmt = lambda x: "--" if pd.isna(x) else f"{x:.3f}"
    fmts = {f"NoC{n}": cov_fmt for n in NOC_VALUES}
    fmts["d64_vs_base"] = lambda x: "" if pd.isna(x) else f"{x:+.3f}"
    print(grid_df.to_string(index=False, formatters=fmts, justify="right"))
    return rows

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

    # ---- 2. Eviction-strategy grid (prefetch-ON, 12MB), ranked by NoC=64 coverage ----
    ev_all = list_ev_trees()
    ev_on = [t for t in ev_all if t[4] == "0x0" and t[5] == 12]
    rows = report_ev_grid(ev_on, base64, ceil_cont, ceil_shuf,
                          f"EVICTION-STRATEGY GRID (base={BASELINE}, prefetch-ON, 12MB), ranked by NoC=64 coverage",
                          rank_noc=64)

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

    # ---- 4. Direct PMU self-eviction fraction vs 1 - coverage ----
    report_selfevict(refcov)

    # ---- 5. Prefetcher sweep: M_cold / M_self across 0x1a4 masks + NoC=32 attribution ----
    report_prefetch_sweep(refcov)

    # ---- 6. In-condition (prefetch-OFF) coverage vs self-eviction -- the decisive test ----
    report_incondition(refcov)

    # ---- 7. Native coverage vs prefetcher mask (scatter penalty + membership gap) ----
    report_native_prefetch_sweep(refcov)

    # ---- 8. Rowhammer.js eviction-strategy sweep with ALL prefetchers OFF (0xf, 12MB) ----
    # Does the sliding-window sweep lift the 0xf low-NoC crater? Ranked by NoC=8 (where the
    # plain-sweep coverage is worst). No plain 1-pass 0xf 12MB baseline was collected, so the
    # delta column is blank; the reachable ceiling is native_shuffled_pref0xf (guaranteed 12/set).
    ev_0xf = [t for t in ev_all if t[4] == "0xf" and t[5] == 12]
    report_ev_grid(ev_0xf, None,
                   refcov.get(NATIVE_CONT_ALLOFF, {}).get(64),
                   refcov.get(NATIVE_SHUF_ALLOFF, {}).get(64),
                   "ROWHAMMER.JS EVICTION-STRATEGY SWEEP @ ALL PREFETCHERS OFF (0xf, 12MB), ranked by NoC=8",
                   rank_noc=8)

if __name__ == "__main__":
    main()
