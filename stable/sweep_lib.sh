#!/usr/bin/env bash
#
# sweep_lib.sh — the stage-agnostic half of a real-browser sweep.
# SOURCE this; do not execute it. Front-ends: run_fingerprint_sweep.sh (Stage 3, stress-ng)
# and run_website_sweep.sh (Stage 4, websites).
#
# Mirrors the split that finalize_lib.sh / finalize_*.sh already use: a front-end's only job
# is to declare its experiment parameters and name its runner; everything below (coordinator
# start / health / per-NoC re-gate, cleanup traps, parameter validation, label composition,
# the run loop, timing, and the finalize gate) is shared so the two stages cannot drift apart.
#
# Contract — set these, then call run_sweep:
#
#   Experiment parameters
#     NOCS[]              spatial sweep; powers of two in [1,64]
#     TST                 total sampling time per trace, seconds (the "{N}TST" label field)
#     K                   accesses between timer polls (the "{N}K" field); 0 = dynamic K
#     CYCLES_PER_ADDRESS  cycles/address (the "{N}cycles" field); sizes the JS cluster quantum
#     SAMPLES_PER_CLASS   samples per class, per NoC
#     SAMPLE_COOLDOWN_US  cooldown between samples, MICROseconds
#     NOC_COOLDOWN_S      settle time between NoC runs, SECONDS
#     BATCH_SIZE          OPTIONAL. Samples/class per orchestrator INVOCATION. 0 or unset (the
#                         default) = one invocation per NoC, i.e. exactly the pre-batching
#                         behaviour. Any other value splits a NoC into
#                         ceil(SAMPLES_PER_CLASS / BATCH_SIZE) invocations.
#     BATCH_COOLDOWN_S    OPTIONAL. Settle time between batches, SECONDS (default 3).
#     STATUS_PER_BATCH    OPTIONAL. 1 = also push a remote status line per batch (default 0;
#                         a 100-batch x 7-NoC sweep would otherwise make 700 ssh round-trips).
#
#   WHY BATCHING EXISTS (it is not just crash recovery)
#     The browser sampler builds its lazy mapping ONCE per page load (main.js runFingerprint:
#     `new LazyMapping(...)`, whose build() Fisher-Yates-shuffles the pages with Math.random).
#     One orchestrator invocation is therefore one Chrome, one page load, and ONE mapping for
#     every trace it collects -- so with BATCH_SIZE unset, all SAMPLES_PER_CLASS x classes
#     traces of a NoC share a single random mapping, and any mapping-specific quirk is baked
#     into the whole training set. Batching relaunches Chrome per batch, which re-randomises
#     the mapping. BATCH_SIZE=1 gives a fresh mapping per ROUND (one round = one sample of
#     every class), which is a blocked design: the mapping varies across samples of a class but
#     is held constant across classes within a round, so it cannot become class-discriminative.
#     Cluster INDEX semantics are unaffected -- a cluster is defined by address bits 6-11, so
#     column c means the same thing under every mapping; only each eviction set's page
#     composition is re-drawn. Feature columns stay aligned across samples.
#     No C, JS or server change is needed for this: the orchestrator already takes
#     samples/class as argv[2], and server.py's /collect picks the next CSV index by scanning
#     the class directory, so re-invoking APPENDS instead of overwriting.
#     BATCH_SIZE          OPTIONAL. Samples/class per orchestrator INVOCATION. 0 or unset (the
#                         default) = one invocation per NoC, i.e. exactly the pre-batching
#                         behaviour. Any other value splits a NoC into
#                         ceil(SAMPLES_PER_CLASS / BATCH_SIZE) invocations.
#     BATCH_COOLDOWN_S    OPTIONAL. Settle time between batches, SECONDS (default 3).
#     STATUS_PER_BATCH    OPTIONAL. 1 = also push a remote status line per batch (default 0: a
#                         100-batch x 7-NoC sweep would otherwise make 700 ssh round-trips).
#
#   WHY BATCHING EXISTS (it is a MAPPING knob, not just crash recovery)
#     The browser sampler builds its lazy mapping ONCE per page load (main.js runFingerprint:
#     `new LazyMapping(...)`, whose build() Fisher-Yates-shuffles the pages with Math.random).
#     One orchestrator invocation is therefore one Chrome, one page load, and ONE mapping for
#     every trace it collects -- so with BATCH_SIZE unset, all (classes x SAMPLES_PER_CLASS)
#     traces of a NoC share a single random mapping, and any mapping-specific quirk is baked
#     into the whole training set. Batching relaunches Chrome per batch, which re-randomises
#     the mapping. BATCH_SIZE=1 gives a fresh mapping per ROUND (one round = one sample of
#     every class): a blocked design, where the mapping varies across samples of a class but is
#     held constant across classes within a round, so it cannot become class-discriminative.
#     Cluster INDEX semantics are unaffected -- a cluster is defined by address bits 6-11, so
#     column c means the same thing under every mapping; only each eviction set's page
#     composition is re-drawn. Feature columns therefore stay aligned across samples.
#     No C, JS or server change is needed for this: the orchestrator already takes samples/class
#     as argv[2], and server.py's /collect picks the next CSV index by scanning the class
#     directory (next_index), so re-invoking APPENDS rather than overwrites.
#
#   Identity
#     SCRIPT_DIR          stable/
#     SWEEP_TITLE         banner text
#     STAGE_TAG           config-dir prefix the orchestrator prepends ("realbrowser"/"website")
#     LOG_PREFIX          per-run log filename prefix
#     CLASS_COUNT         number of classes (for the plan/estimate line)
#     CLASS_NOUN          what a class is ("stressor" / "site")
#
#   Runner
#     ORCH_BIN            path to the orchestrator binary (relative to SCRIPT_DIR)
#     ORCH_MAKE_TARGET    make target that builds it
#     ORCH_SUDO           "sudo" if the orchestrator needs root, "" otherwise
#     ORCH_EXTRA_ARGS[]   extra args appended after <config> <samples> <cooldown_us>
#     CHROME_PROFILE      --user-data-dir this stage's Chrome uses (must be stage-unique)
#     EXTRA_PKILL[]       additional process names to reap on cleanup (e.g. stress-ng)
#     NEEDS_XHOST         1 if Chrome runs as root and needs an xhost grant on :0
#
#   Finalize (see finalize_lib.sh)
#     DO_FINALIZE FINALIZER REMOTE_HOST REMOTE_USER REMOTE_DIR LOCAL_H5_DIR PYTHON_BIN DRY_RUN
#
#   Remote status log (optional; defaults below)
#     STATUS_ENABLE      1 to push a per-NoC progress line to the backup host (default 1)
#     STATUS_REMOTE_DIR  directory on REMOTE_HOST to append the log to
#     STATUS_RETRIES     attempts per push before giving up and continuing (default 3)
#
#   Server
#     SERVER_DIR SERVER_LOG SERVER_CORE CONDA_SH CONDA_ENV LOG_DIR DATA_TREE
#
# Safety contract, unchanged: a bad parameter must cost seconds, not a multi-hour run that
# writes a mislabelled tree; and the finalize (which DELETES the CSVs) runs only when every
# NoC succeeded.

