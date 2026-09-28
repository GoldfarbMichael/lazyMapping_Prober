#!/bin/bash

# Script to run batch_runner.sh sequentially for all core counts (powers of 2: 1 to 512)
# Each execution waits for the previous one to complete
# Features:
#   - Sequential execution (not parallel)
#   - 1 minute cooldown between runs
#   - Sudo support
#   - Detailed logging and progress tracking
#   - Proper signal handling for cleanup

# Trap signals to ensure cleanup
cleanup() {
    echo ""
    echo "⚠️  Received interrupt signal - cleaning up..."
    # Kill all stress-ng and MastikElite processes
    sudo pkill -9 stress-ng 2>/dev/null || true
    sudo pkill -9 MastikElite 2>/dev/null || true
    # Website modes (-wn/-wc) leave a Chrome tree behind otherwise; the bracket keeps the
    # pattern from matching this pkill's own command line.
    sudo pkill -9 -f "user-data-dir=/tmp/chrome-website-nativ[e]" 2>/dev/null || true
    # Kill any remaining batch_runner.sh processes
    pkill -9 -P $$ 2>/dev/null || true
    echo "✅ Cleanup complete. Exiting."
    exit 130  # Standard exit code for SIGINT
}

trap cleanup SIGINT SIGTERM

# ============================================
# Configuration
# ============================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BATCH_RUNNER="$SCRIPT_DIR/batch_runner.sh"
# TIMER_MODE="-c"   # Chrome timer (Mastik loaded-e_set clusters)
# TIMER_MODE="-n"   # native timer
# TIMER_MODE="-j"   # Chrome timer + JS-style lazy-map victim (data/chrome_clock_jsmap/...)
# TIMER_MODE="-jn"  # Native timer + JS-style lazy-map victim (data/native_clock_jsmap/...)
# TIMER_MODE="-jb"  # Chrome timer + JS lazy map BIDIRECTIONAL (data/chrome_clock_jsmap_bidir/...)
# TIMER_MODE="-jnb" # Native timer + JS lazy map BIDIRECTIONAL (data/native_clock_jsmap_bidir/...)
# TIMER_MODE="-jss" # Chrome timer + JS lazy map SINGLE-SWEEP idle-fill (data/chrome_clock_jsmapSS/...)
# TIMER_MODE="-jssb"  # Chrome timer + JS lazy map SINGLE-SWEEP BIDIRECTIONAL (data/chrome_clock_jsmapSS_bidir/...)
# TIMER_MODE="-jnss" # Native timer + JS lazy map SINGLE-SWEEP idle-fill (data/native_clock_jsmapSS/...)
# TIMER_MODE="-jnssb" # Native timer + JS lazy map SINGLE-SWEEP BIDIRECTIONAL (data/native_clock_jsmapSS_bidir/...)
# --- Stage 4b: real-website victim, sampled by the NATIVE Mastik prober (full mapping) ---
TIMER_MODE="-wn"  # Native timer + Mastik clusters + WEBSITE victim (data/native_clock_website/...)
# TIMER_MODE="-wc"  # Chrome timer + Mastik clusters + WEBSITE victim (data/chrome_clock_website/...)
# Shuffled-cluster A/B: set to "-s" WITH TIMER_MODE="-c" to line-shuffle the Mastik clusters
# once -> data/chrome_clock_shuffled/. Empty (default) = normal contiguous clusters.
SHUFFLE_FLAG="-s"
# Cycles per address: the "{N}cycles" field of the config label. The C tool parses it
# (parse_cycles_from_dirname) and sizes the cluster quantum as SST = N*setsPerCluster*assoc,
# exactly as JS main.js does with CYCLES_PER_ADDRESS. 2288 = the Chrome-mock eval config;
# 300 reproduces the legacy native-clock sizing (200*1.5).
CYCLES_PER_ADDRESS=2288
# Total sampling time per trace, in seconds: the "{N}TST" field of the config label. The C tool
# parses it (parse_TST_from_dirname) and it now drives the real sampling window, so changing this
# changes both the data and its output tree (data/<clock>/<NoC>C_<TST>TST_.../). Integer seconds
# only. Memorygram rows scale linearly with TST (rows = TST_cycles / (NoC * SST_cycles)).
TST=4
# Accesses between clock polls: the "{N}K" field of the config label. Parsed by the C tool
# (parse_K_from_dirname) and now honoured by EVERY sampler, so this really does change the data.
#   90 = the historical fixed cadence (what every previous sweep used)
#    0 = DYNAMIC K -- first batch ~4 cluster sweeps, then damped toward the deadline, floored at
#        MIN_DYNAMIC_K; ~5-9 clock reads per quantum instead of one per 90 accesses. Mirrors JS
#        sweepClusterDynamicK, so it is the like-for-like setting against the browser arms.
# NOTE: timer_mode -n (stress-ng, native clock) IGNORES this and always polls every access, so
# that new native_clock data stays comparable with the published Stage 1 tree. Every other mode
# follows the label.
K="${K:-90}"
# Victim buffer size in MB for the jsmap modes (-j/-jn/-jb/-jnb). Must be a multiple of 12.
# 12 (default) = one LLC = mean 12 lines/set; 24 = mean 24 lines/set. Non-default sizes write to
# their own tree (data/<clock>_jsmap[_bidir]_<N>MB/), so 12 MB data is never overwritten.
# Overridable from the environment: JSMAP_BUF_MB=24 ./run_all_configs.sh
JSMAP_BUF_MB="${JSMAP_BUF_MB:-12}"
# Site list for the website modes (-wn/-wc): '<slug>\t<url>' per line, '#' comments. Each slug
# becomes a class directory and an .h5 label, so trimming this file is how you trade run time
# against class count. Ignored by every other timer mode.
SITES_FILE="${SITES_FILE:-$SCRIPT_DIR/sites.txt}"
# Cooldown between website samples, MICROSECONDS. Lets the L3 return to baseline before the
# next page load. Ignored by every other timer mode.
WEB_COOLDOWN_US="${WEB_COOLDOWN_US:-500000}"
# The login user Chrome should drop to for the website modes. Captured HERE because this is the
# last point in the chain where it is still known: the sweep escalates TWICE
# (run_all_configs.sh -> sudo -> batch_runner.sh -> sudo -> MastikElite), and when root invokes
# sudo, sudo sets SUDO_UID=0 -- so the C tool cannot recover the real user on its own. Empty
# means "no non-root user available", and the C tool then warns and falls back to root+--no-sandbox.
if [ "$(id -u)" -eq 0 ]; then
    CHROME_UID="${SUDO_UID:-}"     # already root: only sudo knows who invoked us
    CHROME_GID="${SUDO_GID:-}"
