#!/usr/bin/env bash
# Real-browser stress-ng fingerprinting sweep (Stage 3).
#
# The Stage 3 twin of run_all_configs.sh: every experiment parameter is declared in the
# Configuration block below, composed into one config label per NoC
# ({NoC}C_{TST}TST_{K}K_{CYCLES}cycles), and handed straight to FingerprintOrchestrator -- there is
# no batch_runner-style bridge here because the orchestrator IS the per-config runner.
#
# It starts the Flask coordinator (pinned to core 2), then runs FingerprintOrchestrator for every
# NoC in $NOCS. Each run opens Chrome on core 0 (the JS sampler), runs the stress-ng battery on
# core 1, and writes one memorygram CSV per sample under
#   ../JavaScript/data/realbrowser_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles/<stressor>/<n>.csv
# (the server auto-increments <n>, so re-running APPENDS rather than overwrites).
#
# On a FULLY successful sweep it finalizes: pack every NoC's CSVs into ONE .h5 (inner groups =
# NoCs), delete the source CSVs, and rsync the .h5 to the remote archive. See
# finalize_realbrowser.sh. Any failed NoC keeps ALL CSVs for retry.
#
# Run this AS YOUR NORMAL USER (not via sudo): it needs xhost for your X session, starts the server
# as you (so the CSVs are owned by you), and finalizes with your ssh keys. It calls `sudo` only for
# the orchestrator (which pins cores + launches Chrome as root). Add a passwordless sudoers entry
# for it, e.g. /etc/sudoers.d/fingerprint_orchestrator (adjust user/path):
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
# Total sampling time per trace, in seconds: the "{N}TST" field of the config label. Parsed by both
# the orchestrator (it waits this long per sample) and main.js (MEASUREMENT_TIME_MS), so it drives
# the real sampling window AND the output tree. Integer seconds only.
TST=2
# Accesses between mock-clock polls: the "{N}K" field. main.js uses it directly; K=0 selects the
# DYNAMIC-K sweep (initial K = nodeCounts[0]*4, floor 90, alpha 0.5) rather than a fixed cadence.
K=180
# Cycles per address: the "{N}cycles" field. main.js sizes the cluster quantum from it
# (Q = CYCLES_PER_ADDRESS * setsPerCluster * ways / clock), so it sets the temporal resolution:
# rows T = floor(TST_ms / (Q * NoC)). 4576 is the current real-browser eval config.
CYCLES_PER_ADDRESS=2288
# Samples collected per stressor class, per NoC. 38 stressors, so the run is
# SAMPLES_PER_CLASS * 38 samples of (TST + SAMPLE_COOLDOWN_US/1e6 + overhead) each.
SAMPLES_PER_CLASS=1
# Cooldown between samples, MICROSECONDS — lets the L3 return to baseline before the next trace.
# Passed straight through as the orchestrator's COOLDOWN_US arg, so the unit here is us, NOT s:
# 500000 = 0.5 s. (NOC_COOLDOWN_S below is still seconds — it feeds bash's `sleep`.)
SAMPLE_COOLDOWN_US=500000
# Settle time between NoC runs (after tearing down that run's Chrome profile).
NOC_COOLDOWN_S=5

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
CONDA_ENV="PC37"                                         # env with flask + h5py/pandas (whole pipeline)
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

SERVER_PID=""          # set if WE start the server (so cleanup only kills our own)
SERVER_LISTEN_PID=""   # server.py runs with debug=True, so werkzeug's reloader forks a CHILD that
                       # is the process actually bound to :8080. Killing SERVER_PID alone orphans
                       # that child and leaks the port, so record the real listener as well.

# PID(s) currently listening on :8080. Matching by PORT, never by `pkill -f "python server.py"` --
# a command-line pattern also matches any shell whose argv happens to contain that text.
listener_pids() {
    ss -lptn 'sport = :8080' 2>/dev/null | grep -oP 'pid=\K[0-9]+' | sort -u
}

server_up() {
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8080/fp/state"
}

