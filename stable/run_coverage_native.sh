#!/usr/bin/env bash
# Sweep the NATIVE (pure-C, serial) lazy-mapping coverage validator across NoC
# values, N iterations each. This is the browser-free analog of run_coverage_sweep.sh:
# no Flask server, no Chrome, no xhost/:0 X grant -- the victim "sweep" runs in-process.
#
# Usage: ./run_coverage_native.sh <iterations_per_noc> [mode] [noc]
#   <iterations_per_noc>  number of runs per NoC (required, >=1)
#   [mode]                native (default) | jsmap. Both run the SHUFFLED victim.
#                           native -> CoverageValidator <noc> <iter> native shuffle
#                                     (saved mapping_B clusters, line-shuffled)
#                           jsmap  -> CoverageValidator <noc> <iter> jsmap shuffle
#                                     (JS-faithful mmap victim, page-shuffled)
#   [noc]                 optional: run ONLY this NoC (power of two in 2..64).
#                         Omitted -> sweep the ALL_NOCS list below.
#   [mode] and [noc] may be given in either order (mode = word, noc = number).
#
# native writes data/coverage/native_shuffled/ (the mapping_B victim).
# jsmap ALWAYS writes its own knob-tagged tree (never collides with native_shuffled):
#   data/coverage/native_shuffled_p{PASSES}a{ACCESSES}/NoC{nn}/{iter}.csv
# Tune the knobs (jsmap only) via env vars, defaults JSMAP_PASSES=1, JSMAP_ACCESSES=3,
# JSMAP_SAME=0 (0=different words in the line, 1=repeat the EXACT same address):
#   JSMAP_ACCESSES=1 ./run_coverage_native.sh 2 jsmap                 -> ..._p1a1/
#   JSMAP_ACCESSES=4 ./run_coverage_native.sh 2 jsmap                 -> ..._p1a4/
#   JSMAP_SAME=1 JSMAP_ACCESSES=3 ./run_coverage_native.sh 2 jsmap    -> ..._p1a3_same/
#
# Run this AS YOUR NORMAL USER (not via sudo): it calls sudo itself for each
# CoverageValidator run (which needs hugepages/pagemap). A run whose output already
# exists is SKIPPED, so re-running resumes after a failure.
#
# For UNATTENDED runs (nohup &), set up passwordless sudo (see run_coverage_sweep.sh
# header for the sudoers recipe) so it never needs a tty.
#
# Requires /dev/hugepages/map_A + mapping_A.bin (mapping A = prober). native mode ALSO
# requires /dev/hugepages/map_B + mapping_B.bin (the saved lazy victim); jsmap does not.
set -uo pipefail

cd "$(dirname "$0")"   # stable/

ITERS="${1:-}"
if ! [[ "$ITERS" =~ ^[0-9]+$ ]] || [ "$ITERS" -lt 1 ]; then
    echo "usage: $0 <iterations_per_noc> [mode] [noc]" >&2
    exit 2
fi

# Remaining args (in any order): a word => mode, a number => single NoC.
MODE="native"
ONLY_NOC=""
for arg in "${@:2}"; do
    if [[ "$arg" =~ ^[0-9]+$ ]]; then
        ONLY_NOC="$arg"
    else
        MODE="$arg"
    fi
done
if [ "$MODE" != "native" ] && [ "$MODE" != "jsmap" ]; then
    echo "mode must be 'native' or 'jsmap', got '$MODE'" >&2
    exit 2
fi

# jsmap replacement-policy knobs (env-overridable): full sweeps per probe, accesses per node,
# and access pattern. JSMAP_SAME=1 repeats the EXACT same address (else different words in the
# line). Must mirror the C tool's dir naming (native_shuffled_p{P}a{A}[_same]).
JSMAP_PASSES="${JSMAP_PASSES:-1}"
JSMAP_ACCESSES="${JSMAP_ACCESSES:-3}"
JSMAP_SAME="${JSMAP_SAME:-0}"
if [ "$MODE" = "jsmap" ]; then
    if ! [[ "$JSMAP_PASSES" =~ ^[0-9]+$ ]] || [ "$JSMAP_PASSES" -lt 1 ] \
    || ! [[ "$JSMAP_ACCESSES" =~ ^[0-9]+$ ]] || [ "$JSMAP_ACCESSES" -lt 1 ]; then
        echo "JSMAP_PASSES and JSMAP_ACCESSES must be integers >= 1" >&2
        exit 2
    fi
