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
# JSMAP_BUF_MB (jsmap only, env, default 12): victim buffer size in MB, a multiple of 12
# (12=one LLC=mean 12 lines/set; 24=mean 24). Tagged _<MB>MB (omitted at 12):
#   JSMAP_BUF_MB=24 ./run_coverage_native.sh 2 jsmap 8               -> ..._24MB/
#
# EV_A/EV_D/EV_C (jsmap only, env, default 1/1/1 = off): Rowhammer.js sliding-window eviction
# strategy over the cluster's lines (A=window repeats, D=window size, C=step). Any >1 REPLACES the
# JS pointer chase with sweep_lazy_evict (passes/accesses/same/buddy become inert):
#   EV_A=2 EV_D=4 ./run_coverage_native.sh 2 jsmap 64                 -> ..._p1a3_evA2D4C1/
#
# DECOY (jsmap only, env, default 0 = off): number of random, disjoint-set lines touched
# (lfence-bounded on both sides) between EVERY subcluster window of the eviction-strategy sweep
# above -- only has an effect together with EV_A/EV_D/EV_C (inert with the plain pointer chase).
# Tests whether a small amount of unrelated cache traffic between subcluster bursts is enough to
# move coverage, without assuming it has to be MB-scale. Tagged _dK<DECOY>:
#   EV_A=3 EV_D=3072 EV_C=3072 DECOY=128 ./run_coverage_native.sh 2 jsmap 8  -> ..._evA3D3072C3072_dK128/
#
# SHUFFLE (env, default 1 = shuffled/prefetch-defeating) toggles the victim traversal order
# and picks the output tree, for BOTH modes:
#   SHUFFLE=1 native -> data/coverage/native_shuffled ; jsmap -> data/coverage/native_shuffled_p{P}a{A}..
#   SHUFFLE=0 native -> data/coverage/native          ; jsmap -> data/coverage/native_jsmap_p{P}a{A}..
# SHUFFLE=0 runs the victim in address order (streamer-prefetch A/B against the shuffled default):
#   SHUFFLE=0 ./run_coverage_native.sh 2 native      -> data/coverage/native/
#   SHUFFLE=1 ./run_coverage_native.sh 2 native      -> data/coverage/native_shuffled/  (default)
#
# Normally run this AS YOUR NORMAL USER (not via sudo): it calls sudo itself for each
# CoverageValidator run (which needs hugepages/pagemap). A run whose output already
# exists is SKIPPED, so re-running resumes after a failure.
#
# For UNATTENDED runs (nohup &), the internal per-run "sudo" has no tty to prompt on once
# nohup detaches, so it needs EITHER passwordless sudo configured (see run_coverage_sweep.sh
# header for the sudoers recipe) OR the whole script invoked under an outer `sudo` up front
# (root sudo'ing to root needs no further auth, so every internal call then succeeds silently).
# The output-ownership handback at the end uses $SUDO_UID/$SUDO_GID (set by sudo to the
# ORIGINAL invoking user) when present, so outputs land back with your normal user either way
# -- do NOT "fix" this by reverting to plain "$(id -u):$(id -g)", that resolves to root:root
# when the whole script is itself running as root (outer-sudo invocation).
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
# Victim buffer size in MB (jsmap only), default 12 (= one LLC = mean 12 lines/set). Must be a
# multiple of 12; larger raises membership (24 -> mean 24/set). Tagged into the dir as _<MB>MB
# (omitted at the 12 MB default). Forwarded to the C tool via the JSMAP_BUF_MB env var.
JSMAP_BUF_MB="${JSMAP_BUF_MB:-12}"
# Eviction-strategy knobs (jsmap only): A=window repeats, D=window size, C=step (argv[9..11]).
# Any value >1 selects the Rowhammer.js sliding-window sweep (sweep_lazy_evict) INSTEAD of the JS
# pointer chase; passes/accesses/same then become inert. Default 1/1/1 = off.
EV_A="${EV_A:-1}"; EV_D="${EV_D:-1}"; EV_C="${EV_C:-1}"
# Decoy dose (jsmap, eviction-strategy mode only): random disjoint-set lines touched between
# every subcluster window. Default 0 = off (byte-for-byte the original sweep_lazy_evict).
DECOY="${DECOY:-0}"
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
    if ! [[ "$DECOY" =~ ^[0-9]+$ ]]; then
        echo "DECOY must be an integer >= 0, got '$DECOY'" >&2
        exit 2
    fi
    if [ "$DECOY" -gt 0 ] && [ "$EV_A" -eq 1 ] && [ "$EV_D" -eq 1 ] && [ "$EV_C" -eq 1 ]; then
        echo "[native] WARNING: DECOY=$DECOY has no effect without EV_A/EV_D/EV_C (plain pointer-chase mode ignores it)" >&2
    fi
    if ! [[ "$JSMAP_BUF_MB" =~ ^[0-9]+$ ]] || [ "$JSMAP_BUF_MB" -lt 12 ] || [ $((JSMAP_BUF_MB % 12)) -ne 0 ]; then
        echo "JSMAP_BUF_MB must be a multiple of 12 (>=12), got '$JSMAP_BUF_MB'" >&2
        exit 2
    fi