SERVER_PID=""          # set if WE start the server (so cleanup only kills our own)
SERVER_LISTEN_PID=""   # server.py runs with debug=True, so werkzeug's reloader forks a CHILD that
                       # is the process actually bound to :8080. Killing SERVER_PID alone orphans
                       # that child and leaks the port, so record the real listener as well.

# Wrap the last character in a bracket expression: 'FooBar' -> 'FooBa[r]'.
#
# pkill -f matches against every process's full command line INCLUDING the /bin/sh -c that
# bash spawns to run the pkill itself, so a literal pattern makes pkill race to kill the shell
# that invoked it (pkill excludes its own pid, but not its parent). The bracket matches the
# same character while making the literal command line no longer match the regex -- the
# standard `ps | grep '[p]attern'` trick.
bracketed() {
    local s="$1"
    printf '%s[%s]' "${s%?}" "${s: -1}"
}

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
        echo "[sweep] ERROR: conda.sh not found at $CONDA_SH (set CONDA_SH in the front-end)" >&2
        return 1
    fi
    # set +u inside the subshell: conda's scripts reference unbound vars (e.g. $PS1) and
    # would abort under the script's `set -u`.
    # We invoke the env's python by its EXPLICIT prefix ($CONDA_PREFIX/bin/python, set by
    # `conda activate`) rather than the bare `python`: if the caller's shell has another env
    # active with an inconsistent PATH, a plain `python` can resolve to the wrong interpreter
    # (one without flask). $CONDA_PREFIX always points at the activated env.
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
# report /fp/ready, so the orchestrator burns its full ready timeout and the whole NoC run is
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

