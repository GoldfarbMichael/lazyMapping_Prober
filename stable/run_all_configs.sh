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
TIMER_MODE="-j"   # Chrome timer + JS-style lazy-map victim (data/chrome_clock_jsmap/...)
# TIMER_MODE="-jn"  # Native timer + JS-style lazy-map victim (data/native_clock_jsmap/...)
# TIMER_MODE="-jb"  # Chrome timer + JS lazy map BIDIRECTIONAL (data/chrome_clock_jsmap_bidir/...)
# TIMER_MODE="-jnb" # Native timer + JS lazy map BIDIRECTIONAL (data/native_clock_jsmap_bidir/...)
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
TST=2
# Victim buffer size in MB for the jsmap modes (-j/-jn/-jb/-jnb). Must be a multiple of 12.
# 12 (default) = one LLC = mean 12 lines/set; 24 = mean 24 lines/set. Non-default sizes write to
# their own tree (data/<clock>_jsmap[_bidir]_<N>MB/), so 12 MB data is never overwritten.
# Overridable from the environment: JSMAP_BUF_MB=24 ./run_all_configs.sh
JSMAP_BUF_MB="${JSMAP_BUF_MB:-12}"
COOLDOWN_SECS=60
LOG_DIR="$SCRIPT_DIR/batch_logs"

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
if [[ "$TIMER_MODE" == "-j" || "$TIMER_MODE" == "-jn" \
   || "$TIMER_MODE" == "-jb" || "$TIMER_MODE" == "-jnb" ]]; then
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
if [ "$IS_JSMAP" -eq 0 ] && [ "$JSMAP_BUF_MB" != 12 ]; then
    echo "⚠️  JSMAP_BUF_MB=$JSMAP_BUF_MB has no effect with TIMER_MODE=$TIMER_MODE (no lazy map)."
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
POWERS_OF_2=(32)

# POWERS_OF_2=(2)

for clusters in "${POWERS_OF_2[@]}"; do
    CONFIGS+=("${clusters}C_${TST}TST_90K_${CYCLES_PER_ADDRESS}cycles")
done

TOTAL_CONFIGS=${#CONFIGS[@]}
SUCCESS_COUNT=0
FAIL_COUNT=0
START_TIME=$(date +%s)

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
    CMD="sudo env JSMAP_BUF_MB=$JSMAP_BUF_MB $BATCH_RUNNER $TIMER_MODE $SHUFFLE_FLAG $CONFIG"
    echo "   Command: $CMD"
    echo "   Output:  $BATCH_LOG"
    echo ""
    
    if eval "$CMD" > "$BATCH_LOG" 2>&1; then
        echo "✅ [Config $CONFIG_NUM/$TOTAL_CONFIGS] COMPLETED: $CONFIG"
        ((SUCCESS_COUNT++))
    else
        EXIT_CODE=$?
        echo "❌ [Config $CONFIG_NUM/$TOTAL_CONFIGS] FAILED: $CONFIG (exit code: $EXIT_CODE)"
        echo "   Log: $BATCH_LOG"
        ((FAIL_COUNT++))
    fi
    
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
           "$JSMAP_BUF_MB" "${POWERS_OF_2[@]}"; then
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
