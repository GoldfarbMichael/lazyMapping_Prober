#!/bin/bash
#
# finalize_lib.sh — the experiment-agnostic half of the post-sweep finalize step.
# SOURCE this; do not execute it. Front-ends: finalize_experiment.sh (native C sweeps,
# run_all_configs.sh) and finalize_realbrowser.sh (Stage 3 real browser, run_fingerprint_sweep.sh).
#
# A front-end's only job is to translate its own parameters into a data root, a list of config
# dirs, and an h5 name; everything below (rerun counter, ssh multiplexing, h5 build, CSV delete,
# rsync) is shared so the two paths cannot drift apart.
#
# Contract — set these, then call finalize_run:
#   DATA_ROOT    absolute dir holding the config dirs
#   CONFIG_DIRS  bash array of config dir names under DATA_ROOT
#   CLOCK_LABEL  h5 filename stem (rerun index is appended to it: label, label2, label3, ...)
#   NAME_TAIL    h5 filename suffix, including ".h5"
#   SCRIPT_DIR   stable/ (used to locate python/csv_to_h5.py)
# Env config (defaults applied here):
#   REMOTE_HOST REMOTE_USER REMOTE_DIR PYTHON_BIN LOCAL_H5_DIR DRY_RUN
#
# Safety contract, unchanged from the original finalize_experiment.sh: every config dir must exist
# before anything is touched, and the CSVs are deleted ONLY after csv_to_h5.py exits 0 (its own
# reopen-and-verify gate). Any failure leaves the CSVs intact for a retry.

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
# Drop-to-user helper. Under sudo (the native path: run_all_configs.sh runs as root), run the
# login user's shell env (-H sets HOME so ssh finds ~/.ssh); when already running as the user
# (the Stage 3 path, and manual DRY_RUN), run directly.
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
# Rerun counter helper. Highest existing index (bare filename = 1) among a list of filenames.
# ---------------------------------------------------------------------------
max_index_from_list() {
    # stdin: filenames; echoes max index matching ^<CLOCK_LABEL>(<digits>?)<NAME_TAIL>$ (bare -> 1)
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

close_master() {
    as_user ssh "${SSH_OPTS[@]}" -O exit "$REMOTE" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# finalize_run — banner, precheck, h5 build, CSV delete, remote backup.
# ---------------------------------------------------------------------------
finalize_run() {
    local CONFIGS_CSV
    CONFIGS_CSV="$(IFS=,; echo "${CONFIG_DIRS[*]}")"

    echo "   data_root    : $DATA_ROOT"
    echo "   configs      : $CONFIGS_CSV"
    echo "   clock_label  : $CLOCK_LABEL"
    echo "   remote       : $REMOTE:$REMOTE_DIR"
    echo "   login_user   : $LOGIN_USER (home $USER_HOME)"
    echo "   dry_run      : $DRY_RUN"

    # Sanity: every config dir must exist before we touch anything.
    local missing=() cfg
    for cfg in "${CONFIG_DIRS[@]}"; do
        [ -d "$DATA_ROOT/$cfg" ] || missing+=("$cfg")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "❌ missing config dirs under $DATA_ROOT: ${missing[*]}" >&2
        exit 3
    fi

    # -----------------------------------------------------------------------
    # Persistent SSH connection (ControlMaster), reused by the counter scan and rsync.
    # -----------------------------------------------------------------------
    CM_PATH="$USER_HOME/.ssh/cm-fp-%r@%h:%p"
    SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$CM_PATH" -o ControlPersist=60 -o ConnectTimeout=15)
    trap close_master EXIT

    # Open the master (best-effort; scan/rsync still work per-connection if this fails).
    as_user ssh "${SSH_OPTS[@]}" -Nf "$REMOTE" 2>/dev/null || \
        echo "⚠️  could not pre-open SSH master to $REMOTE (will connect per-op)"

    # -----------------------------------------------------------------------
    # Rerun counter. Highest existing index across LOCAL + REMOTE, +1.
    # new==1 -> bare label; else append the number.
    # -----------------------------------------------------------------------
    mkdir -p "$LOCAL_H5_DIR"
    chown "$LOGIN_USER" "$LOCAL_H5_DIR" 2>/dev/null || true

    local local_list remote_list max_local max_remote max_idx new_idx H5_NAME H5_PATH
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

    # -----------------------------------------------------------------------
    # DRY RUN: report the resolved plan and stop before any write/delete/transfer.
    # -----------------------------------------------------------------------
    if [ "$DRY_RUN" != "0" ]; then
        echo "🔎 DRY_RUN: would build '$H5_PATH' from $DATA_ROOT"
        echo "🔎 DRY_RUN: would rm -rf:"
        for cfg in "${CONFIG_DIRS[@]}"; do echo "     $DATA_ROOT/$cfg"; done
        echo "🔎 DRY_RUN: would rsync -> $REMOTE:$REMOTE_DIR/"
        exit 0
    fi

    # -----------------------------------------------------------------------
    # Build the h5 as the login user (user-owned output; reads root-owned-but-644 CSVs fine).
    # -----------------------------------------------------------------------
    echo "🏗  building $H5_PATH ..."
    if ! as_user "$PYTHON_BIN" "$SCRIPT_DIR/python/csv_to_h5.py" \
            --data-root "$DATA_ROOT" --configs "$CONFIGS_CSV" --out "$H5_PATH"; then
        echo "❌ conversion failed — CSVs left intact, nothing deleted." >&2
        exit 4
    fi

    # -----------------------------------------------------------------------
    # Delete the CSVs that fed the h5 (only after conversion+verify passed). Only the exact
    # config dirs are removed — the shared data root may hold other experiments.
    # -----------------------------------------------------------------------
    echo "🗑  deleting source CSV config dirs ..."
    for cfg in "${CONFIG_DIRS[@]}"; do
        rm -rf "$DATA_ROOT/$cfg"
        echo "     removed $DATA_ROOT/$cfg"
    done

    # -----------------------------------------------------------------------
    # Backup to the remote archive (append-only; no --delete). Local copy is kept.
    # -----------------------------------------------------------------------
    echo "☁️  rsync -> $REMOTE:$REMOTE_DIR/ ..."
    as_user ssh "${SSH_OPTS[@]}" "$REMOTE" "mkdir -p '$REMOTE_DIR'"
    as_user rsync -avz -e "ssh ${SSH_OPTS[*]}" "$H5_PATH" "$REMOTE:$REMOTE_DIR/"

    echo "✅ finalize complete: $H5_NAME (local kept at $H5_PATH, backed up to $REMOTE:$REMOTE_DIR/)"
}