fi
# Buffer-size dir suffix, mirroring the C tool: omitted at the 12 MB default, "_24MB" etc. otherwise.
if [ "$JSMAP_BUF_MB" = 12 ]; then MB_SUFFIX=""; else MB_SUFFIX="_${JSMAP_BUF_MB}MB"; fi
# Eviction-strategy active iff any param >1; token slot (argv[9..11]) and dir suffix.
if [ "$EV_A" -gt 1 ] || [ "$EV_D" -gt 1 ] || [ "$EV_C" -gt 1 ]; then
    EV_ACTIVE=1; EV_SUFFIX="_evA${EV_A}D${EV_D}C${EV_C}"
else
    EV_ACTIVE=0; EV_SUFFIX=""
fi
# Decoy dir suffix (only meaningful -- and only ever nonzero here -- alongside EV_ACTIVE).
if [ "$DECOY" -gt 0 ]; then DECOY_SUFFIX="_dK${DECOY}"; else DECOY_SUFFIX=""; fi
# Access pattern token forwarded to the C tool ("same" or "words"), and dir suffix.
if [ "$JSMAP_SAME" = 1 ]; then JSMAP_PATTERN="same"; SAME_SUFFIX="_same"; else JSMAP_PATTERN="words"; SAME_SUFFIX=""; fi
# Buddy-touch (128B adjacent-line reinforcement) token (argv[8]="buddy") and dir suffix.
# argv[8] is positional after the pattern token, so pass "words" explicitly when buddy is on.
if [ "$JSMAP_BUDDY" = 1 ]; then BUDDY_TOKEN="buddy"; BUDDY_SUFFIX="_buddy"; else BUDDY_TOKEN=""; BUDDY_SUFFIX=""; fi

# Prefetcher-state tag: read MSR 0x1a4 (MSR_MISC_FEATURE_CONTROL; a SET bit DISABLES a prefetcher --
# bit0=L2 streamer, bit1=L2 adjacent-line, bit2=L1 DCU, bit3=L1 DCU-IP) on EVERY logical CPU. If all
# cores agree, tag the output tree _pref0x<val> (e.g. _pref0x0 = all on, _pref0x2 = adjacent-line off)
# so prefetch conditions never share a directory. Cores disagreeing = an invalid setup -> abort.
if ! command -v rdmsr >/dev/null 2>&1; then
    echo "[native] ERROR: rdmsr not found (install msr-tools) -- cannot tag prefetcher state" >&2
    exit 1
fi
sudo modprobe msr 2>/dev/null || true
mapfile -t PREF_MSR < <(sudo rdmsr -a 0x1a4 2>/dev/null)
PREF_VAL="$(printf '%s\n' "${PREF_MSR[@]}" | sort -u)"
if [ "${#PREF_MSR[@]}" -eq 0 ] || [ -z "$PREF_VAL" ]; then
    echo "[native] ERROR: could not read MSR 0x1a4 (need sudo + msr module loaded)" >&2
    exit 1
