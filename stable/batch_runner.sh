#!/bin/bash

#set -e  # Exit on error

# ============================================
# Configuration
# ============================================
PROGRAM="./MastikElite"
BATCH_SIZE=5
TOTAL_ITERATIONS=50
COOLDOWN_SECS=10
OUTPUT_DIR="batch_logs"
DATA_DIR="data"  # Where the C program saves results

# ============================================
# Create directories
# ============================================
mkdir -p "$OUTPUT_DIR"
mkdir -p "$DATA_DIR"

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
# Main execution
# ============================================
NUM_BATCHES=$(( (TOTAL_ITERATIONS + BATCH_SIZE - 1) / BATCH_SIZE ))

echo ""
echo "🚀 BATCH EXECUTION SCHEDULER"
print_separator
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
    
    echo ""
    echo "📊 [Batch $batch/$NUM_BATCHES] Iterations $START_ITER-$((END_ITER-1))"
    echo "   System Health: $(check_system_health)"
    echo "   Running: sudo $PROGRAM $START_ITER $ACTUAL_BATCH_SIZE $DATA_DIR"
    print_separator
    
    # Run the program with batch parameters
    if sudo $PROGRAM $START_ITER $ACTUAL_BATCH_SIZE $DATA_DIR >> "$BATCH_LOG" 2>&1; then
        echo "✅ Batch $batch PASSED"
        ((SUCCESS_COUNT++))
    else
        echo "❌ Batch $batch FAILED (exit code: $?)"
        echo "    Log: $BATCH_LOG"
        ((FAIL_COUNT++))
        
        # Optional: Stop on first failure
        # echo "Stopping execution due to failure"
        # exit 1
    fi
    
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