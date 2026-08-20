#!/bin/bash
#
# finalize_experiment.sh — after a full run_all_configs.sh sweep, pack this experiment's memorygram
# CSVs into ONE per-experiment .h5, delete the CSVs that fed it, and rsync the .h5 to the remote
# archive over a persistent SSH connection. Keeps the local .h5 copy.
#
# Invoked as root (parent runs under `sudo ./run_all_configs.sh`). Steps that must use the login
# user's credentials/keys (the h5 build and all ssh/rsync) are dropped to $SUDO_USER via `as_user`;
# only the `rm` of the root-owned CSVs stays as root.
#
# Usage:
#   finalize_experiment.sh <TIMER_MODE> <SHUFFLE_FLAG> <TST> <CPA> <JSMAP_BUF_MB> <NoC...>
# Config via environment (exported by the caller; defaults below):
#   REMOTE_HOST REMOTE_USER REMOTE_DIR PYTHON_BIN LOCAL_H5_DIR DRY_RUN
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
if [ "$#" -lt 6 ]; then
    echo "usage: $0 <TIMER_MODE> <SHUFFLE_FLAG> <TST> <CPA> <JSMAP_BUF_MB> <NoC...>" >&2
    exit 2
fi
TIMER_MODE="$1"; SHUFFLE_FLAG="$2"; TST="$3"; CPA="$4"; JSMAP_BUF_MB="$5"; shift 5
NOCS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Config (env-overridable)
# ---------------------------------------------------------------------------
REMOTE_HOST="${REMOTE_HOST:-132.72.67.152}"
REMOTE_USER="${REMOTE_USER:-michael}"
REMOTE_DIR="${REMOTE_DIR:-/home/michael/michaels_backup_data}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
LOCAL_H5_DIR="${LOCAL_H5_DIR:-$SCRIPT_DIR/h5}"
DRY_RUN="${DRY_RUN:-0}"

REMOTE="${REMOTE_USER:+$REMOTE_USER@}$REMOTE_HOST"

# ---------------------------------------------------------------------------
# Drop-to-user helper. Under sudo, run the login user's shell env (-H sets HOME so ssh finds
# ~/.ssh); when already running as the user (e.g. manual DRY_RUN), run directly.
# ---------------------------------------------------------------------------
LOGIN_USER="${SUDO_USER:-$(id -un)}"
USER_HOME="$(getent passwd "$LOGIN_USER" | cut -d: -f6)"
[ -n "$USER_HOME" ] || USER_HOME="$HOME"

as_user() {
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        sudo -H -u "$SUDO_USER" "$@"
    else
        "$@"
    fi
}

# ---------------------------------------------------------------------------
# 1. Resolve the on-disk clock_subdir (mirror timer_mode_subdir() in mastikElite.c EXACTLY:
#    base per mode, + "_<N>MB" iff JSMAP_BUF_MB != 12; chrome_clock_shuffled for -c -s).
# ---------------------------------------------------------------------------
case "$TIMER_MODE" in
    -c)   CLOCK_SUBDIR="chrome_clock" ;;
    -j)   CLOCK_SUBDIR="chrome_clock_jsmap" ;;
    -jn)  CLOCK_SUBDIR="native_clock_jsmap" ;;
    -jb)  CLOCK_SUBDIR="chrome_clock_jsmap_bidir" ;;
    -jnb) CLOCK_SUBDIR="native_clock_jsmap_bidir" ;;
    -jss)  CLOCK_SUBDIR="chrome_clock_jsmapSS" ;;
    -jssb) CLOCK_SUBDIR="chrome_clock_jsmapSS_bidir" ;;
    *)    CLOCK_SUBDIR="native_clock" ;;
esac
if [ "$TIMER_MODE" = "-c" ] && [ "$SHUFFLE_FLAG" = "-s" ]; then
    CLOCK_SUBDIR="chrome_clock_shuffled"
