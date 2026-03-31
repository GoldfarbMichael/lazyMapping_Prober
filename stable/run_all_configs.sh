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
TIMER_MODE="-c"  # Chrome timer
COOLDOWN_SECS=60
LOG_DIR="$SCRIPT_DIR/batch_logs"

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
echo "║     SEQUENTIAL BATCH RUNNER FOR ALL CORE COUNTS (1-512)        ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Configuration:"
echo "  Script Directory:  $SCRIPT_DIR"
echo "  Batch Runner:      $BATCH_RUNNER"
echo "  Timer Mode:        $TIMER_MODE (Chrome Mock)"
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
POWERS_OF_2=(1 2 4 8 16 32 64 128 256 512)

for cores in "${POWERS_OF_2[@]}"; do
    CONFIGS+=("${cores}C_2TST_DynamicSST")
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
    CMD="sudo $BATCH_RUNNER $TIMER_MODE $CONFIG"
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
    exit 0
else
    echo "⚠️  Some configurations failed. Review logs for details."
    exit 1
fi
