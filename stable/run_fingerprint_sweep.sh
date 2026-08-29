#!/usr/bin/env bash
# Real-browser stress-ng fingerprinting sweep (Stage 3) — front-end to sweep_lib.sh.
#
# Declares this stage's experiment parameters and names its runner; every mechanism
# (coordinator start/health, cleanup traps, validation, the run loop, the finalize gate)
# lives in sweep_lib.sh and is shared with run_website_sweep.sh, so the two stages cannot
# drift apart. Same split as finalize_lib.sh / finalize_realbrowser.sh.
#
# One config label per NoC ({NoC}C_{TST}TST_{K}K_{CYCLES}cycles) is handed straight to
# FingerprintOrchestrator -- there is no batch_runner-style bridge, because the orchestrator
# IS the per-config runner. Each run opens Chrome on core 0 (the JS sampler), runs the
# stress-ng battery on core 1, and writes one memorygram CSV per sample under
#   ../JavaScript/data/realbrowser_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles/<stressor>/<n>.csv
# (the server auto-increments <n>, so re-running APPENDS rather than overwrites).
#
# On a FULLY successful sweep it finalizes: pack every NoC's CSVs into ONE .h5 (inner groups
# = NoCs), delete the source CSVs, and rsync to the remote archive. See finalize_realbrowser.sh.
# Any failed NoC keeps ALL CSVs for retry.
#
# Run this AS YOUR NORMAL USER (not via sudo): it needs xhost for your X session, starts the
# server as you (so the CSVs are owned by you), and finalizes with your ssh keys. It calls
# `sudo` only for the orchestrator (which pins cores + launches Chrome as root). Add a
# passwordless sudoers entry, e.g. /etc/sudoers.d/fingerprint_orchestrator (adjust user/path):
#     ubu ALL=(root) NOPASSWD: /home/ubu/Desktop/Michael/lazyMapping_Prober/stable/FingerprintOrchestrator
# then `sudo chmod 0440` it and `sudo visudo -c` to validate.
set -uo pipefail

cd "$(dirname "$0")"   # stable/
SCRIPT_DIR="$(pwd)"

# ============================================
# Configuration — the experiment parameters
# ============================================
# Spatial sweep: one orchestrator run per NoC. Powers of two in [1,64] (the Lazy Mapping regime).
NOCS=(1 2 4 8 16 32 64)
# NOCS=(32)

# Total sampling time per trace, in seconds: the "{N}TST" field of the config label. Parsed by both
# the orchestrator (it waits this long per sample) and main.js (MEASUREMENT_TIME_MS), so it drives
# the real sampling window AND the output tree. Integer seconds only.
TST=6
# Accesses between mock-clock polls: the "{N}K" field. main.js uses it directly; K=0 selects the
# DYNAMIC-K sweep (initial K = nodeCounts[0]*4, floor 90, alpha 0.5) rather than a fixed cadence.
K=180
# Cycles per address: the "{N}cycles" field. main.js sizes the cluster quantum from it
# (Q = CYCLES_PER_ADDRESS * setsPerCluster * ways / clock), so it sets the temporal resolution:
# rows T = floor(TST_ms / (Q * NoC)).
CYCLES_PER_ADDRESS=2288
# Samples collected per stressor class, per NoC.
SAMPLES_PER_CLASS=50
# Cooldown between samples, MICROSECONDS — lets the L3 return to baseline before the next trace.
# Passed straight through as the orchestrator's COOLDOWN_US arg, so the unit here is us, NOT s:
# 500000 = 0.5 s. (NOC_COOLDOWN_S below is still seconds — it feeds bash's `sleep`.)
SAMPLE_COOLDOWN_US=500000
# Settle time between NoC runs (after tearing down that run's Chrome profile).
NOC_COOLDOWN_S=5

# ============================================
# Stage identity / runner (sweep_lib.sh contract)
# ============================================
SWEEP_TITLE="REAL-BROWSER FINGERPRINTING SWEEP (STAGE 3)"
STAGE_TAG="realbrowser"          # the tag fingerprint_orchestrator.c prepends to the config dir
LOG_PREFIX="fp"
CLASS_COUNT=38                   # stress_battery in src/mastikElite.c
CLASS_NOUN="stressor"

ORCH_BIN="./FingerprintOrchestrator"
ORCH_MAKE_TARGET="FingerprintOrchestrator"
ORCH_SUDO="sudo"                 # pins cores and launches Chrome as root
ORCH_EXTRA_ARGS=()
CHROME_PROFILE="/tmp/chrome-fingerprint"
EXTRA_PKILL=(stress-ng)
NEEDS_XHOST=1                    # Chrome runs as root, so :0 needs an explicit grant

# ============================================
# Server / environment
# ============================================
# Resolved once, up front: the server runs with this as its cwd, and server.py's DATA_ROOT="data"
# is relative to it -- so this is also the root finalize_realbrowser.sh packs from.
SERVER_DIR="$(cd "$SCRIPT_DIR/../JavaScript" 2>/dev/null && pwd)" || {
    echo "[sweep] ERROR: JavaScript/ not found next to $SCRIPT_DIR" >&2; exit 1; }
DATA_TREE="$SERVER_DIR/data"
SERVER_LOG="$SCRIPT_DIR/fingerprint_server.log"
SERVER_CORE=2
CONDA_SH="/home/ubu/anaconda3/etc/profile.d/conda.sh"   # sourced to enable `conda activate`
CONDA_ENV="PC37"                                         # env with flask + h5py/pandas
LOG_DIR="$SCRIPT_DIR/fp_logs"

# ============================================
# Finalize / backup (post-sweep)
# ============================================
# After a FULLY successful sweep, pack this experiment's CSVs into one per-experiment .h5 (inner
# groups = NoCs), delete the source CSVs, and rsync the .h5 to the remote archive. Runs only when
# every NoC succeeded. Set DO_FINALIZE=0 to skip. DRY_RUN=1 reports the plan without writing.
DO_FINALIZE="${DO_FINALIZE:-1}"
FINALIZER="$SCRIPT_DIR/finalize_realbrowser.sh"
REMOTE_HOST="${REMOTE_HOST:-132.72.67.152}"
REMOTE_USER="${REMOTE_USER:-michael}"
REMOTE_DIR="${REMOTE_DIR:-/home/michael/michaels_backup_data}"
LOCAL_H5_DIR="${LOCAL_H5_DIR:-$SCRIPT_DIR/h5}"
# Absolute path to a python3 that has h5py + pandas.
PYTHON_BIN="${PYTHON_BIN:-/home/ubu/anaconda3/envs/PC37/bin/python3}"
DRY_RUN="${DRY_RUN:-0}"

# shellcheck source=sweep_lib.sh
source "$SCRIPT_DIR/sweep_lib.sh"
run_sweep