else
    CHROME_UID="$(id -u)"          # normal case: we ARE the login user
    CHROME_GID="$(id -g)"
fi
COOLDOWN_SECS=60
LOG_DIR="$SCRIPT_DIR/batch_logs"

# ============================================
# Remote per-NoC status log (mirrors sweep_lib.sh, used by the Stage 3/4 browser sweeps)
# ============================================
# Appends ONE line per NoC to a log on the backup host, so a multi-hour sweep can be watched
# from anywhere without ssh-ing in to tail a local file. Synchronous by design: the push happens
# before the next NoC starts, so the remote log can never claim a NoC finished after the
# following one already began. But it is BOUNDED (STATUS_RETRIES) -- an unreachable backup host
# must not stall a 27 h experiment. Every line is echoed locally first, so nothing is lost when
# the push fails.
STATUS_ENABLE="${STATUS_ENABLE:-1}"
STATUS_REMOTE_DIR="${STATUS_REMOTE_DIR:-/home/michael/experimentStatusLogs}"
STATUS_RETRIES="${STATUS_RETRIES:-3}"
STATUS_LOG_NAME=""     # resolved once below, from the .h5 stem this sweep will produce

# The naming table (timer mode -> tree name, timer mode -> .h5 label) shared with
# finalize_experiment.sh, so the status log is named after the .h5 this run will produce.
# shellcheck source=clock_labels.sh
source "$SCRIPT_DIR/clock_labels.sh"