fi
if [ "$(printf '%s\n' "$PREF_VAL" | wc -l)" -ne 1 ]; then
    echo "[native] ERROR: MSR 0x1a4 is not uniform across cores -- prefetcher state is mixed:" >&2
    printf '%s\n' "${PREF_MSR[@]}" | sort | uniq -c >&2
    exit 1
fi
PREF_SUFFIX="_pref0x${PREF_VAL}"

# Output tree, matching the C tool. Shuffled/unshuffled pick different roots:
#   native : native_shuffled (SHUFFLE=1) | native (SHUFFLE=0)
#   jsmap  : native_jsmap_shuffled_p{P}a{A}[..] (SHUFFLE=1) | native_jsmap_p{P}a{A}[..] (SHUFFLE=0)
# jsmap ALWAYS gets its own _p{P}a{A}[_same][_buddy][_evA{A}D{D}C{C}] tree (incl. p1a1) so it never
# collides with the mapping_B native/native_shuffled trees. The _pref0x<val> tag is appended last.
if [ "$MODE" = "jsmap" ]; then
    JSMAP_ROOT="$([ "$SHUFFLE" = 1 ] && echo native_jsmap_shuffled || echo native_jsmap)"
    OUT_ROOT="data/coverage/${JSMAP_ROOT}_p${JSMAP_PASSES}a${JSMAP_ACCESSES}${SAME_SUFFIX}${BUDDY_SUFFIX}${EV_SUFFIX}${DECOY_SUFFIX}${PREF_SUFFIX}${MB_SUFFIX}"
else
    OUT_ROOT="data/coverage/$([ "$SHUFFLE" = 1 ] && echo native_shuffled || echo native)${PREF_SUFFIX}"
fi

# ALL_NOCS=(32 16 8 4 2 64)
ALL_NOCS=(32 16 8 64)
# ALL_NOCS=(32)



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
        DECOY_NOTE=""
        [ "$DECOY" -gt 0 ] && DECOY_NOTE=" + DECOY=$DECOY (random disjoint-set lines between every subcluster window)"
        echo "[native] mode: jsmap ($SHUFFLE_TOKEN, EVICTION-STRATEGY A=$EV_A D=$EV_D C=$EV_C -- passes/accesses/pattern inert)${DECOY_NOTE}   NoCs: ${NOCS[*]}   iterations each: $ITERS"
    else
        echo "[native] mode: jsmap ($SHUFFLE_TOKEN, passes=$JSMAP_PASSES accesses/line=$JSMAP_ACCESSES pattern=$JSMAP_PATTERN buddy=$JSMAP_BUDDY buf=${JSMAP_BUF_MB}MB)   NoCs: ${NOCS[*]}   iterations each: $ITERS"
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
        # PREF_SUFFIX, JSMAP_BUF_MB and DECOY must reach the (sudo'd) binary, which builds the
        # actual output path/buffer/decoy pool; sudo resets the env, so pass them explicitly.
        if ! sudo env "PREF_SUFFIX=$PREF_SUFFIX" "JSMAP_BUF_MB=$JSMAP_BUF_MB" "DECOY=$DECOY" "${run_cmd[@]}"; then
            echo "[native] WARNING: run failed (NoC=$noc iter=$i) -- continuing"
            fail=$((fail + 1))
        fi
    done
done

# the sudo'd binary wrote root-owned CSVs; hand them back to the invoking user
# SUDO_UID/SUDO_GID (set by sudo to the ORIGINAL invoking user) take priority over
# id -u/id -g, so this hands output back to you whether the script was launched as your
# normal user (no SUDO_UID; id -u/id -g are already yours) or wrapped in an outer sudo
# (SUDO_UID/SUDO_GID are yours; id -u/id -g would be root's without this fallback order).
sudo chown -R "${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" "$OUT_ROOT" 2>/dev/null || true

echo "[native] done ($fail failed run(s)). data under: $(pwd)/$OUT_ROOT"
exit 0
