#!/bin/bash
#
# finalize_experiment.sh — NATIVE (Stage 1/2) front-end to finalize_lib.sh.
#
# After a full run_all_configs.sh sweep, pack this experiment's memorygram CSVs into ONE
# per-experiment .h5, delete the CSVs that fed it, and rsync the .h5 to the remote archive over a
# persistent SSH connection. Keeps the local .h5 copy.
#
# This script owns ONLY the native naming rules: timer mode -> on-disk clock_subdir, timer mode ->
# friendly h5 label, and the "<NoC>C_<TST>TST_90K_<cpa>cycles" config dir names. Everything else
# (rerun counter, ssh multiplexing, h5 build, delete, rsync) lives in finalize_lib.sh, shared with
# the Stage 3 front-end finalize_realbrowser.sh.
#
# Invoked as root (parent runs under `sudo ./run_all_configs.sh`). Steps that must use the login
# user's credentials/keys (the h5 build and all ssh/rsync) are dropped to $SUDO_USER via `as_user`;
# only the `rm` of the root-owned CSVs stays as root.
#
# Usage:
#   finalize_experiment.sh <TIMER_MODE> <SHUFFLE_FLAG> <TST> <CPA> <JSMAP_BUF_MB> <K> <NoC...>
# Config via environment (exported by the caller; defaults in finalize_lib.sh):
#   REMOTE_HOST REMOTE_USER REMOTE_DIR PYTHON_BIN LOCAL_H5_DIR DRY_RUN
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
if [ "$#" -lt 7 ]; then
    echo "usage: $0 <TIMER_MODE> <SHUFFLE_FLAG> <TST> <CPA> <JSMAP_BUF_MB> <K> <NoC...>" >&2
    exit 2
fi
TIMER_MODE="$1"; SHUFFLE_FLAG="$2"; TST="$3"; CPA="$4"; JSMAP_BUF_MB="$5"; K="$6"; shift 6
NOCS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# 1. Resolve the on-disk clock_subdir (mirror timer_mode_subdir() in mastikElite.c EXACTLY:
#    base per mode, + "_<N>MB" iff JSMAP_BUF_MB != 12; chrome_clock_shuffled for -c -s).
# ---------------------------------------------------------------------------
# shellcheck source=clock_labels.sh
source "$SCRIPT_DIR/clock_labels.sh"
CLOCK_SUBDIR="$(clock_subdir_for "$TIMER_MODE" "$SHUFFLE_FLAG" "$JSMAP_BUF_MB")"
DATA_ROOT="$SCRIPT_DIR/data/$CLOCK_SUBDIR"

# Config dir names for this sweep. K comes from the caller now (it used to be the literal 90),
# so a dynamic-K sweep (K=0) finalizes its own tree instead of looking for a 90K one.
CONFIG_DIRS=()
for noc in "${NOCS[@]}"; do
    CONFIG_DIRS+=("${noc}C_${TST}TST_${K}K_${CPA}cycles")
done

# ---------------------------------------------------------------------------
# 2. Friendly clock label for the h5 filename (distinct from the on-disk tree name).
# ---------------------------------------------------------------------------
CLOCK_LABEL="$(clock_label_for "$TIMER_MODE" "$SHUFFLE_FLAG")"

# Suffix that pins the parameter tuple; the h5 name ALWAYS carries _<BUF>MB (unlike the tree).
NAME_TAIL="_${TST}TST_${K}K_${CPA}cycles_${JSMAP_BUF_MB}MB.h5"

# ---------------------------------------------------------------------------
# 3. Hand off to the shared machinery.
# ---------------------------------------------------------------------------
# shellcheck source=finalize_lib.sh
source "$SCRIPT_DIR/finalize_lib.sh"

echo "🧩 finalize_experiment"
echo "   clock_subdir : $CLOCK_SUBDIR"
finalize_run
