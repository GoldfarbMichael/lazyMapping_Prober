#!/bin/bash
#
# finalize_realbrowser.sh — REAL-BROWSER (Stage 3) front-end to finalize_lib.sh.
#
# After a full run_fingerprint_sweep.sh sweep, pack the browser-collected memorygram CSVs into ONE
# per-experiment .h5, delete the CSVs that fed it, and rsync the .h5 to the remote archive. Keeps
# the local .h5 copy. All of that work is finalize_lib.sh's; this script only names things.
#
# Differences from the native front-end (finalize_experiment.sh):
#   * data lives under JavaScript/data (written by the Flask coordinator's /collect), NOT
#     stable/data/<clock_subdir>.
#   * config dirs carry the "realbrowser_" tag that fingerprint_orchestrator.c prepends, which keeps
#     a manual single-shot browser run (it writes a BARE <NoC>C_... dir under the same root via
#     /set-metadata) out of a sweep's class dirs.
#   * no clock/shuffle/buffer knobs: the sampler is real Chrome, and its victim buffer is pinned at
#     12 MB by main.js's LLC geometry (LLC_SETS * LLC_WAYS * 64B), so there is no _<N>MB tail.
#
# Unlike the native path this normally runs AS THE LOGIN USER (run_fingerprint_sweep.sh is not a
# sudo script): `as_user` is then a no-op, the CSVs are server-owned (= user-owned) so the delete
# needs no root, and ssh/rsync use the user's own keys.
#
# Usage:
#   finalize_realbrowser.sh <TST> <K> <CPA> <NoC...>
# Config via environment (exported by the caller; defaults in finalize_lib.sh):
#   REMOTE_HOST REMOTE_USER REMOTE_DIR PYTHON_BIN LOCAL_H5_DIR DRY_RUN
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
if [ "$#" -lt 4 ]; then
    echo "usage: $0 <TST> <K> <CPA> <NoC...>" >&2
    exit 2
fi
TST="$1"; K="$2"; CPA="$3"; shift 3
NOCS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# 1. Where the browser wrote its CSVs: server.py's DATA_ROOT="data", relative to the server's cwd
#    (JavaScript/). Resolve it without requiring JavaScript/data to exist yet.
# ---------------------------------------------------------------------------
JS_DIR="$(cd "$SCRIPT_DIR/../JavaScript" && pwd)"
DATA_ROOT="$JS_DIR/data"

# ---------------------------------------------------------------------------
# 2. Config dir names: the label run_fingerprint_sweep.sh hands the orchestrator, with the
#    "realbrowser_" tag the orchestrator prepends before handing it to /fp/cmd.
# ---------------------------------------------------------------------------
CONFIG_DIRS=()
for noc in "${NOCS[@]}"; do
    CONFIG_DIRS+=("realbrowser_${noc}C_${TST}TST_${K}K_${CPA}cycles")
done

# ---------------------------------------------------------------------------
# 3. h5 name: <label><rerun-index><tail>, e.g. realbrowser_2TST_0K_4576cycles.h5, then
#    realbrowser2_2TST_0K_4576cycles.h5 for the next sweep with the same parameters.
# ---------------------------------------------------------------------------
CLOCK_LABEL="realbrowser"
NAME_TAIL="_${TST}TST_${K}K_${CPA}cycles.h5"

# ---------------------------------------------------------------------------
# 4. Hand off to the shared machinery.
# ---------------------------------------------------------------------------
# shellcheck source=finalize_lib.sh
source "$SCRIPT_DIR/finalize_lib.sh"

echo "🧩 finalize_realbrowser"
finalize_run