# Start the Flask coordinator in the background and block until it answers /fp/state.
# Returns non-zero (and dumps the tail of its log) if it never comes up.
start_server() {
    echo "[sweep] starting Flask coordinator (conda env '$CONDA_ENV', core $SERVER_CORE) -> $SERVER_LOG"
    if [ ! -f "$CONDA_SH" ]; then
        echo "[sweep] ERROR: conda.sh not found at $CONDA_SH (set CONDA_SH in this script)" >&2
        return 1
    fi
    # set +u inside the subshell: conda's scripts reference unbound vars (e.g. $PS1) and
    # would abort under the script's `set -u`.
    # We invoke the env's python by its EXPLICIT prefix ($CONDA_PREFIX/bin/python, set by
    # `conda activate`) rather than the bare `python`: if the caller's shell has another env
    # (e.g. PC37) active with an inconsistent PATH, a plain `python` can resolve to the wrong
    # interpreter (one without flask). $CONDA_PREFIX always points at the activated env.
    # shellcheck disable=SC1090
    ( cd "$SERVER_DIR" \
        && set +u \
        && source "$CONDA_SH" \
        && conda activate "$CONDA_ENV" \
        && exec taskset -c "$SERVER_CORE" "$CONDA_PREFIX/bin/python" server.py ) >>"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 60); do server_up && break; sleep 0.5; done
    if ! server_up; then
        echo "[sweep] ERROR: server did not come up on :8080 (see $SERVER_LOG)" >&2
        echo "[sweep] --- tail of $SERVER_LOG ---" >&2
        tail -20 "$SERVER_LOG" >&2 2>/dev/null || true
        SERVER_PID=""
        return 1
    fi
    SERVER_LISTEN_PID="$(listener_pids | head -1)"
    echo "[sweep] server up (pid $SERVER_PID, listener ${SERVER_LISTEN_PID:-?}), serving $SERVER_DIR"
    return 0
}

# The coordinator MUST be reachable before any orchestrator run: without it the browser can never
# report /fp/ready, so the orchestrator burns its full 120 s ready timeout and the whole NoC run is
# lost. Reuse a live server, otherwise (re)start one.
ensure_server() {
    if server_up; then
        return 0
    fi
    if [ -n "$SERVER_PID" ]; then
        echo "[sweep] WARNING: the coordinator we started (pid $SERVER_PID) is gone -- restarting" >&2
        kill "$SERVER_PID" >/dev/null 2>&1 || true   # reap a half-dead process before rebinding :8080
        [ -n "$SERVER_LISTEN_PID" ] && kill "$SERVER_LISTEN_PID" >/dev/null 2>&1 || true
        SERVER_PID=""; SERVER_LISTEN_PID=""
        sleep 1
    fi
    start_server
}

CLEANED=0
cleanup() {
    [ "$CLEANED" = 1 ] && return   # idempotent: EXIT trap may fire after an INT/TERM exit
    CLEANED=1
    echo "[sweep] cleanup: stopping orchestrator / chrome / stressors / server"
    # The orchestrator + its Chrome are root-owned. On a terminal Ctrl+C the orchestrator
    # already got SIGINT (same foreground group) and self-tears-down its Chrome; these
    # sudo pkills are a backstop for non-terminal kills (need root, hence sudo).
    sudo pkill -9 -f FingerprintOrchestrator >/dev/null 2>&1 || true
    sudo pkill -9 -f 'user-data-dir=/tmp/chrome-fingerprint' >/dev/null 2>&1 || true
    sudo pkill -9 stress-ng >/dev/null 2>&1 || true
    # Only tear down a server WE started (a pre-existing one belongs to whoever started it).
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        [ -n "$SERVER_LISTEN_PID" ] && kill "$SERVER_LISTEN_PID" >/dev/null 2>&1 || true
        # Backstop: the reloader may have re-forked since we recorded the listener.
        sleep 0.5
        for p in $(listener_pids); do kill -9 "$p" >/dev/null 2>&1 || true; done
    fi
}
# INT/TERM -> exit, which fires the EXIT trap (cleanup) ONCE and stops the sweep loop.
# (A plain `trap cleanup INT` would clean up but then let the for-loop spawn the next NoC.)
trap 'exit 130' INT TERM
trap cleanup EXIT

format_duration() {
    local seconds=$1
    printf "%02d:%02d:%02d" $((seconds / 3600)) $(((seconds % 3600) / 60)) $((seconds % 60))
}

# ---- validate the parameter block before touching anything ----
# A bad tuple must cost seconds, not a multi-hour run that writes a mislabelled tree.
for v in TST CYCLES_PER_ADDRESS SAMPLES_PER_CLASS; do
    if ! [[ "${!v}" =~ ^[0-9]+$ ]] || [ "${!v}" -lt 1 ]; then
        echo "[sweep] ERROR: $v must be a positive integer, got '${!v}'" >&2; exit 2
    fi
