#!/usr/bin/env bash
# Sweep the NATIVE (pure-C, serial) lazy-mapping coverage validator across NoC
# values, N iterations each. This is the browser-free analog of run_coverage_sweep.sh:
# no Flask server, no Chrome, no xhost/:0 X grant -- the victim "sweep" runs in-process.
#
# Usage: ./run_coverage_native.sh <iterations_per_noc> [mode] [noc]
#   <iterations_per_noc>  number of runs per NoC (required, >=1)
#   [mode]                native (default) | jsmap. Shuffled victim by default (SHUFFLE=1).
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
# JSMAP_SAME=0 (0=different words in the line, 1=repeat the EXACT same address), JSMAP_BUDDY=0
# (1=also demand-access each line's 128B buddy -- the adjacent-line reinforcement diagnostic):
#   JSMAP_ACCESSES=1 ./run_coverage_native.sh 2 jsmap                 -> ..._p1a1/
#   JSMAP_ACCESSES=4 ./run_coverage_native.sh 2 jsmap                 -> ..._p1a4/
#   JSMAP_SAME=1 JSMAP_ACCESSES=3 ./run_coverage_native.sh 2 jsmap    -> ..._p1a3_same/
#   JSMAP_BUDDY=1 ./run_coverage_native.sh 2 jsmap                    -> ..._p1a3_buddy/
#
# EV_A/EV_D/EV_C (jsmap only, env, default 1/1/1 = off): Rowhammer.js sliding-window eviction
# strategy over the cluster's lines (A=window repeats, D=window size, C=step). Any >1 REPLACES the
# JS pointer chase with sweep_lazy_evict (passes/accesses/same/buddy become inert):
#   EV_A=2 EV_D=4 ./run_coverage_native.sh 2 jsmap 64                 -> ..._p1a3_evA2D4C1/
#
# SHUFFLE (env, default 1 = shuffled/prefetch-defeating) toggles the victim traversal order
# and picks the output tree, for BOTH modes:
#   SHUFFLE=1 native -> data/coverage/native_shuffled ; jsmap -> data/coverage/native_shuffled_p{P}a{A}..
#   SHUFFLE=0 native -> data/coverage/native          ; jsmap -> data/coverage/native_jsmap_p{P}a{A}..
# SHUFFLE=0 runs the victim in address order (streamer-prefetch A/B against the shuffled default):
#   SHUFFLE=0 ./run_coverage_native.sh 2 native      -> data/coverage/native/
#   SHUFFLE=1 ./run_coverage_native.sh 2 native      -> data/coverage/native_shuffled/  (default)
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

# Victim traversal order (both modes): 1 = shuffled (default, prefetch-defeating),
# 0 = address order (streamer-prefetch A/B). Forwarded to the C tool as argv[4]
# ("shuffle"/"noshuffle") and selects the unshuffled vs shuffled output tree.
SHUFFLE="${SHUFFLE:-1}"
if [ "$SHUFFLE" != 0 ] && [ "$SHUFFLE" != 1 ]; then
    echo "SHUFFLE must be 0 or 1, got '$SHUFFLE'" >&2
    exit 2
fi
if [ "$SHUFFLE" = 1 ]; then SHUFFLE_TOKEN="shuffle"; else SHUFFLE_TOKEN="noshuffle"; fi

# jsmap replacement-policy knobs (env-overridable): full sweeps per probe, accesses per node,
# and access pattern. JSMAP_SAME=1 repeats the EXACT same address (else different words in the
# line). Must mirror the C tool's dir naming (native_shuffled_p{P}a{A}[_same]).
JSMAP_PASSES="${JSMAP_PASSES:-1}"
JSMAP_ACCESSES="${JSMAP_ACCESSES:-1}"
JSMAP_SAME="${JSMAP_SAME:-0}"
JSMAP_BUDDY="${JSMAP_BUDDY:-0}"
# Eviction-strategy knobs (jsmap only): A=window repeats, D=window size, C=step (argv[9..11]).
# Any value >1 selects the Rowhammer.js sliding-window sweep (sweep_lazy_evict) INSTEAD of the JS
# pointer chase; passes/accesses/same then become inert. Default 1/1/1 = off.
EV_A="${EV_A:-1}"; EV_D="${EV_D:-1}"; EV_C="${EV_C:-1}"
if [ "$MODE" = "jsmap" ]; then
    if ! [[ "$JSMAP_PASSES" =~ ^[0-9]+$ ]] || [ "$JSMAP_PASSES" -lt 1 ] \
    || ! [[ "$JSMAP_ACCESSES" =~ ^[0-9]+$ ]] || [ "$JSMAP_ACCESSES" -lt 1 ]; then
        echo "JSMAP_PASSES and JSMAP_ACCESSES must be integers >= 1" >&2
        exit 2
    fi
    for ev in "$EV_A" "$EV_D" "$EV_C"; do
        if ! [[ "$ev" =~ ^[0-9]+$ ]] || [ "$ev" -lt 1 ]; then
            echo "EV_A/EV_D/EV_C must be integers >= 1, got A=$EV_A D=$EV_D C=$EV_C" >&2
            exit 2
        fi
    done
