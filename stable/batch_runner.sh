#!/bin/bash
trap 'sudo pkill -9 stress-ng 2>/dev/null; sudo pkill -9 MastikElite 2>/dev/null; sudo pkill -9 -f "user-data-dir=/tmp/chrome-website-nativ[e]" 2>/dev/null' EXIT
#set -e  # Exit on error

# ============================================
# Configuration
# ============================================
PROGRAM="./MastikElite"
TIMER_MODE="-n"  # Default: -n (native), can be -c (chrome)
CONFIG_DIR=""    # Will be set from command-line argument
BATCH_SIZE=50
TOTAL_ITERATIONS=50
COOLDOWN_SECS=10
OUTPUT_DIR="batch_logs"

# --- Per-run timeout is DERIVED from the workload (below), never a fixed value, so a
# healthy long run is never killed while a genuinely hung process is still recovered.
# These mirror the C program's inner cost in runStressNG_batches():
NUM_STRESSORS=38       # entries in stress_battery[] (mastikElite.c) — keep in sync
PER_SAMPLE_OVERHEAD=3  # per-sample cost ON TOP of TST: 1s cooldown + 50ms steady state + CSV write
# Website modes (-wn/-wc) replace the 38 stressors with the site list, and each sample costs
# more than a stress-ng one (CDP tab open + renderer teardown on top of the cooldown). Both are
# resolved below, after the flags are parsed.
WEB_PER_SAMPLE_OVERHEAD=4
SITES_FILE="${SITES_FILE:-sites.txt}"
INTER_ROUND_SECS=8     # sleep() between rounds in runStressNG_batches
DEFAULT_TST_SECS=2     # TST_SEC in mastikElite.h; used only if the label has no {N}TST field
# PER_SAMPLE_SECS is DERIVED from the config label's TST field below (the C tool parses the same
# field via parse_TST_from_dirname), so a TST=4 run widens the kill timeout automatically instead
# of tripping a timeout sized for TST=2.
TIMEOUT_SAFETY_PCT=140 # allow 140% of the estimate (40% headroom)