fi
case "$TIMER_MODE" in
    -j|-jn|-jb|-jnb|-jss|-jssb)
        if [ "$JSMAP_BUF_MB" != "12" ]; then CLOCK_SUBDIR="${CLOCK_SUBDIR}_${JSMAP_BUF_MB}MB"; fi ;;
esac
DATA_ROOT="$SCRIPT_DIR/data/$CLOCK_SUBDIR"

# Config dir names for this sweep. "90K" is the literal used by run_all_configs.sh.
CONFIG_DIRS=()
for noc in "${NOCS[@]}"; do
    CONFIG_DIRS+=("${noc}C_${TST}TST_90K_${CPA}cycles")
done
CONFIGS_CSV="$(IFS=,; echo "${CONFIG_DIRS[*]}")"

# ---------------------------------------------------------------------------
# 2. Friendly clock label for the h5 filename (distinct from the on-disk tree name).
# ---------------------------------------------------------------------------
case "$TIMER_MODE" in
    -c)   CLOCK_LABEL="chrome" ;;
    -j)   CLOCK_LABEL="chromeJSmap" ;;
    -jn)  CLOCK_LABEL="nativeJSmap" ;;
    -jb)  CLOCK_LABEL="chromeJSmapBidir" ;;
    -jnb) CLOCK_LABEL="nativeJSmapBidir" ;;
    -jss)  CLOCK_LABEL="chromeJSmapSS" ;;
    -jssb) CLOCK_LABEL="chromeJSmapSSBidir" ;;
    *)    CLOCK_LABEL="native" ;;
esac
if [ "$TIMER_MODE" = "-c" ] && [ "$SHUFFLE_FLAG" = "-s" ]; then
    CLOCK_LABEL="chromeShuffled"
fi
# Suffix that pins the parameter tuple; the h5 name ALWAYS carries _<BUF>MB (unlike the tree).
NAME_TAIL="_${TST}TST_90K_${CPA}cycles_${JSMAP_BUF_MB}MB.h5"

echo "🧩 finalize_experiment"
echo "   clock_subdir : $CLOCK_SUBDIR"
echo "   data_root    : $DATA_ROOT"
echo "   configs      : $CONFIGS_CSV"
echo "   clock_label  : $CLOCK_LABEL"
echo "   remote       : $REMOTE:$REMOTE_DIR"
echo "   login_user   : $LOGIN_USER (home $USER_HOME)"
echo "   dry_run      : $DRY_RUN"

# Sanity: every config dir must exist before we touch anything.
missing=()
for cfg in "${CONFIG_DIRS[@]}"; do
    [ -d "$DATA_ROOT/$cfg" ] || missing+=("$cfg")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "❌ missing config dirs under $DATA_ROOT: ${missing[*]}" >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# 3. Persistent SSH connection (ControlMaster), reused by the counter scan and rsync.
# ---------------------------------------------------------------------------
CM_PATH="$USER_HOME/.ssh/cm-fp-%r@%h:%p"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$CM_PATH" -o ControlPersist=60 -o ConnectTimeout=15)

close_master() {
    as_user ssh "${SSH_OPTS[@]}" -O exit "$REMOTE" >/dev/null 2>&1 || true
}
trap close_master EXIT

# Open the master (best-effort; scan/rsync still work per-connection if this fails).
as_user ssh "${SSH_OPTS[@]}" -Nf "$REMOTE" 2>/dev/null || \
    echo "⚠️  could not pre-open SSH master to $REMOTE (will connect per-op)"

# ---------------------------------------------------------------------------
# 4. Rerun counter. Highest existing index (bare filename = 1) across LOCAL + REMOTE, +1.
#    new==1 -> bare label; else append the number.
# ---------------------------------------------------------------------------
max_index_from_list() {
    # stdin: filenames; echoes max index matching ^<label>(<digits>?)<NAME_TAIL>$ (bare -> 1)
    local pat_pre="$CLOCK_LABEL" pat_post="$NAME_TAIL" max=0
    local re="^${pat_pre}([0-9]*)$(printf '%s' "$pat_post" | sed 's/[.[\*^$]/\\&/g')\$"
    while IFS= read -r f; do
        f="$(basename "$f")"
        if [[ "$f" =~ $re ]]; then
            local idx="${BASH_REMATCH[1]}"
            [ -z "$idx" ] && idx=1
            (( idx > max )) && max=$idx
        fi
    done
    echo "$max"
}

