"""Summary tables: coverage, cleansing and spillover for a chosen set of victim trees.

Aggregation over samples:
  coverage, cleansing -> MIN  (higher-is-better; worst-case sample, established convention)
  spillover           -> MEAN (explicit project decision; the max is very noisy at n=2)

Emits markdown-ready tables. Definitions: docs/journal/Metrics_Definitions.md
"""
from pathlib import Path

import numpy as np
import pandas as pd

STABLE = Path(__file__).resolve().parent.parent
DATA = STABLE / "data" / "coverage"

ASSOC = 12
PAGE_OFFSET_BITS = 12
BASELINE_ROWS = 15
NOC_ALL = [2, 4, 8, 16, 32, 64]

# (display label, tree directory)
TREES = [
    ("native (0x0)", "native"),
    ("native_pref0xf", "native_pref0xf"),
    ("bidirR1 0x0", "native_jsmap_shuffled_p1a1_bidirR1_pref0x0"),
    ("bidirR1 0xf", "native_jsmap_shuffled_p1a1_bidirR1_pref0xf"),
    ("bidirR4 0x0", "native_jsmap_shuffled_p1a1_bidirR4_pref0x0"),
    ("bidirR4 0xf", "native_jsmap_shuffled_p1a1_bidirR4_pref0xf"),
    ("p1a1_same (0x0)", "native_jsmap_shuffled_p1a1_same"),
    ("p3a1_pref0xf", "native_jsmap_shuffled_p3a1_pref0xf"),
    ("p3a1_same (0x0)", "native_jsmap_shuffled_p3a1_same"),
]

_pc_cache, _m_cache = {}, {}


def phys_clusters(tree, noc):
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


def offdiag_row_mean(M):
    noc = M.shape[0]
    return np.zeros(noc) if noc < 2 else (M.sum(axis=1) - np.diag(M)) / (noc - 1)


def metrics(tree, noc):
    key = (tree, noc)
    if key in _m_cache:
        return _m_cache[key]
    d = DATA / tree / f"NoC{noc:02d}"
    paths = sorted(d.glob("*.csv")) if d.is_dir() else []
    pc = phys_clusters(tree, noc)
    if not paths or pc is None:
        _m_cache[key] = None
        return None
    A, C, s_raw, s_sub = [], [], [], []
    for p in paths:
        data = pd.read_csv(p).to_numpy(dtype=float)
        if data.shape[0] != noc + BASELINE_ROWS:
            raise ValueError(f"{p}: {data.shape[0]} rows, expected {noc + BASELINE_ROWS}")
        rows, idle = data[:noc], data[noc:].mean(axis=0)
        full = rows >= ASSOC
        a = np.zeros((noc, noc)); c = np.zeros((noc, noc)); b = np.zeros(noc)
        for g in range(noc):
            mask = pc == g
            if mask.any():
                a[:, g] = rows[:, mask].mean(axis=1)
                c[:, g] = full[:, mask].mean(axis=1)
                b[g] = idle[mask].mean()
        sub = np.clip(a - b[None, :], 0.0, None)
        A.append(a); C.append(c)
        s_raw.append(offdiag_row_mean(a) / ASSOC)
        s_sub.append(offdiag_row_mean(sub) / ASSOC)
    A, C = np.array(A), np.array(C)
    s_raw, s_sub = np.array(s_raw), np.array(s_sub)
    out = {
        "n": len(paths),
        "coverage": float(np.diag(A.min(axis=0)).mean() / ASSOC),   # MIN over samples
        "cleansing": float(np.diag(C.min(axis=0)).mean()),          # MIN over samples
        "spill_sub": float(s_sub.mean(axis=0).mean()),              # MEAN over samples
        "spill_raw": float(s_raw.mean(axis=0).mean()),              # MEAN over samples
    }
    _m_cache[key] = out
    return out


def wide(metric, nd=3):
    """rows = tree, cols = NoC."""
    recs = []
    for label, tree in TREES:
        rec = {"victim": label}
        for noc in NOC_ALL:
            m = metrics(tree, noc)
            rec[f"NoC{noc}"] = "--" if m is None else f"{m[metric]:.{nd}f}"
        recs.append(rec)
    return pd.DataFrame(recs)


def long_form():
    recs = []
    for label, tree in TREES:
        for noc in NOC_ALL:
            m = metrics(tree, noc)
            if m is None:
                continue
            recs.append({"victim": label, "noc": noc, "n": m["n"],
                         "coverage": round(m["coverage"], 4),
                         "cleansing": round(m["cleansing"], 4),
                         "spillover_sub": round(m["spill_sub"], 4),
                         "spillover_raw": round(m["spill_raw"], 4)})
    return pd.DataFrame(recs)


def to_md(df):
    """Minimal markdown table writer (tabulate is not installed)."""
    cols = list(df.columns)
    out = ["| " + " | ".join(cols) + " |",
           "|" + "|".join("---" for _ in cols) + "|"]
    for _, r in df.iterrows():
        out.append("| " + " | ".join(str(r[c]) for c in cols) + " |")
    return "\n".join(out)


def main():
    for metric, nd, title in [("coverage", 3, "COVERAGE (min over samples)"),
                              ("cleansing", 3, "CLEANSING (min over samples)"),
                              ("spill_sub", 4, "SPILLOVER, baseline-subtracted (MEAN over samples)"),
                              ("spill_raw", 4, "SPILLOVER, raw (MEAN over samples)")]:
        print("=" * 100)
        print(title)
        print("=" * 100)
        print(to_md(wide(metric, nd)))
        print()
    print("=" * 100)
    print("LONG FORM")
    print("=" * 100)
    print(to_md(long_form()))


if __name__ == "__main__":
    main()