# ============================================
# Finalize / backup (post-sweep)
# ============================================
# After a FULLY successful sweep, pack this experiment's CSVs into one per-experiment .h5
# (inner groups = NoCs), delete the source CSVs, and rsync the .h5 to the remote archive.
# Runs only when FAIL_COUNT==0 (any failed config keeps ALL CSVs for retry). See
# finalize_experiment.sh. Set DO_FINALIZE=0 to skip. DRY_RUN=1 reports the plan without writing.
DO_FINALIZE="${DO_FINALIZE:-1}"
FINALIZER="$SCRIPT_DIR/finalize_experiment.sh"
REMOTE_HOST="${REMOTE_HOST:-132.72.67.152}"
REMOTE_USER="${REMOTE_USER:-michael}"
REMOTE_DIR="${REMOTE_DIR:-/home/michael/michaels_backup_data}"
LOCAL_H5_DIR="${LOCAL_H5_DIR:-$SCRIPT_DIR/h5}"
# Absolute path to a python3 that has h5py + pandas (the login user's conda env, NOT root's).
PYTHON_BIN="${PYTHON_BIN:-/home/ubu/anaconda3/envs/PC37/bin/python3}"
DRY_RUN="${DRY_RUN:-0}"

# The jsmap victim shuffles its pages internally (build_lazy_mapping); -s is a Mastik-e_set-only
# knob, so never forward a stale shuffle flag into any jsmap run (forward-only or bidirectional).
# Website modes: no lazy map and no Mastik-e_set shuffle, so never forward a stale -s.
if [[ "$TIMER_MODE" == "-wn" || "$TIMER_MODE" == "-wc" ]]; then
    SHUFFLE_FLAG=""
    IS_WEBSITE=1
else
    IS_WEBSITE=0
fi
if [[ "$TIMER_MODE" == "-j" || "$TIMER_MODE" == "-jn" \
   || "$TIMER_MODE" == "-jb" || "$TIMER_MODE" == "-jnb" \
   || "$TIMER_MODE" == "-jss" || "$TIMER_MODE" == "-jssb" \
   || "$TIMER_MODE" == "-jnss" || "$TIMER_MODE" == "-jnssb" ]]; then
    SHUFFLE_FLAG=""
    IS_JSMAP=1
else
    IS_JSMAP=0
fi

# Validate the buffer size here so a bad value fails immediately instead of after the first
# config has already started. batch_runner.sh re-validates; the C tool falls back to 12.
if ! [[ "$JSMAP_BUF_MB" =~ ^[0-9]+$ ]] || [ "$JSMAP_BUF_MB" -lt 12 ] || [ $((JSMAP_BUF_MB % 12)) -ne 0 ]; then
    echo "❌ JSMAP_BUF_MB must be a multiple of 12 (>=12), got '$JSMAP_BUF_MB'"
    exit 2
fi
if [ "$IS_JSMAP" -eq 0 ] && [ "$IS_WEBSITE" -eq 0 ] && [ "$JSMAP_BUF_MB" != 12 ]; then
    echo "⚠️  JSMAP_BUF_MB=$JSMAP_BUF_MB has no effect with TIMER_MODE=$TIMER_MODE (no lazy map)."
fi

# Fail fast on a missing/short site list: a bad list must cost seconds here, not a multi-hour
# sweep that writes a mislabelled tree.
if [ "$IS_WEBSITE" -eq 1 ]; then
    if [[ ! -f "$SITES_FILE" ]]; then
        echo "❌ TIMER_MODE=$TIMER_MODE needs a sites file; '$SITES_FILE' not found"
        exit 2
    fi
    NUM_SITES=$(grep -cE '^[[:space:]]*[^#[:space:]]' "$SITES_FILE" || true)
    if ! [[ "$NUM_SITES" =~ ^[0-9]+$ ]] || [ "$NUM_SITES" -lt 2 ]; then
        echo "❌ '$SITES_FILE' has ${NUM_SITES:-0} uncommented site(s); need at least 2"
        exit 2
    fi
fi

# ============================================
# Verify prerequisites
# ============================================
if [[ ! -f "$BATCH_RUNNER" ]]; then
    echo "❌ Error: batch_runner.sh not found at $BATCH_RUNNER"
    exit 1
fi

if [[ ! -x "$BATCH_RUNNER" ]]; then
    echo "⚠️  Warning: batch_runner.sh is not executable. Making it executable..."
    chmod +x "$BATCH_RUNNER"
fi

