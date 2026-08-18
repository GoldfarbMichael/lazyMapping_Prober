#!/usr/bin/env bash
# Direct PMU measurement of SELF-EVICTION in the lazy-map victim, swept across NoC.
# For each cluster the CoverageValidator `selfevict` mode flushes the cluster, warms it
# with one sweep, then measures demand-load L3 misses on a second sweep via a programmable
# PMU counter (MEM_LOAD_RETIRED.L3_MISS, 0xD1/0x20). M_self/nodeCount ~ 0 means the sweep
# kept its lines resident; a large fraction means the cluster's same-set lines self-evicted
# under the scan-resistant L3 insertion policy. Prediction: ~0 at low NoC, rising toward
# NoC=64, tracking (1 - coverage). See docs/FINDINGS_coverage_insertion_policy.md sec 2.
#
# Unlike native/jsmap coverage, selfevict has NO attacker: it loads no mapping_A/B and needs
# no hugepages. It DOES need the PMU prerequisites below (root: wrmsr + rdpmc + nmi_watchdog).
#
# Usage: ./run_selfevict.sh <iterations_per_noc> [noc]
#   <iterations_per_noc>  runs per NoC (required, >=1)
#   [noc]                 optional: run ONLY this NoC (else sweep ALL_NOCS).
# The tree is auto-tagged with the current prefetcher mask (MSR 0x1a4) and buffer size, e.g.
# selfevict_shuffled_pref0x2_24MB. Set the MSR (wrmsr -a 0x1a4 <mask>) before running to control it.
# Env:
#   SHUFFLE   (default 1) 1=page-shuffled victim -> selfevict_shuffled/ ; 0=in-order -> selfevict/
#   WARMPASSES(default 1) warm sweeps (incl. the cold one) before the measured pass. >1 tests
#                         the sec-4 steady-state recovery (M_self should fall as WARMPASSES rises).
#   JSMAP_BUF_MB (default 12) victim buffer in MB, a multiple of 12 (24 -> mean 24 lines/set).
#                         Tagged _<MB>MB (omitted at 12).
#   BIDIR_R   (default 0 = off) >0 selects the bidirectional (Mastik double-sided) sweep for
#                         every cold/warm/measured pass instead of the plain pointer chase, with
#                         R fwd+bwd oscillations. Tagged _bidirR<n>, e.g. selfevict_shuffled_bidirR2_pref0x0.
#
# Run as your normal user; it sudo's for the PMU setup and each measured run. Existing outputs
# are skipped so re-running resumes.
set -uo pipefail

cd "$(dirname "$0")"   # stable/

ITERS="${1:-}"
if ! [[ "$ITERS" =~ ^[0-9]+$ ]] || [ "$ITERS" -lt 1 ]; then
    echo "usage: $0 <iterations_per_noc> [noc]" >&2
    exit 2
fi
ONLY_NOC="${2:-}"

SHUFFLE="${SHUFFLE:-1}"
if [ "$SHUFFLE" != 0 ] && [ "$SHUFFLE" != 1 ]; then
    echo "SHUFFLE must be 0 or 1, got '$SHUFFLE'" >&2; exit 2
fi
if [ "$SHUFFLE" = 1 ]; then SHUFFLE_TOKEN="shuffle"; else SHUFFLE_TOKEN="noshuffle"; fi

WARMPASSES="${WARMPASSES:-10}"
if ! [[ "$WARMPASSES" =~ ^[0-9]+$ ]] || [ "$WARMPASSES" -lt 1 ]; then
    echo "WARMPASSES must be an integer >= 1, got '$WARMPASSES'" >&2; exit 2
fi

# Victim buffer size in MB (default 12 = one LLC = mean 12 lines/set); must be a multiple of 12.
# Tagged _<MB>MB (omitted at 12); forwarded to the C tool via JSMAP_BUF_MB.
JSMAP_BUF_MB="${JSMAP_BUF_MB:-12}"
if ! [[ "$JSMAP_BUF_MB" =~ ^[0-9]+$ ]] || [ "$JSMAP_BUF_MB" -lt 12 ] || [ $((JSMAP_BUF_MB % 12)) -ne 0 ]; then
    echo "JSMAP_BUF_MB must be a multiple of 12 (>=12), got '$JSMAP_BUF_MB'" >&2; exit 2
fi
if [ "$JSMAP_BUF_MB" = 12 ]; then MB_SUFFIX=""; else MB_SUFFIX="_${JSMAP_BUF_MB}MB"; fi

BIDIR_R="${BIDIR_R:-0}"
if ! [[ "$BIDIR_R" =~ ^[0-9]+$ ]]; then
    echo "BIDIR_R must be a non-negative integer, got '$BIDIR_R'" >&2; exit 2
fi
if [ "$BIDIR_R" -gt 0 ]; then BIDIR_TAG="_bidirR${BIDIR_R}"; else BIDIR_TAG=""; fi

# Prefetcher-state tag: read MSR 0x1a4 (set bit DISABLES a prefetcher; bit1=L2 adjacent-line) on
# every logical CPU. If all agree, tag the tree _pref0x<val> so prefetch conditions never collide.
if ! command -v rdmsr >/dev/null 2>&1; then
    echo "[selfevict] ERROR: rdmsr not found (install msr-tools) -- cannot tag prefetcher state" >&2; exit 1