# ============================================
# Parse command-line arguments
# ============================================
if [[ $# -eq 0 ]]; then
    echo "❌ Missing required argument: CONFIG_DIR"
    echo ""
    echo "Usage: $0 [-c|-n|-j|-jn|-jb|-jnb|-jss|-jssb|-jnss|-jnssb] [-s] CONFIG_DIR"
    echo ""
    echo "Options:"
    echo "  -c              : Use Chrome mock timer (jittered, 100us clamped)"
    echo "  -n              : Use native rdtscp64 timer (default)"
    echo "  -j              : Use Chrome mock timer with the JS-style lazy-map victim"
    echo "  -jn             : Use native rdtscp64 timer with the JS-style lazy-map victim"
    echo "  -jb             : -j + BIDIRECTIONAL (Mastik double-sided) sweep"
    echo "  -jnb            : -jn + BIDIRECTIONAL (Mastik double-sided) sweep"
    echo "  -jss            : Chrome mock timer + JS lazy map SINGLE-SWEEP (idle-fill)"
    echo "  -jssb           : -jss + BIDIRECTIONAL single sweep"
    echo "  -jnss           : Native rdtscp64 timer + JS lazy map SINGLE-SWEEP (idle-fill)"
    echo "  -jnssb          : -jnss + BIDIRECTIONAL single sweep"
    echo "  -wn             : Native timer + Mastik clusters + REAL-WEBSITE victim"
    echo "  -wc             : Chrome timer + Mastik clusters + REAL-WEBSITE victim"
    echo ""
    echo "Arguments:"
    echo "  CONFIG_DIR      : Configuration directory name (e.g., '16C_2TST_90K_2288cycles')"
    echo ""
    echo "Examples:"
    echo "  $0 -n 16C_2TST_90K_300cycles          # Native timer, Mastik e-sets"
    echo "  $0 -c 64C_2TST_90K_2288cycles         # Chrome timer, Mastik e-sets"
    echo "  $0 -jn 16C_2TST_90K_2288cycles        # Native timer, JS-style lazy map"
    echo "  $0 -jnb 16C_2TST_90K_2288cycles       # Native timer, JS lazy map, bidirectional"
    echo ""
    exit 1
fi

# Parse leading flags (timer mode + optional -s shuffle), in any order, until CONFIG_DIR.
SHUFFLE_FLAG=""   # "-s" line-shuffles the Mastik clusters once (only meaningful with -c)
while [[ "$1" == -* ]]; do
    case "$1" in
        -c) TIMER_MODE="-c"; echo "Timer Mode set to: Chrome Mock (-c)"; shift ;;
        -n) TIMER_MODE="-n"; echo "Timer Mode set to: Native rdtscp64 (-n)"; shift ;;
        -j) TIMER_MODE="-j"; echo "Timer Mode set to: Chrome Mock + JS-style lazy map (-j)"; shift ;;
        -jn) TIMER_MODE="-jn"; echo "Timer Mode set to: Native rdtscp64 + JS-style lazy map (-jn)"; shift ;;
        -jb) TIMER_MODE="-jb"; echo "Timer Mode set to: Chrome Mock + JS-style lazy map BIDIRECTIONAL (-jb)"; shift ;;
        -jnb) TIMER_MODE="-jnb"; echo "Timer Mode set to: Native rdtscp64 + JS-style lazy map BIDIRECTIONAL (-jnb)"; shift ;;
        -jss) TIMER_MODE="-jss"; echo "Timer Mode set to: Chrome Mock + JS-style lazy map SINGLE-SWEEP (-jss)"; shift ;;
        -jssb) TIMER_MODE="-jssb"; echo "Timer Mode set to: Chrome Mock + JS-style lazy map SINGLE-SWEEP BIDIRECTIONAL (-jssb)"; shift ;;
        -jnss) TIMER_MODE="-jnss"; echo "Timer Mode set to: Native rdtscp64 + JS-style lazy map SINGLE-SWEEP (-jnss)"; shift ;;
        -jnssb) TIMER_MODE="-jnssb"; echo "Timer Mode set to: Native rdtscp64 + JS-style lazy map SINGLE-SWEEP BIDIRECTIONAL (-jnssb)"; shift ;;
        -wn) TIMER_MODE="-wn"; echo "Timer Mode set to: Native rdtscp64 + Mastik clusters + WEBSITE victim (-wn)"; shift ;;
        -wc) TIMER_MODE="-wc"; echo "Timer Mode set to: Chrome Mock + Mastik clusters + WEBSITE victim (-wc)"; shift ;;
        -s) SHUFFLE_FLAG="-s"; echo "Cluster shuffle: ON (-s; effective only with -c)"; shift ;;
        -h|--help)
            echo "Usage: $0 [-c|-n|-j|-jn|-jb|-jnb|-jss|-jssb|-jnss|-jnssb] [-s] CONFIG_DIR"
            echo ""
            echo "Options:"
            echo "  -c              : Use Chrome mock timer (jittered, 100us clamped)"
            echo "  -n              : Use native rdtscp64 timer (default)"
            echo "  -j              : Use Chrome mock timer with the JS-style lazy-map victim"
            echo "  -jn             : Use native rdtscp64 timer with the JS-style lazy-map victim"
            echo "                    (-> data/native_clock_jsmap/)"
            echo "  -jb             : -j + BIDIRECTIONAL sweep (-> data/chrome_clock_jsmap_bidir/)"
            echo "  -jnb            : -jn + BIDIRECTIONAL sweep (-> data/native_clock_jsmap_bidir/)"
            echo "  -jss            : Chrome mock + JS lazy map SINGLE-SWEEP (-> data/chrome_clock_jsmapSS/)"
            echo "  -jssb           : -jss + BIDIRECTIONAL (-> data/chrome_clock_jsmapSS_bidir/)"
            echo "  -jnss           : Native + JS lazy map SINGLE-SWEEP (-> data/native_clock_jsmapSS/)"
            echo "  -jnssb          : -jnss + BIDIRECTIONAL (-> data/native_clock_jsmapSS_bidir/)"
            echo "  -wn             : Native timer, Mastik clusters, WEBSITE victim"
            echo "                    (-> data/native_clock_website/)"
            echo "  -wc             : Chrome timer, Mastik clusters, WEBSITE victim"
            echo "                    (-> data/chrome_clock_website/)"
            echo "  -s              : Line-shuffle the Mastik clusters once (only with -c;"
            echo "                    -> data/chrome_clock_shuffled/)"
            echo "  -h, --help      : Show this help message"
            echo ""
            echo "Arguments:"
            echo "  CONFIG_DIR      : Configuration directory name (e.g., '16C_2TST_90K_2288cycles')"
            echo ""
            exit 0 ;;
        *) echo "❌ Unknown flag: $1"; echo "Usage: $0 [-c|-n|-j|-jn|-wn|-wc] [-s] CONFIG_DIR"; exit 1 ;;
    esac
done

