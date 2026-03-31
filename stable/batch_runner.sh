#!/bin/bash
trap 'sudo pkill -9 stress-ng 2>/dev/null; sudo pkill -9 MastikElite 2>/dev/null' EXIT
#set -e  # Exit on error

# ============================================
# Configuration
# ============================================
PROGRAM="./MastikElite"
TIMER_MODE="-n"  # Default: -n (native), can be -c (chrome)
CONFIG_DIR=""    # Will be set from command-line argument
BATCH_SIZE=1
TOTAL_ITERATIONS=50
COOLDOWN_SECS=10
OUTPUT_DIR="batch_logs"

# ============================================
# Parse command-line arguments
# ============================================
if [[ $# -eq 0 ]]; then
    echo "❌ Missing required argument: CONFIG_DIR"
    echo ""
    echo "Usage: $0 [-c|-n] CONFIG_DIR"
    echo ""
    echo "Options:"
    echo "  -c              : Use Chrome mock timer (jittered, 100us clamped)"
    echo "  -n              : Use native rdtscp64 timer (default)"
    echo ""
    echo "Arguments:"
    echo "  CONFIG_DIR      : Configuration directory name (e.g., '16C_15TST_DynamicSST')"
    echo ""
    echo "Examples:"
    echo "  $0 16C_15TST_DynamicSST              # Chrome timer (default -c)"
    echo "  $0 -n 16C_15TST_DynamicSST           # Native timer"
    echo "  $0 -c 64C_15TST_DynamicSST           # Chrome timer with specific config"
    echo ""
    exit 1
fi

# Parse timer mode flag if present
if [[ "$1" == "-c" ]]; then
    TIMER_MODE="-c"
    echo "Timer Mode set to: Chrome Mock (-c)"
    shift  # Move to next argument
elif [[ "$1" == "-n" ]]; then
    TIMER_MODE="-n"
    echo "Timer Mode set to: Native rdtscp64 (-n)"
    shift  # Move to next argument
elif [[ "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $0 [-c|-n] CONFIG_DIR"
    echo ""
    echo "Options:"
    echo "  -c              : Use Chrome mock timer (jittered, 100us clamped)"
    echo "  -n              : Use native rdtscp64 timer (default)"
    echo "  -h, --help      : Show this help message"
    echo ""
    echo "Arguments:"
    echo "  CONFIG_DIR      : Configuration directory name (e.g., '16C_15TST_DynamicSST')"
    echo ""
    exit 0
fi

# CONFIG_DIR should be the next argument (or first if no flag)
if [[ -z "$1" ]]; then
    echo "❌ Missing required argument: CONFIG_DIR"
    echo "Usage: $0 [-c|-n] CONFIG_DIR"
    echo ""
    exit 1
fi

CONFIG_DIR="$1"
echo "Configuration Directory: $CONFIG_DIR"

# ============================================
# Create directories
# ============================================
mkdir -p "$OUTPUT_DIR"
TIMER_SUBDIR=$([ "$TIMER_MODE" = "-c" ] && echo "chrome_clock" || echo "native_clock")
mkdir -p "data/$TIMER_SUBDIR/$CONFIG_DIR"  # Ensure config-specific data directory exists

# ============================================
# Helper functions
# ============================================
check_system_health() {
    local mem_used=$(free | awk '/^Mem:/ {printf "%.1f", $3/$2 * 100}')
    local load=$(uptime | awk -F'load average:' '{print $2}' | cut -d, -f1 | xargs)
    
    # Check if load is concerning for 2-pinned-core workload
    if (( $(echo "$load > 4" | bc -l) )); then
        echo "Memory: ${mem_used}% | Load: $load ⚠️ HIGH"
    else
        echo "Memory: ${mem_used}% | Load: $load ✓"
    fi
}

print_separator() {
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# ============================================
# Refresh sudo credentials
# ============================================
echo "🔐 Refreshing sudo credentials..."
sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Failed to authenticate with sudo. Exiting."
    exit 1
fi
echo "✅ Sudo credentials refreshed"
echo ""

# ============================================
# Main execution
# ============================================
NUM_BATCHES=$(( (TOTAL_ITERATIONS + BATCH_SIZE - 1) / BATCH_SIZE ))

echo ""
echo "🚀 BATCH EXECUTION SCHEDULER"
print_separator
echo "Timer Mode:      $TIMER_MODE ($([ "$TIMER_MODE" == "-c" ] && echo "Chrome Mock" || echo "Native rdtscp64"))"
echo "Config Directory: $CONFIG_DIR"
echo "Total Iterations: $TOTAL_ITERATIONS"
echo "Batch Size:      $BATCH_SIZE"
echo "Total Batches:   $NUM_BATCHES"
echo "Cooldown (sec):  $COOLDOWN_SECS"
print_separator
echo ""

SUCCESS_COUNT=0
FAIL_COUNT=0

for ((batch=1; batch<=NUM_BATCHES; batch++)); do
    START_ITER=$(( (batch - 1) * BATCH_SIZE ))
    END_ITER=$(( START_ITER + BATCH_SIZE ))
    if [ $END_ITER -gt $TOTAL_ITERATIONS ]; then
        END_ITER=$TOTAL_ITERATIONS
    fi
    
    ACTUAL_BATCH_SIZE=$(( END_ITER - START_ITER ))
    BATCH_LOG="$OUTPUT_DIR/batch_${batch}.log"
    sudo -v 2>/dev/null
    
    echo ""
    echo "📊 [Batch $batch/$NUM_BATCHES] Iterations $START_ITER-$((END_ITER-1))"
    echo "   Start Time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "   System Health: $(check_system_health)"
    echo "   Running: sudo $PROGRAM $TIMER_MODE $START_ITER $ACTUAL_BATCH_SIZE $CONFIG_DIR"
    print_separator
    
    # Run the program with timeout (6:30 min = 390 seconds)
    BATCH_SUCCESS=false
    RETRY_COUNT=0
    MAX_RETRIES=3
    
    while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
        sudo timeout 450 $PROGRAM $TIMER_MODE $START_ITER $ACTUAL_BATCH_SIZE $CONFIG_DIR >> "$BATCH_LOG" 2>&1
        EXIT_CODE=$?
        
        if [ $EXIT_CODE -eq 124 ]; then
            # Timeout occurred (exit code 124 is timeout)
            ((RETRY_COUNT++))
            echo "[TIMEOUT] Batch $batch timed out after 6:30 min - restarting iteration ($RETRY_COUNT/$MAX_RETRIES)"
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] TIMEOUT: Iteration $START_ITER-$((END_ITER-1)) timed out (attempt $RETRY_COUNT)" >> "$BATCH_LOG"
            sudo pkill -9 stress-ng 2>/dev/null
            
            if [ $RETRY_COUNT -ge $MAX_RETRIES ]; then
                echo "❌ Batch $batch FAILED (exceeded max retries after timeout)"
                ((FAIL_COUNT++))
                break
            fi
        elif [ $EXIT_CODE -eq 0 ]; then
            # Success
            if [ $RETRY_COUNT -gt 0 ]; then
                echo "✅ Batch $batch PASSED (succeeded on attempt $((RETRY_COUNT+1)))"
            else
                echo "✅ Batch $batch PASSED"
            fi
            ((SUCCESS_COUNT++))
            BATCH_SUCCESS=true
            break
        else
            # Other failure
            echo "❌ Batch $batch FAILED (exit code: $EXIT_CODE)"
            echo "    Log: $BATCH_LOG"
            ((FAIL_COUNT++))
            break
        fi
    done
    
    # Cooldown between batches
    if [ $batch -lt $NUM_BATCHES ]; then
        echo ""
        echo "⏳ Cooldown phase: ${COOLDOWN_SECS}s"
        for ((i=COOLDOWN_SECS; i>0; i--)); do
            sleep 1
            printf "\r   Waiting: $i seconds remaining..."
        done
        echo ""
    fi
done

# ============================================
# Summary
# ============================================
echo ""
print_separator
echo "✨ EXECUTION COMPLETE"
print_separator
echo "Timer Mode:         $TIMER_MODE ($([ "$TIMER_MODE" == "-c" ] && echo "Chrome Mock" || echo "Native rdtscp64"))"
echo "Config Directory:   $CONFIG_DIR"
echo "Successful Batches: $SUCCESS_COUNT / $NUM_BATCHES"
echo "Failed Batches:     $FAIL_COUNT / $NUM_BATCHES"
echo "Logs saved to:      $OUTPUT_DIR/"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo "🎉 All batches completed successfully!"
    exit 0
else
    echo "⚠️  Some batches failed. Review logs for details."
    exit 1
fi