done
for v in K SAMPLE_COOLDOWN_US NOC_COOLDOWN_S; do
    if ! [[ "${!v}" =~ ^[0-9]+$ ]]; then
        echo "[sweep] ERROR: $v must be a non-negative integer, got '${!v}'" >&2; exit 2
    fi
done
if [ "${#NOCS[@]}" -eq 0 ]; then echo "[sweep] ERROR: NOCS is empty" >&2; exit 2; fi
for noc in "${NOCS[@]}"; do
    if ! [[ "$noc" =~ ^[0-9]+$ ]] || [ "$noc" -lt 1 ] || [ "$noc" -gt 64 ] || [ $((noc & (noc - 1))) -ne 0 ]; then
        echo "[sweep] ERROR: NoC must be a power of two in [1,64], got '$noc'" >&2; exit 2
    fi
done

# ---- resolve the config labels once; the loop and the finalizer both use this tuple ----
CONFIGS=()
for noc in "${NOCS[@]}"; do
    CONFIGS+=("${noc}C_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles")
done
TOTAL_CONFIGS=${#CONFIGS[@]}
mkdir -p "$LOG_DIR"

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║        REAL-BROWSER FINGERPRINTING SWEEP (STAGE 3)             ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Configuration:"
echo "  Script Directory:  $SCRIPT_DIR"
echo "  NoCs:              ${NOCS[*]}"
echo "  TST (sampling):    ${TST}s"
echo "  K (poll cadence):  $K$([ "$K" -eq 0 ] && echo '  (dynamic K)')"
echo "  Cycles/address:    $CYCLES_PER_ADDRESS"
echo "  Samples/class:     $SAMPLES_PER_CLASS"
echo "  Sample cooldown:   ${SAMPLE_COOLDOWN_US} us ($((SAMPLE_COOLDOWN_US / 1000)) ms)"
echo "  Config labels:     ${CONFIGS[*]}"
echo "  Data tree:         $DATA_TREE/realbrowser_<NoC>C_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles"
echo "  Log Directory:     $LOG_DIR"
if [ "$DO_FINALIZE" != "0" ]; then
    echo "  Finalize:          -> $LOCAL_H5_DIR + $REMOTE_USER@$REMOTE_HOST:$REMOTE_DIR (dry_run=$DRY_RUN)"
else
    echo "  Finalize:          disabled (DO_FINALIZE=0) — CSVs left in place"
fi
echo ""

# ---- pre-flight ----
# Grant root access to :0 using the SAME display/cookie the orchestrator gives Chrome.
# Without this Chrome can't reach :0 (your ~/.Xauthority is the :1 Xtigervnc cookie) and
# every CSV would be noise. Machine-specific (uid 1000).
echo "[sweep] granting root access to X display :0 (xhost)"
if DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority xhost +SI:localuser:root >/dev/null 2>&1; then
    echo "[sweep] xhost grant OK"
else
    echo "[sweep] WARNING: xhost grant FAILED -- Chrome will not reach :0 and data will be" >&2
    echo "        noise. Fix X access before trusting results." >&2
fi

echo "[sweep] building FingerprintOrchestrator"
make FingerprintOrchestrator || { echo "[sweep] build failed" >&2; exit 1; }

# Verify passwordless sudo FOR THE ORCHESTRATOR ITSELF (so the sweep runs unattended).
# Probing with `sudo -n true` would be wrong: a per-binary NOPASSWD rule does not cover /bin/true,
# so that check reports a false alarm on a correctly configured machine. Invoke the real binary with
# no args (it prints usage and exits 2 immediately) and look for sudo's own refusal on stderr.
if sudo -n ./FingerprintOrchestrator 2>&1 >/dev/null | grep -q 'password is required'; then
    echo "[sweep] WARNING: passwordless sudo not available for FingerprintOrchestrator -- runs will" >&2
    echo "        prompt, and an unattended (nohup) sweep WILL stall. Add the NOPASSWD sudoers" >&2
    echo "        entry (see header)." >&2
else
    echo "[sweep] passwordless sudo OK for FingerprintOrchestrator"
fi

# ---- start the server (reuse one if already up) ----
# This is a HARD pre-flight gate: the sweep does not start unless the coordinator answers.
if server_up; then
    echo "[sweep] server already reachable on :8080 -- reusing it (not starting a new one)"
else
    start_server || { echo "[sweep] ABORT: no coordinator, nothing can be collected" >&2; exit 1; }
fi

# ---- sweep ----
FAIL_COUNT=0
START_TIME=$(date +%s)
for ((i = 0; i < TOTAL_CONFIGS; i++)); do
    CONFIG="${CONFIGS[$i]}"
    RUN_LOG="$LOG_DIR/fp_${CONFIG}_$(date +%Y%m%d_%H%M%S).log"
    echo "============================================================"
    echo "[sweep] [$((i + 1))/$TOTAL_CONFIGS] $CONFIG   ($(date '+%Y-%m-%d %H:%M:%S'))"
    echo "[sweep] log: $RUN_LOG"
    echo "============================================================"
    # Re-gate on every NoC: a coordinator that died during the previous run (OOM, stray pkill,
    # crash) would otherwise cost 120 s of ready-timeout per remaining NoC and collect nothing.
    if ! ensure_server; then
        echo "[sweep] ABORT: coordinator unreachable and could not be restarted" >&2
        FAIL_COUNT=$((FAIL_COUNT + TOTAL_CONFIGS - i))   # count this NoC and every one not run
        break
    fi
    # The two non-label knobs are passed as ARGUMENTS, not `sudo env VAR=...`: the sudoers rule
    # grants NOPASSWD on this binary (any args), whereas `sudo env` would need /usr/bin/env in
    # sudoers -- which is effectively NOPASSWD: ALL, since `sudo env` can exec anything as root.
    # Arguments keep the sweep unattended (no password prompt) without weakening sudo.
    # `set -o pipefail` (top of file) makes this `if` see the ORCHESTRATOR's status, not tee's.
    if ! sudo ./FingerprintOrchestrator "$CONFIG" "$SAMPLES_PER_CLASS" "$SAMPLE_COOLDOWN_US" \
            2>&1 | tee "$RUN_LOG"; then
        echo "[sweep] WARNING: orchestrator failed for $CONFIG -- continuing" >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    # Tear down the run's Chrome so the next NoC starts from a clean profile, then settle.
    sudo pkill -f 'user-data-dir=/tmp/chrome-fingerprint' >/dev/null 2>&1 || true
    sleep "$NOC_COOLDOWN_S"
done
TOTAL_DURATION=$(( $(date +%s) - START_TIME ))

# ============================================
# Summary
# ============================================
echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    SWEEP COMPLETE                              ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Results Summary:"
echo "  Total Configs:       $TOTAL_CONFIGS"
echo "  Successful:          $((TOTAL_CONFIGS - FAIL_COUNT)) / $TOTAL_CONFIGS"
echo "  Failed:              $FAIL_COUNT / $TOTAL_CONFIGS"
echo "  Total Duration:      $(format_duration $TOTAL_DURATION)"
echo "  Data:                $DATA_TREE"
echo "  Logs:                $LOG_DIR"
echo ""

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "⚠️  $FAIL_COUNT NoC run(s) failed — skipping finalize; ALL CSVs preserved for retry."
    exit 1
fi
echo "🎉 All NoC runs completed successfully!"

# ============================================
# Post-sweep: convert -> delete CSVs -> backup (only on a fully successful sweep)
# ============================================
if [ "$DO_FINALIZE" = "0" ]; then
    echo "ℹ️  DO_FINALIZE=0 — skipping h5 conversion/backup; CSVs left in place."
    exit 0
fi
[ -x "$FINALIZER" ] || chmod +x "$FINALIZER" 2>/dev/null || true
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🧩 Finalizing experiment (h5 + delete CSVs + backup)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if REMOTE_HOST="$REMOTE_HOST" REMOTE_USER="$REMOTE_USER" REMOTE_DIR="$REMOTE_DIR" \
   LOCAL_H5_DIR="$LOCAL_H5_DIR" PYTHON_BIN="$PYTHON_BIN" DRY_RUN="$DRY_RUN" \
   "$FINALIZER" "$TST" "$K" "$CYCLES_PER_ADDRESS" "${NOCS[@]}"; then
    echo "✅ Finalize step completed."
    exit 0
else
    echo "❌ Finalize step FAILED — CSVs preserved. Re-run finalize_realbrowser.sh manually."
    exit 1
fi