# CONFIG_DIR should be the next argument (or first if no flag)
if [[ -z "$1" ]]; then
    echo "❌ Missing required argument: CONFIG_DIR"
    echo "Usage: $0 [-c|-n] CONFIG_DIR"
    echo ""
    exit 1
fi

CONFIG_DIR="$1"
echo "Configuration Directory: $CONFIG_DIR"

# Total sampling time per trace, read from the label's "{N}TST" field — the same field
# parse_TST_from_dirname() feeds to the C sampler. Keeps the timeout estimate honest for any TST.
TST_SECS="$(sed -n 's/.*[^0-9]\([0-9]\+\)TST.*/\1/p; s/^\([0-9]\+\)TST.*/\1/p' <<< "$CONFIG_DIR" | head -1)"
if ! [[ "$TST_SECS" =~ ^[0-9]+$ ]] || [ "$TST_SECS" -le 0 ]; then
    echo "⚠️  No {N}TST field in '$CONFIG_DIR'; assuming TST=${DEFAULT_TST_SECS}s for the timeout estimate"
    TST_SECS=$DEFAULT_TST_SECS
fi
# Class count and per-sample budget depend on the victim: stress_battery[] for the stress-ng
# modes, the sites file for the website modes. Getting this wrong only mis-sizes the hang
# timeout, but a timeout sized for 38 classes would kill a healthy 100-site run outright.
case "$TIMER_MODE" in
    -wn|-wc)
        if [[ ! -f "$SITES_FILE" ]]; then
            echo "❌ $TIMER_MODE needs a sites file; '$SITES_FILE' not found" >&2
            echo "   Set SITES_FILE=/path/to/sites.txt" >&2
            exit 2
        fi
        NUM_CLASSES=$(grep -cE '^[[:space:]]*[^#[:space:]]' "$SITES_FILE" || true)
        if ! [[ "$NUM_CLASSES" =~ ^[0-9]+$ ]] || [ "$NUM_CLASSES" -lt 2 ]; then
            echo "❌ '$SITES_FILE' has ${NUM_CLASSES:-0} uncommented site(s); need at least 2" >&2
            exit 2
        fi
        PER_SAMPLE_SECS=$(( TST_SECS + WEB_PER_SAMPLE_OVERHEAD ))
        echo "Sites: $NUM_CLASSES from $SITES_FILE"
        ;;
    *)
        NUM_CLASSES=$NUM_STRESSORS
        PER_SAMPLE_SECS=$(( TST_SECS + PER_SAMPLE_OVERHEAD ))
        ;;
esac
echo "TST: ${TST_SECS}s  ->  per-sample budget ${PER_SAMPLE_SECS}s over $NUM_CLASSES classes"

# ============================================
# Create directories
# ============================================
mkdir -p "$OUTPUT_DIR"
case "$TIMER_MODE" in
    -c)  TIMER_SUBDIR="chrome_clock" ;;
    -j)  TIMER_SUBDIR="chrome_clock_jsmap" ;;
    -jn) TIMER_SUBDIR="native_clock_jsmap" ;;
    -jb) TIMER_SUBDIR="chrome_clock_jsmap_bidir" ;;
    -jnb) TIMER_SUBDIR="native_clock_jsmap_bidir" ;;
    -jss) TIMER_SUBDIR="chrome_clock_jsmapSS" ;;
    -jssb) TIMER_SUBDIR="chrome_clock_jsmapSS_bidir" ;;
    -jnss) TIMER_SUBDIR="native_clock_jsmapSS" ;;
    -jnssb) TIMER_SUBDIR="native_clock_jsmapSS_bidir" ;;
    -wn) TIMER_SUBDIR="native_clock_website" ;;
    -wc) TIMER_SUBDIR="chrome_clock_website" ;;
    *)   TIMER_SUBDIR="native_clock" ;;
esac
# Shuffled Mastik e-set runs go to a distinct tree (must match the C tool's output path).
if [[ -n "$SHUFFLE_FLAG" && "$TIMER_MODE" == "-c" ]]; then
    TIMER_SUBDIR="chrome_clock_shuffled"
fi
# Victim buffer size (jsmap modes only), forwarded to the C tool via JSMAP_BUF_MB. Non-default
# sizes get their own tree (_<N>MB) so a 24 MB run never overwrites 12 MB data. Must mirror
# timer_mode_subdir() in mastikElite.c exactly.
JSMAP_BUF_MB="${JSMAP_BUF_MB:-12}"
if ! [[ "$JSMAP_BUF_MB" =~ ^[0-9]+$ ]] || [ "$JSMAP_BUF_MB" -lt 12 ] || [ $((JSMAP_BUF_MB % 12)) -ne 0 ]; then
    echo "❌ JSMAP_BUF_MB must be a multiple of 12 (>=12), got '$JSMAP_BUF_MB'" >&2
    exit 2