fi
sudo modprobe msr 2>/dev/null || true
mapfile -t PREF_MSR < <(sudo rdmsr -a 0x1a4 2>/dev/null)
PREF_VAL="$(printf '%s\n' "${PREF_MSR[@]}" | sort -u)"
if [ "${#PREF_MSR[@]}" -eq 0 ] || [ -z "$PREF_VAL" ]; then
    echo "[selfevict] ERROR: could not read MSR 0x1a4 (need sudo + msr module loaded)" >&2; exit 1
fi
if [ "$(printf '%s\n' "$PREF_VAL" | wc -l)" -ne 1 ]; then
    echo "[selfevict] ERROR: MSR 0x1a4 is not uniform across cores -- prefetcher state is mixed:" >&2
    printf '%s\n' "${PREF_MSR[@]}" | sort | uniq -c >&2; exit 1
fi
PREF_SUFFIX="_pref0x${PREF_VAL}"

OUT_ROOT="data/coverage/$([ "$SHUFFLE" = 1 ] && echo selfevict_shuffled || echo selfevict)${BIDIR_TAG}${PREF_SUFFIX}${MB_SUFFIX}"

ALL_NOCS=(2 4 8 16 32 64)
if [ -n "$ONLY_NOC" ]; then
    valid=0; for n in "${ALL_NOCS[@]}"; do [ "$n" = "$ONLY_NOC" ] && valid=1; done
    [ "$valid" -eq 1 ] || { echo "noc must be one of: ${ALL_NOCS[*]}" >&2; exit 2; }
    NOCS=("$ONLY_NOC")
else
    NOCS=("${ALL_NOCS[@]}")
fi

# ---- pre-flight ----
echo "[selfevict] building CoverageValidator"
make CoverageValidator || { echo "[selfevict] build failed" >&2; exit 1; }

if ! command -v wrmsr >/dev/null 2>&1; then
    echo "[selfevict] ERROR: 'wrmsr' not found. Install msr-tools (apt install msr-tools)." >&2
    exit 1
fi
if ! sudo -n true 2>/dev/null; then
    echo "[selfevict] WARNING: passwordless sudo not available; foreground runs will prompt." >&2
fi

# ---- one-time PMU host setup (root) ----
#   modprobe msr            : expose /dev/cpu/*/msr for wrmsr
#   /sys/devices/cpu/rdpmc=2: CR4.PCE=1 so the ring-3 rdpmc in the tool is legal
#   nmi_watchdog=0          : free the PMC the watchdog would otherwise hold
echo "[selfevict] PMU host setup (msr module, rdpmc, nmi_watchdog)"
sudo modprobe msr || { echo "[selfevict] ERROR: modprobe msr failed" >&2; exit 1; }
sudo sh -c 'echo 2 > /sys/devices/cpu/rdpmc' || { echo "[selfevict] ERROR: enabling rdpmc failed" >&2; exit 1; }
sudo sh -c 'echo 0 > /proc/sys/kernel/nmi_watchdog' 2>/dev/null || \
    echo "[selfevict] note: could not disable nmi_watchdog (may already be off)"

echo "[selfevict] SHUFFLE=$SHUFFLE ($SHUFFLE_TOKEN) WARMPASSES=$WARMPASSES buf=${JSMAP_BUF_MB}MB pref=0x${PREF_VAL} bidirR=${BIDIR_R}  NoCs: ${NOCS[*]}  iters each: $ITERS"
echo "[selfevict] output tree: $OUT_ROOT"

# ---- sweep ----
fail=0
for noc in "${NOCS[@]}"; do
    for ((i = 0; i < ITERS; i++)); do
        out="$OUT_ROOT/NoC$(printf '%02d' "$noc")/$(printf '%03d' "$i").csv"
        if [ -s "$out" ]; then
            echo "[selfevict] skip NoC=$noc iter=$i (exists: $out)"; continue
        fi
        echo "============================================================"
        echo "[selfevict] NoC=$noc iter=$i   ($(date '+%Y-%m-%d %H:%M:%S'))"
        echo "============================================================"
        # sudo resets env; pass the tree tags the C tool needs to build its output path + buffer.
        if ! sudo env "PREF_SUFFIX=$PREF_SUFFIX" "JSMAP_BUF_MB=$JSMAP_BUF_MB" \
                "JSMAP_BIDIR=$([ "$BIDIR_R" -gt 0 ] && echo 1 || echo 0)" "JSMAP_BIDIR_R=$BIDIR_R" \
                ./CoverageValidator "$noc" "$i" selfevict "$SHUFFLE_TOKEN" "$WARMPASSES"; then
            echo "[selfevict] WARNING: run failed (NoC=$noc iter=$i) -- continuing"
            fail=$((fail + 1))
        fi
    done
done

sudo chown -R "$(id -u):$(id -g)" "$OUT_ROOT" 2>/dev/null || true
echo "[selfevict] done ($fail failed run(s)). data under: $(pwd)/$OUT_ROOT"
exit 0