fi
# Access pattern token forwarded to the C tool ("same" or "words"), and dir suffix.
if [ "$JSMAP_SAME" = 1 ]; then JSMAP_PATTERN="same"; SAME_SUFFIX="_same"; else JSMAP_PATTERN="words"; SAME_SUFFIX=""; fi

# Output tree, matching the C tool. native -> native_shuffled. jsmap ALWAYS gets its own
# _p{P}a{A}[_same] tree (incl. p1a1) so it never collides with the mapping_B native_shuffled tree.
if [ "$MODE" = "jsmap" ]; then
    OUT_ROOT="data/coverage/native_shuffled_p${JSMAP_PASSES}a${JSMAP_ACCESSES}${SAME_SUFFIX}"
else
    OUT_ROOT="data/coverage/native_shuffled"
fi

ALL_NOCS=(32 16 8 4 2 64)

if [ -n "$ONLY_NOC" ]; then
    valid=0
    for n in "${ALL_NOCS[@]}"; do [ "$n" = "$ONLY_NOC" ] && valid=1; done
    if [ "$valid" -ne 1 ]; then
        echo "noc must be one of: ${ALL_NOCS[*]}" >&2
        exit 2
    fi
    NOCS=("$ONLY_NOC")
else
    NOCS=("${ALL_NOCS[@]}")
fi

# ---- pre-flight ----
echo "[native] building CoverageValidator"
make CoverageValidator || { echo "[native] build failed" >&2; exit 1; }

# mapping_A is always needed (prober); mapping_B only for the saved-victim `native` mode.
REQUIRED_FILES=(/dev/hugepages/map_A mapping_A.bin)
if [ "$MODE" = "native" ]; then
    REQUIRED_FILES+=(/dev/hugepages/map_B mapping_B.bin)
fi
for f in "${REQUIRED_FILES[@]}"; do
    if [ ! -e "$f" ]; then
        echo "[native] ERROR: required mapping artifact missing: $f" >&2
        echo "         (re)create the hugepages + BIN mappings before running." >&2
        exit 1
    fi
done

# Verify sudo won't block on a password (needed for hugepages/pagemap access).
if ! sudo -n true 2>/dev/null; then
    echo "[native] WARNING: passwordless sudo not available. Foreground runs will prompt;"
    echo "         unattended (nohup) runs WILL fail after the tty closes -- add the"
    echo "         NOPASSWD sudoers entry (see run_coverage_sweep.sh header)." >&2
fi

if [ "$MODE" = "jsmap" ]; then
    echo "[native] mode: jsmap (shuffled, passes=$JSMAP_PASSES accesses/line=$JSMAP_ACCESSES pattern=$JSMAP_PATTERN)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
else
    echo "[native] mode: native (shuffled)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
fi
echo "[native] output tree: $OUT_ROOT"

# ---- sweep ----
fail=0
for noc in "${NOCS[@]}"; do
    for ((i = 0; i < ITERS; i++)); do
        out="$OUT_ROOT/NoC$(printf '%02d' "$noc")/$(printf '%03d' "$i").csv"
        if [ -s "$out" ]; then
            echo "[native] skip NoC=$noc iter=$i (exists: $out)"
            continue
        fi
        echo "============================================================"
        echo "[native] NoC=$noc iter=$i   ($(date '+%Y-%m-%d %H:%M:%S'))"
        echo "============================================================"
        if [ "$MODE" = "jsmap" ]; then
            run_cmd=(./CoverageValidator "$noc" "$i" jsmap shuffle "$JSMAP_PASSES" "$JSMAP_ACCESSES" "$JSMAP_PATTERN")
        else
            run_cmd=(./CoverageValidator "$noc" "$i" native shuffle)
        fi
        if ! sudo "${run_cmd[@]}"; then
            echo "[native] WARNING: run failed (NoC=$noc iter=$i) -- continuing"
            fail=$((fail + 1))
        fi
    done
done

# the sudo'd binary wrote root-owned CSVs; hand them back to the invoking user
sudo chown -R "$(id -u):$(id -g)" "$OUT_ROOT" 2>/dev/null || true

echo "[native] done ($fail failed run(s)). data under: $(pwd)/$OUT_ROOT"
exit 0