fi
case "$TIMER_MODE" in
    -j|-jn|-jb|-jnb|-jss|-jssb|-jnss|-jnssb)
        if [ "$JSMAP_BUF_MB" != 12 ]; then TIMER_SUBDIR="${TIMER_SUBDIR}_${JSMAP_BUF_MB}MB"; fi ;;
    -wn|-wc) : ;;   # Mastik clusters, no lazy map: the buffer knob does not apply
    *)
        if [ "$JSMAP_BUF_MB" != 12 ]; then
            echo "⚠️  JSMAP_BUF_MB=$JSMAP_BUF_MB ignored: $TIMER_MODE does not use the lazy map" >&2
        fi ;;
esac
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
echo "Timer Mode:      $TIMER_MODE ($TIMER_SUBDIR)"
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

    # Derive a generous timeout from the actual work this invocation performs:
    #   ACTUAL_BATCH_SIZE rounds, each = NUM_STRESSORS samples + one inter-round cooldown.
    EST_SECS=$(( ACTUAL_BATCH_SIZE * (NUM_CLASSES * PER_SAMPLE_SECS + INTER_ROUND_SECS) ))
    TIMEOUT_SECS=$(( EST_SECS * TIMEOUT_SAFETY_PCT / 100 ))

    echo ""
    echo "📊 [Batch $batch/$NUM_BATCHES] Iterations $START_ITER-$((END_ITER-1))"
    echo "   Start Time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "   System Health: $(check_system_health)"
    echo "   Estimated runtime: ~${EST_SECS}s | Kill timeout: ${TIMEOUT_SECS}s"
    echo "   Running: sudo env JSMAP_BUF_MB=$JSMAP_BUF_MB SITES_FILE=$SITES_FILE${WEB_COOLDOWN_US:+ WEB_COOLDOWN_US=$WEB_COOLDOWN_US}${CHROME_UID:+ CHROME_UID=$CHROME_UID}${CHROME_GID:+ CHROME_GID=$CHROME_GID} $PROGRAM $TIMER_MODE $SHUFFLE_FLAG $START_ITER $ACTUAL_BATCH_SIZE $CONFIG_DIR"
    print_separator

    # Run the program with a workload-derived timeout (hang recovery only; never trims a healthy run).
    BATCH_SUCCESS=false
    RETRY_COUNT=0
    MAX_RETRIES=3

    while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
        # sudo resets the environment, so JSMAP_BUF_MB must be passed explicitly via `env`.
        # CHROME_UID/CHROME_GID must be relayed explicitly: this script already runs as root,
        # so the sudo below sets SUDO_UID=0 and the C tool could not otherwise tell who to drop
        # Chrome to. Only forwarded when non-empty, so a direct `./batch_runner.sh` run (single
        # sudo, SUDO_UID correct) keeps working unchanged.
        sudo env "JSMAP_BUF_MB=$JSMAP_BUF_MB" "SITES_FILE=$SITES_FILE" \
             ${WEB_COOLDOWN_US:+"WEB_COOLDOWN_US=$WEB_COOLDOWN_US"} \
             ${CHROME_UID:+"CHROME_UID=$CHROME_UID"} ${CHROME_GID:+"CHROME_GID=$CHROME_GID"} \
             timeout "$TIMEOUT_SECS" $PROGRAM $TIMER_MODE $SHUFFLE_FLAG $START_ITER $ACTUAL_BATCH_SIZE $CONFIG_DIR >> "$BATCH_LOG" 2>&1
        EXIT_CODE=$?

        if [ $EXIT_CODE -eq 124 ]; then
            # Timeout occurred (exit code 124 is timeout)
            ((RETRY_COUNT++))
            echo "[TIMEOUT] Batch $batch exceeded ${TIMEOUT_SECS}s (est ~${EST_SECS}s) - restarting iteration ($RETRY_COUNT/$MAX_RETRIES)"
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] TIMEOUT: Iteration $START_ITER-$((END_ITER-1)) exceeded ${TIMEOUT_SECS}s (attempt $RETRY_COUNT)" >> "$BATCH_LOG"
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
echo "Timer Mode:         $TIMER_MODE ($TIMER_SUBDIR)"
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