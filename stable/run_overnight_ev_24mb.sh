#!/usr/bin/env bash
# Overnight batch (MSR 0x1a4 forced to 0xf = all prefetchers OFF):
#   Phase 1: the full eviction-strategy grid (A,D,C) at the default 12 MB buffer.
#   Phase 2: the plain p3a1_same pointer-chase at 24 MB (the membership test).
# Each CoverageValidator run sudo's itself, so PASSWORDLESS SUDO must be configured for an
# unattended nohup run. Skip-resume is on: re-running continues where it stopped.
set -uo pipefail
cd "$(dirname "$0")"   # stable/

ITERS_EV=2       # iters per (A,D,C) x NoC for the eviction-strategy grid (12 MB)
ITERS_24MB=5     # iters per NoC for the p3a1_same 24 MB run

# --- force prefetcher state = all OFF (0xf) for the whole batch (MSRs are volatile) ---
sudo modprobe msr 2>/dev/null || true
sudo wrmsr -a 0x1a4 0xf || { echo "[overnight] ERROR: wrmsr 0x1a4=0xf failed" >&2; exit 1; }
echo "[overnight] $(date '+%F %T')  MSR 0x1a4 = 0xf (all prefetchers OFF) on all cores"

# --- Phase 1: eviction-strategy grid at 12 MB (same combos as the prior grid) ---
A_LIST=(2 3 4)
D_LIST=(2 4 8 16)
C_LIST=(1 2)
for A in "${A_LIST[@]}"; do
  for D in "${D_LIST[@]}"; do
    for C in "${C_LIST[@]}"; do
      echo "[overnight] $(date '+%F %T')  === EV A=$A D=$D C=$C  (12 MB) ==="
      EV_A=$A EV_D=$D EV_C=$C ./run_coverage_native.sh "$ITERS_EV" jsmap \
        || echo "[overnight] WARNING: EV A=$A D=$D C=$C returned nonzero -- continuing"
    done
  done
done

# --- Phase 2: plain p3a1_same at 24 MB (double buffer -> mean 24 lines/set) ---
echo "[overnight] $(date '+%F %T')  === p3a1_same  (24 MB) ==="
JSMAP_PASSES=3 JSMAP_ACCESSES=1 JSMAP_SAME=1 JSMAP_BUF_MB=24 \
  ./run_coverage_native.sh "$ITERS_24MB" jsmap \
  || echo "[overnight] WARNING: p3a1_same 24MB returned nonzero"

echo "[overnight] $(date '+%F %T')  ALL DONE"