# ---------------------------------------------------------------------------
# Remote status log.
#
# Purpose: watch a multi-day sweep WITHOUT logging into the experiment machine. After each
# NoC the sweep appends one line to a file on the backup host -- progress only, never data.
#
# Synchronous by design: the push happens before the next NoC starts, so the remote log can
# never claim a NoC finished after the following one already began. But it is BOUNDED
# (STATUS_RETRIES attempts): an unreachable backup host must not stall a 39 h experiment, so
# after the last attempt the sweep warns loudly and carries on. The line is always echoed
# locally too, so nothing is lost when the push fails.
# ---------------------------------------------------------------------------
STATUS_ENABLE="${STATUS_ENABLE:-1}"
STATUS_REMOTE_DIR="${STATUS_REMOTE_DIR:-/home/michael/experimentStatusLogs}"
STATUS_RETRIES="${STATUS_RETRIES:-3}"
STATUS_LOG_NAME=""     # resolved once per sweep, in run_sweep

# ---------------------------------------------------------------------------
# Batching defaults.
#
# Defaulted HERE, at library scope, because the front-ends run under `set -u`: a front-end that
# never heard of batching (run_fingerprint_sweep.sh) must not abort on an unbound BATCH_SIZE.
# 0 = no batching, which is the pre-batching behaviour exactly.
# ---------------------------------------------------------------------------
BATCH_SIZE="${BATCH_SIZE:-0}"
BATCH_COOLDOWN_S="${BATCH_COOLDOWN_S:-3}"
STATUS_PER_BATCH="${STATUS_PER_BATCH:-0}"
# Rough per-batch fixed cost (Chrome launch + the 1 s-granularity CDP/ready polls + mapping
# build + teardown). Used ONLY to keep the wall-time estimate honest once batching is on.
BATCH_OVERHEAD_S="${BATCH_OVERHEAD_S:-10}"
# How long to wait for the previous batch's DevTools endpoint to disappear before launching the
# next Chrome. See wait_cdp_clear.
CDP_CLEAR_TIMEOUT_S="${CDP_CLEAR_TIMEOUT_S:-20}"

