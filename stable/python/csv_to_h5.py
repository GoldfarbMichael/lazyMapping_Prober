#!/usr/bin/env python3
"""
csv_to_h5.py — pack one experiment's memorygram CSVs into a single self-contained HDF5.

One experiment = one (clock, TST, cpa, JSMAP_BUF_MB, shuffle) tuple = one .h5 file whose inner
groups are the NoCs. Each NoC group is written ONCE from fully-materialized CSVs, so there is no
append and no dataset resizing (the structural win over the old global-append workflow).

On-disk CSV tree (see mastikElite.c:1142):
    <data_root>/<NoC>C_<TST>TST_90K_<cpa>cycles/<stressor>/<n>.csv
CSV shape: header "G0..G{NoC-1}", then rows of uint. rows = T (time slots), cols = NoC (G).
Stored X keeps the project convention (samples, G, T) via a per-sample transpose (T,G) -> (G,T).

Exit code is the delete gate: the bash caller deletes CSVs ONLY on exit 0. Any missing config,
shape mismatch, or verification failure returns nonzero and writes no partial-but-blessed file.

Usage:
    csv_to_h5.py --data-root data/<clock_subdir> \
                 --configs "1C_4TST_90K_2288cycles,2C_4TST_90K_2288cycles,..." \
                 --out /path/chromeJSmap_4TST_90K_2288cycles_12MB.h5
"""
import argparse
import json
import re
import sys
from pathlib import Path

import h5py
import numpy as np
import pandas as pd

NOC_RE = re.compile(r"^(\d+)C_")


def parse_noc(config_name: str) -> int:
    m = NOC_RE.match(config_name)
    if not m:
        raise ValueError(f"cannot parse NoC from config dir name '{config_name}' (expected '<N>C_...')")
    return int(m.group(1))


def load_csv(path: Path) -> np.ndarray:
    """Read one memorygram CSV -> int32 array shaped (T, G) (rows=time, cols=clusters)."""
    df = pd.read_csv(path, header=0).fillna(0)
    return df.values.astype(np.int32)


def collect_config(data_root: Path, config: str):
    """Return (noc, label_names_sorted, samples) for one config dir.

    samples = list of (label_name, T, G, matrix_TxG). Raises on empty/malformed input.
    """
    cfg_dir = data_root / config
    if not cfg_dir.is_dir():
        raise FileNotFoundError(f"config dir missing: {cfg_dir}")
    noc = parse_noc(config)

    stressor_dirs = sorted(p for p in cfg_dir.iterdir() if p.is_dir())
    if not stressor_dirs:
        raise FileNotFoundError(f"no stressor subdirs under {cfg_dir}")

    samples = []
    ref_shape = None  # (T, G) must be identical across every CSV in this NoC group
    bad = []
    for sdir in stressor_dirs:
        csvs = sorted(sdir.glob("*.csv"))
        if not csvs:
            raise FileNotFoundError(f"no CSVs under {sdir}")
        for csv in csvs:
            mat = load_csv(csv)  # (T, G)
            if ref_shape is None:
                ref_shape = mat.shape
            elif mat.shape != ref_shape:
                bad.append(f"{csv}: shape {mat.shape} != group shape {ref_shape}")
                continue
            samples.append((sdir.name, mat))
    if bad:
        raise ValueError("CSV shape mismatch(es) in %s:\n  %s" % (config, "\n  ".join(bad)))

    labels_sorted = sorted({s[0] for s in samples})
    return noc, labels_sorted, samples, ref_shape  # ref_shape = (T, G)


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack experiment CSVs into one per-experiment HDF5.")
    ap.add_argument("--data-root", required=True, help="data/<clock_subdir> (holds the <NoC>C_... dirs)")
    ap.add_argument("--configs", required=True, help="comma-separated config dir names")
    ap.add_argument("--out", required=True, help="output .h5 path")
    args = ap.parse_args()

    data_root = Path(args.data_root)
    configs = [c.strip() for c in args.configs.split(",") if c.strip()]
    out = Path(args.out)

    if not configs:
        print("[csv_to_h5] ERROR: no configs given", file=sys.stderr)
        return 2
    if not data_root.is_dir():
        print(f"[csv_to_h5] ERROR: data-root not found: {data_root}", file=sys.stderr)
        return 2

    # 1. Gather every config first (fail before writing anything if input is incomplete).
    gathered = {}          # noc -> (labels_sorted, samples, (T,G))
    all_labels = set()
    expected_counts = {}   # noc -> num samples
    for cfg in configs:
        try:
            noc, labels, samples, tg = collect_config(data_root, cfg)
        except (FileNotFoundError, ValueError) as e:
            print(f"[csv_to_h5] ERROR: {e}", file=sys.stderr)
            return 3
        if noc in gathered:
            print(f"[csv_to_h5] ERROR: duplicate NoC {noc} (config '{cfg}')", file=sys.stderr)
            return 3
        gathered[noc] = (labels, samples, tg)
        all_labels.update(labels)
        expected_counts[noc] = len(samples)

    # 2. Per-file label map (sorted union of stressor names) -> stable int ids.
    label_to_int = {name: i for i, name in enumerate(sorted(all_labels))}

    # 3. Write. Each NoC group written once; no maxshape, no resize.
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    try:
        with h5py.File(tmp, "w") as h5:
            h5.attrs["label_map"] = json.dumps(label_to_int)
            for noc in sorted(gathered):
                labels, samples, (T, G) = gathered[noc]
                n = len(samples)
                X = np.empty((n, G, T), dtype=np.int32)
                y = np.empty((n,), dtype=np.int32)
                for i, (label_name, mat_TxG) in enumerate(samples):
                    X[i] = mat_TxG.T                       # (T,G) -> (G,T): project X convention
                    y[i] = label_to_int[label_name]
                grp = h5.create_group(str(noc))
                grp.create_dataset("X", data=X, dtype="int32",
                                   chunks=(1, G, T), compression="gzip", compression_opts=4)
                grp.create_dataset("y", data=y, dtype="int32")
                grp.attrs["NoC"] = noc
                grp.attrs["G"] = G
                grp.attrs["time_steps"] = T
                grp.attrs["num_samples"] = n
                print(f"[csv_to_h5]   NoC={noc}: {n} samples, X=({n},{G},{T})")
    except Exception as e:  # noqa: BLE001 — any write failure must abort the delete gate
        print(f"[csv_to_h5] ERROR while writing {tmp}: {e}", file=sys.stderr)
        tmp.unlink(missing_ok=True)
        return 4

    # 4. Verification gate: reopen and confirm every config produced a group with the right count.
    try:
        with h5py.File(tmp, "r") as h5:
            for noc, want in expected_counts.items():
                key = str(noc)
                if key not in h5:
                    raise AssertionError(f"group {key} missing after write")
                got = h5[key]["X"].shape[0]
                if got != want:
                    raise AssertionError(f"NoC={noc}: X has {got} samples, expected {want}")
    except (AssertionError, OSError, KeyError) as e:
        print(f"[csv_to_h5] VERIFY FAILED: {e}", file=sys.stderr)
        tmp.unlink(missing_ok=True)
        return 5

    tmp.replace(out)  # atomic promote only after verification passes
    total = sum(expected_counts.values())
    print(f"[csv_to_h5] OK: {out} ({len(gathered)} NoC groups, {total} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