mkdir -p "$LOCAL_H5_DIR"
chown "$LOGIN_USER" "$LOCAL_H5_DIR" 2>/dev/null || true

local_list="$(ls -1 "$LOCAL_H5_DIR" 2>/dev/null || true)"
remote_list="$(as_user ssh "${SSH_OPTS[@]}" "$REMOTE" "ls -1 '$REMOTE_DIR' 2>/dev/null" 2>/dev/null || true)"

max_local="$(printf '%s\n' "$local_list"  | max_index_from_list)"
max_remote="$(printf '%s\n' "$remote_list" | max_index_from_list)"
max_idx=$(( max_local > max_remote ? max_local : max_remote ))
new_idx=$(( max_idx + 1 ))

if [ "$new_idx" -eq 1 ]; then
    H5_NAME="${CLOCK_LABEL}${NAME_TAIL}"
else
    H5_NAME="${CLOCK_LABEL}${new_idx}${NAME_TAIL}"
fi
H5_PATH="$LOCAL_H5_DIR/$H5_NAME"
echo "   h5 name      : $H5_NAME  (index $new_idx; local max=$max_local remote max=$max_remote)"

# ---------------------------------------------------------------------------
# DRY RUN: report the resolved plan and stop before any write/delete/transfer.
# ---------------------------------------------------------------------------
if [ "$DRY_RUN" != "0" ]; then
    echo "🔎 DRY_RUN: would build '$H5_PATH' from $DATA_ROOT"
    echo "🔎 DRY_RUN: would rm -rf:"
    for cfg in "${CONFIG_DIRS[@]}"; do echo "     $DATA_ROOT/$cfg"; done
    echo "🔎 DRY_RUN: would rsync -> $REMOTE:$REMOTE_DIR/"
    exit 0
fi

# ---------------------------------------------------------------------------
# 5. Build the h5 as the login user (user-owned output; reads root-owned-but-644 CSVs fine).
# ---------------------------------------------------------------------------
echo "🏗  building $H5_PATH ..."
if ! as_user "$PYTHON_BIN" "$SCRIPT_DIR/python/csv_to_h5.py" \
        --data-root "$DATA_ROOT" --configs "$CONFIGS_CSV" --out "$H5_PATH"; then
    echo "❌ conversion failed — CSVs left intact, nothing deleted." >&2
    exit 4
fi

# ---------------------------------------------------------------------------
# 6. Delete the CSVs that fed the h5 (root; only after conversion+verify passed). Only the exact
#    config dirs are removed — the shared clock_subdir may hold other experiments.
# ---------------------------------------------------------------------------
echo "🗑  deleting source CSV config dirs ..."
for cfg in "${CONFIG_DIRS[@]}"; do
    rm -rf "$DATA_ROOT/$cfg"
    echo "     removed $DATA_ROOT/$cfg"
done

# ---------------------------------------------------------------------------
# 7. Backup to the remote archive (append-only; no --delete). Local copy is kept.
# ---------------------------------------------------------------------------
echo "☁️  rsync -> $REMOTE:$REMOTE_DIR/ ..."
as_user ssh "${SSH_OPTS[@]}" "$REMOTE" "mkdir -p '$REMOTE_DIR'"
as_user rsync -avz -e "ssh ${SSH_OPTS[*]}" "$H5_PATH" "$REMOTE:$REMOTE_DIR/"

echo "✅ finalize complete: $H5_NAME (local kept at $H5_PATH, backed up to $REMOTE:$REMOTE_DIR/)"