mkdir -p "$LOG_DIR"

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║     SEQUENTIAL BATCH RUNNER FOR ALL CLUSTER COUNTS.            ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Configuration:"
echo "  Script Directory:  $SCRIPT_DIR"
echo "  Batch Runner:      $BATCH_RUNNER"
echo "  Timer Mode:        $TIMER_MODE"
echo "  Shuffle Flag:      ${SHUFFLE_FLAG:-<none>}"
echo "  Cycles/address:    $CYCLES_PER_ADDRESS"
echo "  TST (sampling):    ${TST}s"
if [ "$K" = "0" ]; then
    echo "  K (poll cadence):  dynamic"
else
    echo "  K (poll cadence):  $K accesses/poll"
fi
if [ "$IS_WEBSITE" -eq 1 ]; then
    echo "  Sites file:        $SITES_FILE ($NUM_SITES sites)"
    echo "  Sample cooldown:   ${WEB_COOLDOWN_US} us"
    if [ -n "$CHROME_UID" ] && [ "$CHROME_UID" != 0 ]; then
        echo "  Chrome runs as:    uid=$CHROME_UID gid=$CHROME_GID (sandboxed)"
    else
        echo "  Chrome runs as:    ⚠️  ROOT (--no-sandbox) — no login uid could be determined"
    fi
fi
if [ "$IS_JSMAP" -eq 1 ]; then
    if [ "$JSMAP_BUF_MB" = 12 ]; then
        echo "  Victim buffer:     ${JSMAP_BUF_MB} MB (default tree)"
    else
        echo "  Victim buffer:     ${JSMAP_BUF_MB} MB  -> tree tagged _${JSMAP_BUF_MB}MB"
    fi
fi
echo "  Cooldown:          $COOLDOWN_SECS seconds"
echo "  Log Directory:     $LOG_DIR"
echo ""

# ============================================
# Verify sudo access
# ============================================
echo "🔐 Verifying sudo credentials..."
sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Failed to authenticate with sudo. Exiting."
    exit 1
fi
echo "✅ Sudo credentials verified"
echo ""

# ============================================
# Helper functions
# ============================================
print_separator() {
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# Run a command as the LOGIN user. This script runs under sudo, so a bare ssh would use root's
# keys and known_hosts, not yours -- the same reason finalize_lib.sh has this helper.
as_user() {
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        sudo -H -u "$SUDO_USER" "$@"
    else
        "$@"
    fi
}

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
        if printf '%s\n' "$line" | as_user ssh -o BatchMode=yes -o ConnectTimeout=10 \
                -o ServerAliveInterval=5 -o ServerAliveCountMax=2 "$remote" \
                "mkdir -p '$STATUS_REMOTE_DIR' && cat >> '$STATUS_REMOTE_DIR/$STATUS_LOG_NAME'" \
                2>/dev/null; then
            return 0
        fi
        [ "$attempt" -lt "$STATUS_RETRIES" ] && sleep 5
    done
    echo "⚠️  Could not push status to $remote:$STATUS_REMOTE_DIR after $STATUS_RETRIES attempts" \
         "— continuing (the run is unaffected; only the remote status log is behind)" >&2
    return 1
}

format_duration() {
    local seconds=$1
    local hours=$((seconds / 3600))
    local minutes=$(((seconds % 3600) / 60))
    local secs=$((seconds % 60))
    printf "%02d:%02d:%02d" $hours $minutes $secs
}

# ============================================
# Main execution loop
# ============================================
CONFIGS=()
# POWERS_OF_2=(1 2 4 8 16 32 64 256 512 1024 2048 4096)
# POWERS_OF_2=(1 2 4 8 16 32 64)
POWERS_OF_2=(2 4 8 16 32 64)


# POWERS_OF_2=(2)

for clusters in "${POWERS_OF_2[@]}"; do
    CONFIGS+=("${clusters}C_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles")
done