# Append one line to the remote status log. Never fatal.
status_push() {
    local line="$1"
    echo "$line"                                   # local copy first: the push may fail
    [ "$STATUS_ENABLE" = "1" ] || return 0
    [ -n "$STATUS_LOG_NAME" ] || return 0

    local remote="${REMOTE_USER:+$REMOTE_USER@}$REMOTE_HOST"
    local attempt
    for ((attempt = 1; attempt <= STATUS_RETRIES; attempt++)); do
        # The line goes over STDIN, not inside the remote command string: it contains spaces,
        # '=' and '/' and would otherwise need a second layer of quoting to survive the remote
        # shell. BatchMode keeps a missing key from turning into a password prompt that would
        # hang an unattended sweep.
        if printf '%s\n' "$line" | ssh -o BatchMode=yes -o ConnectTimeout=10 \
                -o ServerAliveInterval=5 -o ServerAliveCountMax=2 "$remote" \
                "mkdir -p '$STATUS_REMOTE_DIR' && cat >> '$STATUS_REMOTE_DIR/$STATUS_LOG_NAME'" \
                2>/dev/null; then
            return 0
        fi
        [ "$attempt" -lt "$STATUS_RETRIES" ] && sleep 5
    done
    echo "[sweep] WARNING: could not push status to $remote:$STATUS_REMOTE_DIR after" \
         "$STATUS_RETRIES attempts -- continuing (the run is unaffected; only the remote" \
         "status log is behind)" >&2
    return 1
}

# Reap this stage's Chrome. Used both between NoC runs and on cleanup.
kill_stage_chrome() {
    $ORCH_SUDO pkill -9 -f "user-data-dir=$(bracketed "$CHROME_PROFILE")" >/dev/null 2>&1 || true
}