fi
# Eviction-strategy active iff any param >1; token slot (argv[9..11]) and dir suffix.
if [ "$EV_A" -gt 1 ] || [ "$EV_D" -gt 1 ] || [ "$EV_C" -gt 1 ]; then
    EV_ACTIVE=1; EV_SUFFIX="_evA${EV_A}D${EV_D}C${EV_C}"
else
    EV_ACTIVE=0; EV_SUFFIX=""
fi
# Access pattern token forwarded to the C tool ("same" or "words"), and dir suffix.
if [ "$JSMAP_SAME" = 1 ]; then JSMAP_PATTERN="same"; SAME_SUFFIX="_same"; else JSMAP_PATTERN="words"; SAME_SUFFIX=""; fi
# Buddy-touch (128B adjacent-line reinforcement) token (argv[8]="buddy") and dir suffix.
# argv[8] is positional after the pattern token, so pass "words" explicitly when buddy is on.
if [ "$JSMAP_BUDDY" = 1 ]; then BUDDY_TOKEN="buddy"; BUDDY_SUFFIX="_buddy"; else BUDDY_TOKEN=""; BUDDY_SUFFIX=""; fi

# Output tree, matching the C tool. Shuffled/unshuffled pick different roots:
#   native : native_shuffled (SHUFFLE=1) | native (SHUFFLE=0)
#   jsmap  : native_jsmap_shuffled_p{P}a{A}[..] (SHUFFLE=1) | native_jsmap_p{P}a{A}[..] (SHUFFLE=0)
# jsmap ALWAYS gets its own _p{P}a{A}[_same][_buddy][_evA{A}D{D}C{C}] tree (incl. p1a1) so it never
# collides with the mapping_B native/native_shuffled trees.
if [ "$MODE" = "jsmap" ]; then
    JSMAP_ROOT="$([ "$SHUFFLE" = 1 ] && echo native_jsmap_shuffled || echo native_jsmap)"
    OUT_ROOT="data/coverage/${JSMAP_ROOT}_p${JSMAP_PASSES}a${JSMAP_ACCESSES}${SAME_SUFFIX}${BUDDY_SUFFIX}${EV_SUFFIX}"
else
    OUT_ROOT="data/coverage/$([ "$SHUFFLE" = 1 ] && echo native_shuffled || echo native)"
fi

# ALL_NOCS=(32 16 8 4 2 64)
ALL_NOCS=(32 16 8 64)


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
    if [ "$EV_ACTIVE" = 1 ]; then
        echo "[native] mode: jsmap ($SHUFFLE_TOKEN, EVICTION-STRATEGY A=$EV_A D=$EV_D C=$EV_C -- passes/accesses/pattern inert)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
    else
        echo "[native] mode: jsmap ($SHUFFLE_TOKEN, passes=$JSMAP_PASSES accesses/line=$JSMAP_ACCESSES pattern=$JSMAP_PATTERN buddy=$JSMAP_BUDDY)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
    fi
else
    echo "[native] mode: native ($SHUFFLE_TOKEN)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
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
            run_cmd=(./CoverageValidator "$noc" "$i" jsmap "$SHUFFLE_TOKEN" "$JSMAP_PASSES" "$JSMAP_ACCESSES" "$JSMAP_PATTERN")
            if [ "$EV_ACTIVE" = 1 ]; then
                # argv[8] must be filled before A/D/C (argv[9..11]); use buddy token or a placeholder.
                run_cmd+=("${BUDDY_TOKEN:-nobuddy}" "$EV_A" "$EV_D" "$EV_C")
            else
                [ -n "$BUDDY_TOKEN" ] && run_cmd+=("$BUDDY_TOKEN")   # argv[8]="buddy" (reinforcement diagnostic)
            fi
        else
            run_cmd=(./CoverageValidator "$noc" "$i" native "$SHUFFLE_TOKEN")
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