TOTAL_CONFIGS=${#CONFIGS[@]}
SUCCESS_COUNT=0
FAIL_COUNT=0
START_TIME=$(date +%s)

# On-disk tree these CSVs land in — needed to COUNT what actually landed per NoC, which is the
# honest progress number (independent of exit codes).
CLOCK_SUBDIR="$(clock_subdir_for "$TIMER_MODE" "$SHUFFLE_FLAG" "$JSMAP_BUF_MB")"
CLOCK_LABEL="$(clock_label_for "$TIMER_MODE" "$SHUFFLE_FLAG")"
DATA_TREE="$SCRIPT_DIR/data/$CLOCK_SUBDIR"

# Expected CSVs per NoC = classes x iterations. Both live in batch_runner.sh, so read them from
# there rather than hardcoding a second copy that could silently drift. A parse failure only
# costs the denominator, never the run.
ITERS_PER_CONFIG=$(sed -n 's/^TOTAL_ITERATIONS=\([0-9]\+\).*/\1/p' "$BATCH_RUNNER" | head -1)
if [ "$IS_WEBSITE" -eq 1 ]; then
    CLASS_COUNT="$NUM_SITES"; CLASS_NOUN="sites"
else
    CLASS_COUNT=$(sed -n 's/^NUM_STRESSORS=\([0-9]\+\).*/\1/p' "$BATCH_RUNNER" | head -1)
    CLASS_NOUN="stressors"
fi
if [[ "$CLASS_COUNT" =~ ^[0-9]+$ ]] && [[ "$ITERS_PER_CONFIG" =~ ^[0-9]+$ ]]; then
    EXPECTED_PER_CONFIG=$(( CLASS_COUNT * ITERS_PER_CONFIG ))
else
    EXPECTED_PER_CONFIG=0     # unknown -> the per-NoC line prints csvs=N with no denominator
fi

# One status log per sweep: the same stem as the .h5 this run will produce, plus a timestamp so
# a re-run never appends into the previous attempt's file.
STATUS_LOG_NAME="${CLOCK_LABEL}_${TST}TST_${K}K_${CYCLES_PER_ADDRESS}cycles_${JSMAP_BUF_MB}MB_$(date +%Y%m%d_%H%M%S).log"
if [ "$STATUS_ENABLE" = "1" ]; then
    echo "📡 Remote status log: ${REMOTE_USER:+$REMOTE_USER@}$REMOTE_HOST:$STATUS_REMOTE_DIR/$STATUS_LOG_NAME"
fi
status_push "=== SWEEP START $(date '+%Y-%m-%d %H:%M:%S')  host=$(hostname)  mode=$TIMER_MODE \
tree=$CLOCK_SUBDIR  classes=${CLASS_COUNT:-?} $CLASS_NOUN  iters/NoC=${ITERS_PER_CONFIG:-?}  \
NoCs=[${POWERS_OF_2[*]}]  TST=${TST}s K=$K cycles=$CYCLES_PER_ADDRESS"

for ((i=0; i<TOTAL_CONFIGS; i++)); do
    CONFIG="${CONFIGS[$i]}"
    CONFIG_NUM=$((i + 1))
    BATCH_LOG="$LOG_DIR/batch_${CONFIG}_$(date +%Y%m%d_%H%M%S).log"
    
    echo ""
    print_separator
    echo "🚀 [Config $CONFIG_NUM/$TOTAL_CONFIGS] Starting: $CONFIG"
    echo "   Timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
    print_separator
    
    # Refresh sudo credentials
    sudo -v 2>/dev/null
    
    # Run the batch_runner.sh
    # sudo resets the environment, so JSMAP_BUF_MB must be handed over explicitly via `env`;
    # otherwise batch_runner.sh would silently fall back to the 12 MB default.
    CMD="sudo env JSMAP_BUF_MB=$JSMAP_BUF_MB SITES_FILE=$SITES_FILE WEB_COOLDOWN_US=$WEB_COOLDOWN_US CHROME_UID=$CHROME_UID CHROME_GID=$CHROME_GID $BATCH_RUNNER $TIMER_MODE $SHUFFLE_FLAG $CONFIG"
    echo "   Command: $CMD"
    echo "   Output:  $BATCH_LOG"
    echo ""
    
    NOC_T0=$(date +%s)
    if eval "$CMD" > "$BATCH_LOG" 2>&1; then
        echo "✅ [Config $CONFIG_NUM/$TOTAL_CONFIGS] COMPLETED: $CONFIG"
        ((SUCCESS_COUNT++))
        NOC_STATUS="OK"
    else
        EXIT_CODE=$?
        echo "❌ [Config $CONFIG_NUM/$TOTAL_CONFIGS] FAILED: $CONFIG (exit code: $EXIT_CODE)"
        echo "   Log: $BATCH_LOG"
        ((FAIL_COUNT++))
        NOC_STATUS="FAILED"
    fi
    NOC_T1=$(date +%s)

    # What actually landed on disk for this NoC.
    NOC_CSVS=$(find "$DATA_TREE/$CONFIG" -name '*.csv' 2>/dev/null | wc -l)
    if [ "$EXPECTED_PER_CONFIG" -gt 0 ]; then
        CSV_FIELD="csvs=$NOC_CSVS/$EXPECTED_PER_CONFIG"
    else
        CSV_FIELD="csvs=$NOC_CSVS"
    fi
    # Push BEFORE the cooldown and before the next NoC, so the remote log can never report a
    # NoC as finished out of order.
    status_push "$(printf '[%d/%d] NoC=%-4s started %s  finished %s  duration %s  status=%-6s %s' \
        "$CONFIG_NUM" "$TOTAL_CONFIGS" "${POWERS_OF_2[$i]}" \
        "$(date -d "@$NOC_T0" '+%Y-%m-%d %H:%M:%S')" \
        "$(date -d "@$NOC_T1" '+%Y-%m-%d %H:%M:%S')" \
        "$(format_duration $((NOC_T1 - NOC_T0)))" \
        "$NOC_STATUS" "$CSV_FIELD")"
    
    # Cooldown between runs (except after the last one)
    if [ $((i + 1)) -lt $TOTAL_CONFIGS ]; then
        echo ""
        echo "⏳ Cooldown phase: ${COOLDOWN_SECS}s"
        for ((j=COOLDOWN_SECS; j>0; j--)); do
            sleep 1
            printf "\r   Waiting: $j seconds remaining..."
        done
        echo ""
    fi
done

# ============================================
# Summary
# ============================================
END_TIME=$(date +%s)
TOTAL_DURATION=$((END_TIME - START_TIME))
FORMATTED_DURATION=$(format_duration $TOTAL_DURATION)

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    EXECUTION COMPLETE                          ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Results Summary:"
echo "  Total Configs:       $TOTAL_CONFIGS"
echo "  Successful:          $SUCCESS_COUNT / $TOTAL_CONFIGS"
echo "  Failed:              $FAIL_COUNT / $TOTAL_CONFIGS"
echo "  Total Duration:      $FORMATTED_DURATION"
echo "  Logs Directory:      $LOG_DIR"
echo ""

status_push "=== SWEEP END   $(date '+%Y-%m-%d %H:%M:%S')  duration $FORMATTED_DURATION  \
ok=$SUCCESS_COUNT/$TOTAL_CONFIGS  $([ "$FAIL_COUNT" -eq 0 ] \
    && { [ "$DO_FINALIZE" = "0" ] && echo 'finalize SKIPPED (DO_FINALIZE=0), CSVs kept' || echo '-> finalizing'; } \
    || echo 'finalize SKIPPED, all CSVs preserved for retry')"

if [ $FAIL_COUNT -eq 0 ]; then
    echo "🎉 All configurations completed successfully!"

    # ============================================
    # Post-sweep: convert -> delete CSVs -> backup (only on a fully successful sweep)
    # ============================================
    if [ "$DO_FINALIZE" != "0" ]; then
        if [[ ! -x "$FINALIZER" ]]; then
            chmod +x "$FINALIZER" 2>/dev/null || true
        fi
        echo ""
        print_separator
        echo "🧩 Finalizing experiment (h5 + delete CSVs + backup)"
        print_separator
        if REMOTE_HOST="$REMOTE_HOST" REMOTE_USER="$REMOTE_USER" REMOTE_DIR="$REMOTE_DIR" \
           LOCAL_H5_DIR="$LOCAL_H5_DIR" PYTHON_BIN="$PYTHON_BIN" DRY_RUN="$DRY_RUN" \
           "$FINALIZER" "$TIMER_MODE" "$SHUFFLE_FLAG" "$TST" "$CYCLES_PER_ADDRESS" \
           "$JSMAP_BUF_MB" "$K" "${POWERS_OF_2[@]}"; then
            echo "✅ Finalize step completed."
        else
            echo "❌ Finalize step FAILED — CSVs preserved. Re-run finalize_experiment.sh manually."
            exit 1
        fi
    else
        echo "ℹ️  DO_FINALIZE=0 — skipping h5 conversion/backup; CSVs left in place."
    fi

    exit 0
else
    echo "⚠️  Some configurations failed. Review logs for details."
    exit 1
fi