# Block until Chrome's DevTools endpoint is GONE (bounded by CDP_CLEAR_TIMEOUT_S).
#
# Batching introduces a failure mode a single-invocation run never had: the next orchestrator
# launches a browser and then polls :9222 for the DevTools endpoint (web_wait_cdp). If the
# PREVIOUS batch's Chrome is still shutting down, that poll can answer from the DYING instance
# -- the orchestrator would then drive a browser that is about to vanish, and the batch is lost
# (or worse, half-collected). SIGTERM + teardown is not instant, so wait for the port to go
# quiet instead of guessing with a sleep. Non-fatal on timeout: warn and let the orchestrator's
# own health checks deal with it, rather than killing a multi-day sweep here.
wait_cdp_clear() {
    local waited=0
    while curl -s -o /dev/null --max-time 1 "http://127.0.0.1:9222/json/version"; do
        if [ "$waited" -ge "$CDP_CLEAR_TIMEOUT_S" ]; then
            echo "[sweep] WARNING: DevTools still answering on :9222 after ${waited}s -- a" \
                 "previous Chrome did not die. The next batch may attach to it." >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

CLEANED=0
cleanup() {
    [ "$CLEANED" = 1 ] && return   # idempotent: EXIT trap may fire after an INT/TERM exit
    CLEANED=1
    echo "[sweep] cleanup: stopping orchestrator / chrome / victims / server"
    # On a terminal Ctrl+C the orchestrator already got SIGINT (same foreground group) and
    # self-tears-down its Chrome; these pkills are a backstop for non-terminal kills. They run
    # under $ORCH_SUDO because a root-launched orchestrator's tree needs root to signal.
    $ORCH_SUDO pkill -9 -f "$(bracketed "$(basename "$ORCH_BIN")")" >/dev/null 2>&1 || true
    kill_stage_chrome
    for name in ${EXTRA_PKILL[@]+"${EXTRA_PKILL[@]}"}; do
        $ORCH_SUDO pkill -9 "$name" >/dev/null 2>&1 || true
    done
    # Only tear down a server WE started (a pre-existing one belongs to whoever started it).
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        [ -n "$SERVER_LISTEN_PID" ] && kill "$SERVER_LISTEN_PID" >/dev/null 2>&1 || true
        # Backstop: the reloader may have re-forked since we recorded the listener.
        sleep 0.5
        for p in $(listener_pids); do kill -9 "$p" >/dev/null 2>&1 || true; done
    fi
}

format_duration() {
    local seconds=$1
    printf "%02d:%02d:%02d" $((seconds / 3600)) $(((seconds % 3600) / 60)) $((seconds % 60))
}

# ---------------------------------------------------------------------------
# The sweep itself.
# ---------------------------------------------------------------------------
run_sweep() {
    # INT/TERM -> exit, which fires the EXIT trap (cleanup) ONCE and stops the sweep loop.
    # (A plain `trap cleanup INT` would clean up but then let the for-loop spawn the next NoC.)
    trap 'exit 130' INT TERM
    trap cleanup EXIT

    # ---- validate the parameter block before touching anything ----
    local v noc
    for v in TST CYCLES_PER_ADDRESS SAMPLES_PER_CLASS; do
        if ! [[ "${!v}" =~ ^[0-9]+$ ]] || [ "${!v}" -lt 1 ]; then
            echo "[sweep] ERROR: $v must be a positive integer, got '${!v}'" >&2; exit 2
        fi
    done
    for v in K SAMPLE_COOLDOWN_US NOC_COOLDOWN_S BATCH_SIZE BATCH_COOLDOWN_S; do
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
    local CONFIGS=()
    for noc in "${NOCS[@]}"; do
        CONFIGS+=("${noc}C_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles")
    done
    local TOTAL_CONFIGS=${#CONFIGS[@]}
    mkdir -p "$LOG_DIR"

    # ---- resolve batching (see "WHY BATCHING EXISTS" in the header) ----
    # BATCH_SIZE of 0 (the default), or one >= SAMPLES_PER_CLASS, collapses to a SINGLE
    # invocation per NoC -- byte-identical to the pre-batching path, which is what Stage 3 gets.
    local BATCH_N=$SAMPLES_PER_CLASS NUM_BATCHES=1
    if [ "$BATCH_SIZE" -gt 0 ] && [ "$BATCH_SIZE" -lt "$SAMPLES_PER_CLASS" ]; then
        BATCH_N=$BATCH_SIZE
        NUM_BATCHES=$(( (SAMPLES_PER_CLASS + BATCH_N - 1) / BATCH_N ))
    fi

    # Per-sample wall time: the trace, the ack margin, the cooldown, and ~1 s of victim setup.
    local per_sample_s
    per_sample_s=$(awk -v t="$TST" -v c="$SAMPLE_COOLDOWN_US" 'BEGIN{printf "%.2f", t + 0.5 + c/1e6 + 1.0}')
    # Batching adds a fixed per-invocation cost (relaunch + mapping build) and a cooldown; at
    # BATCH_SIZE=1 that is paid once per round, so the estimate must include it or a batched
    # sweep will look like it is running late when it is exactly on schedule.
    local est_h
    est_h=$(awk -v n="$CLASS_COUNT" -v s="$SAMPLES_PER_CLASS" -v k="$TOTAL_CONFIGS" \
                -v p="$per_sample_s" -v b="$NUM_BATCHES" -v o="$BATCH_OVERHEAD_S" \
                -v bc="$BATCH_COOLDOWN_S" \
                'BEGIN{printf "%.1f", (n*s*k*p + k*b*(o + bc))/3600}')

    echo ""
    echo "╔════════════════════════════════════════════════════════════════╗"
    printf "║  %-62s║\n" "$SWEEP_TITLE"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Configuration:"
    echo "  Script Directory:  $SCRIPT_DIR"
    echo "  NoCs:              ${NOCS[*]}"
    echo "  TST (sampling):    ${TST}s"
    echo "  K (poll cadence):  $K$([ "$K" -eq 0 ] && echo '  (dynamic K)')"
    echo "  Cycles/address:    $CYCLES_PER_ADDRESS"
    echo "  Classes:           $CLASS_COUNT ${CLASS_NOUN}s"
    echo "  Samples/class:     $SAMPLES_PER_CLASS"
    if [ "$NUM_BATCHES" -gt 1 ]; then
        echo "  Batching:          $NUM_BATCHES batches/NoC x $BATCH_N sample(s)/${CLASS_NOUN} \
-> a FRESH lazy mapping every batch (${BATCH_COOLDOWN_S}s between batches)"
    else
        echo "  Batching:          off (1 invocation/NoC -> ONE lazy mapping for every trace)"
    fi
    echo "  Sample cooldown:   ${SAMPLE_COOLDOWN_US} us ($((SAMPLE_COOLDOWN_US / 1000)) ms)"
    echo "  Config labels:     ${CONFIGS[*]}"
    echo "  Data tree:         $DATA_TREE/${STAGE_TAG}_<NoC>C_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles"
    echo "  Log Directory:     $LOG_DIR"
    if [ "$NUM_BATCHES" -gt 1 ]; then
        echo "  Est. wall time:    ~${est_h} h  (${per_sample_s}s/sample x $CLASS_COUNT x $SAMPLES_PER_CLASS x $TOTAL_CONFIGS" \
             "+ $((TOTAL_CONFIGS * NUM_BATCHES)) batches x $((BATCH_OVERHEAD_S + BATCH_COOLDOWN_S))s relaunch)"
    else
        echo "  Est. wall time:    ~${est_h} h  (${per_sample_s}s/sample x $CLASS_COUNT x $SAMPLES_PER_CLASS x $TOTAL_CONFIGS)"
    fi
    if [ "$DO_FINALIZE" != "0" ]; then
        echo "  Finalize:          -> $LOCAL_H5_DIR + $REMOTE_USER@$REMOTE_HOST:$REMOTE_DIR (dry_run=$DRY_RUN)"
    else
        echo "  Finalize:          disabled (DO_FINALIZE=0) — CSVs left in place"
    fi
    echo ""

    # ---- pre-flight ----
    if [ "$NEEDS_XHOST" = "1" ]; then
        # Grant root access to :0 using the SAME display/cookie the orchestrator gives Chrome.
        # Without this Chrome can't reach :0 (~/.Xauthority is the :1 Xtigervnc cookie) and every
        # CSV would be noise. Machine-specific (uid 1000). Only needed when Chrome runs as root.
        echo "[sweep] granting root access to X display :0 (xhost)"
        if DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority xhost +SI:localuser:root >/dev/null 2>&1; then
            echo "[sweep] xhost grant OK"
        else
            echo "[sweep] WARNING: xhost grant FAILED -- Chrome will not reach :0 and data will be" >&2
            echo "        noise. Fix X access before trusting results." >&2
        fi
    fi

    echo "[sweep] building $ORCH_MAKE_TARGET"
    make "$ORCH_MAKE_TARGET" || { echo "[sweep] build failed" >&2; exit 1; }

    if [ -n "$ORCH_SUDO" ]; then
        # Verify passwordless sudo FOR THE ORCHESTRATOR ITSELF (so the sweep runs unattended).
        # Probing with `sudo -n true` would be wrong: a per-binary NOPASSWD rule does not cover
        # /bin/true, so that check reports a false alarm on a correctly configured machine.
        # Invoke the real binary with no args (it prints usage and exits 2 immediately) and look
        # for sudo's own refusal on stderr.
        if sudo -n "$ORCH_BIN" 2>&1 >/dev/null | grep -q 'password is required'; then
            echo "[sweep] WARNING: passwordless sudo not available for $ORCH_BIN -- runs will" >&2
            echo "        prompt, and an unattended (nohup) sweep WILL stall. Add the NOPASSWD" >&2
            echo "        sudoers entry (see the front-end's header)." >&2
        else
            echo "[sweep] passwordless sudo OK for $ORCH_BIN"
        fi
    fi

    # ---- start the server (reuse one if already up) ----
    # HARD pre-flight gate: the sweep does not start unless the coordinator answers.
    if server_up; then
        echo "[sweep] server already reachable on :8080 -- reusing it (not starting a new one)"
    else
        start_server || { echo "[sweep] ABORT: no coordinator, nothing can be collected" >&2; exit 1; }
    fi

    # ---- sweep ----
    local FAIL_COUNT=0 i CONFIG RUN_LOG
    local START_TIME; START_TIME=$(date +%s)

    # One status log per sweep: same stem as the .h5 this run will produce, plus a timestamp so
    # a re-run never appends into the previous attempt's file.
    STATUS_LOG_NAME="${STAGE_TAG}_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles_$(date +%Y%m%d_%H%M%S).log"
    if [ "$STATUS_ENABLE" = "1" ]; then
        echo "[sweep] remote status log: ${REMOTE_USER:+$REMOTE_USER@}$REMOTE_HOST:$STATUS_REMOTE_DIR/$STATUS_LOG_NAME"
    fi
    status_push "=== SWEEP START $(date '+%Y-%m-%d %H:%M:%S')  host=$(hostname)  stage=$STAGE_TAG  \
classes=$CLASS_COUNT ${CLASS_NOUN}s  samples/class=$SAMPLES_PER_CLASS  NoCs=[${NOCS[*]}]  \
batches/NoC=$NUM_BATCHES x $BATCH_N  \
TST=${TST}s K=$K cycles=$CYCLES_PER_ADDRESS  est=${est_h}h"
    local ABORT=0
    for ((i = 0; i < TOTAL_CONFIGS; i++)); do
        CONFIG="${CONFIGS[$i]}"
        echo "============================================================"
        echo "[sweep] [$((i + 1))/$TOTAL_CONFIGS] $CONFIG   ($(date '+%Y-%m-%d %H:%M:%S'))"
        [ "$NUM_BATCHES" -gt 1 ] && \
            echo "[sweep] $NUM_BATCHES batches x $BATCH_N sample(s)/${CLASS_NOUN}, fresh mapping each"
        echo "============================================================"

        local noc_t0 noc_t1 noc_status noc_csvs b batch_n done_samples batch_fail batch_t0
        noc_t0=$(date +%s)
        noc_status="OK"
        batch_fail=0
        done_samples=0

        # Each batch is one orchestrator invocation = one Chrome = one lazy mapping. With
        # NUM_BATCHES=1 this loop body runs exactly once and does exactly what the
        # pre-batching code did.
        for ((b = 1; b <= NUM_BATCHES; b++)); do
            batch_n=$(( SAMPLES_PER_CLASS - done_samples ))
            [ "$batch_n" -gt "$BATCH_N" ] && batch_n=$BATCH_N

            # Re-gate on every BATCH (it was every NoC): a coordinator that died mid-NoC would
            # otherwise cost a full ready-timeout per remaining batch and collect nothing.
            if ! ensure_server; then
                echo "[sweep] ABORT: coordinator unreachable and could not be restarted" >&2
                ABORT=1
                break
            fi

            if [ "$NUM_BATCHES" -gt 1 ]; then
                RUN_LOG="$LOG_DIR/${LOG_PREFIX}_${CONFIG}_b$(printf '%03d' "$b")_$(date +%Y%m%d_%H%M%S).log"
                echo "[sweep] --- batch $b/$NUM_BATCHES  ($batch_n sample(s)/${CLASS_NOUN}, \
$((done_samples + batch_n))/$SAMPLES_PER_CLASS after this)  $(date '+%H:%M:%S')"
            else
                RUN_LOG="$LOG_DIR/${LOG_PREFIX}_${CONFIG}_$(date +%Y%m%d_%H%M%S).log"
            fi
            echo "[sweep] log: $RUN_LOG"

            # The non-label knobs are passed as ARGUMENTS, not `sudo env VAR=...`: a sudoers rule
            # grants NOPASSWD on the binary (any args), whereas `sudo env` would need /usr/bin/env
            # in sudoers -- effectively NOPASSWD: ALL, since `sudo env` can exec anything as root.
            # `set -o pipefail` (front-end) makes this `if` see the ORCHESTRATOR's status, not
            # tee's.
            batch_t0=$(date +%s)
            if ! $ORCH_SUDO "$ORCH_BIN" "$CONFIG" "$batch_n" "$SAMPLE_COOLDOWN_US" \
                    ${ORCH_EXTRA_ARGS[@]+"${ORCH_EXTRA_ARGS[@]}"} 2>&1 | tee "$RUN_LOG"; then
                echo "[sweep] WARNING: orchestrator failed for $CONFIG" \
                     "$([ "$NUM_BATCHES" -gt 1 ] && echo "(batch $b/$NUM_BATCHES) ")-- continuing" >&2
                batch_fail=$((batch_fail + 1))
                noc_status="FAILED"
            fi
            done_samples=$((done_samples + batch_n))

            # Tear down this batch's Chrome so the next batch (or NoC) starts from a clean
            # profile AND a freshly built mapping.
            kill_stage_chrome

            if [ "$b" -lt "$NUM_BATCHES" ]; then
                # Do not launch the next browser until this one's DevTools endpoint is gone.
                wait_cdp_clear
                if [ "$STATUS_PER_BATCH" = "1" ]; then
                    status_push "$(printf '[%d/%d] NoC=%-3s batch %d/%d  %d sample(s)  duration %s  status=%s' \
                        "$((i + 1))" "$TOTAL_CONFIGS" "${NOCS[$i]}" "$b" "$NUM_BATCHES" \
                        "$batch_n" "$(format_duration $(($(date +%s) - batch_t0)))" \
                        "$([ "$batch_fail" -gt 0 ] && echo FAILED || echo OK)")"
                fi
                sleep "$BATCH_COOLDOWN_S"
            fi
        done
        noc_t1=$(date +%s)

        # An aborted NoC also condemns every NoC after it, so count them all here; otherwise a
        # NoC is failed iff ANY of its batches failed (partial data must never reach finalize).
        if [ "$ABORT" = 1 ]; then
            FAIL_COUNT=$((FAIL_COUNT + TOTAL_CONFIGS - i))   # this NoC and every one not run
            noc_status="ABORT"
        elif [ "$batch_fail" -gt 0 ]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            noc_status="FAILED"
        fi

        # What actually landed on disk -- the honest progress number, independent of exit code.
        noc_csvs=$(find "$DATA_TREE/${STAGE_TAG}_${CONFIG}" -name '*.csv' 2>/dev/null | wc -l)

        # Push BEFORE the cooldown and before the next NoC, so the remote log can never report
        # a NoC as finished out of order.
        status_push "$(printf '[%d/%d] NoC=%-3s started %s  finished %s  duration %s  status=%-6s batches=%d/%d  csvs=%d/%d' \
            "$((i + 1))" "$TOTAL_CONFIGS" "${NOCS[$i]}" \
            "$(date -d "@$noc_t0" '+%Y-%m-%d %H:%M:%S')" \
            "$(date -d "@$noc_t1" '+%Y-%m-%d %H:%M:%S')" \
            "$(format_duration $((noc_t1 - noc_t0)))" \
            "$noc_status" "$((b - 1 - batch_fail))" "$NUM_BATCHES" \
            "$noc_csvs" "$((CLASS_COUNT * SAMPLES_PER_CLASS))")"

        [ "$ABORT" = 1 ] && break
        sleep "$NOC_COOLDOWN_S"
    done
    local TOTAL_DURATION=$(( $(date +%s) - START_TIME ))
    status_push "=== SWEEP END   $(date '+%Y-%m-%d %H:%M:%S')  duration $(format_duration $TOTAL_DURATION)  \
ok=$((TOTAL_CONFIGS - FAIL_COUNT))/$TOTAL_CONFIGS  $([ "$FAIL_COUNT" -eq 0 ] \
    && { [ "$DO_FINALIZE" = "0" ] && echo 'finalize SKIPPED (DO_FINALIZE=0), CSVs kept' || echo '-> finalizing'; } \
    || echo 'finalize SKIPPED, all CSVs preserved for retry')"

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
        echo "❌ Finalize step FAILED — CSVs preserved. Re-run $(basename "$FINALIZER") manually."
        exit 1
    fi
}
