#!/bin/bash
#
# finalize_website.sh — WEBSITE (Stage 4) front-end to finalize_lib.sh.
#
# After a full run_fingerprint_sweep.sh sweep, pack the browser-collected memorygram CSVs into ONE
# per-experiment .h5, delete the CSVs that fed it, and rsync the .h5 to the remote archive. Keeps
# the local .h5 copy. All of that work is finalize_lib.sh's; this script only names things.
#
# Identical to finalize_realbrowser.sh except for the tag and the h5 stem. Kept as a separate
# front-end (rather than a parameter) so a Stage 4 tree can never be packed with a Stage 3 name.
#
# Differences from the native front-end (finalize_experiment.sh):
#   * data lives under JavaScript/data (written by the Flask coordinator's /collect), NOT
#     stable/data/<clock_subdir>.
#   * config dirs carry the "website_" tag that website_orchestrator.c prepends, which keeps this
#     tree disjoint from Stage 3's "realbrowser_" tree and from the BARE <NoC>C_... dir a manual
#     single-shot browser run writes under the same root via /set-metadata.
#   * CLASS DIRS ARE SITE SLUGS, not stressor names -- they come from sites.txt and become the
#     keys of the .h5 label_map that the off-machine classifier reads.
#   * no clock/shuffle/buffer knobs: the sampler is real Chrome, and its victim buffer is pinned at
#     12 MB by main.js's LLC geometry (LLC_SETS * LLC_WAYS * 64B), so there is no _<N>MB tail.
#
# Unlike the native path this runs AS THE LOGIN USER (run_website_sweep.sh uses no sudo at all): `as_user` is then a no-op, the CSVs are server-owned (= user-owned) so the delete
# needs no root, and ssh/rsync use the user's own keys.
#
# Usage:
#   finalize_website.sh <TST> <K> <CPA> <NoC...>
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
    CONFIG_DIRS+=("website_${noc}C_${TST}TST_${K}K_${CPA}cycles")
done

# ---------------------------------------------------------------------------
# 3. h5 name: <label><rerun-index><tail>, e.g. realbrowser_2TST_0K_4576cycles.h5, then
#    realbrowser2_2TST_0K_4576cycles.h5 for the next sweep with the same parameters.
# ---------------------------------------------------------------------------
CLOCK_LABEL="website"
NAME_TAIL="_${TST}TST_${K}K_${CPA}cycles.h5"

# ---------------------------------------------------------------------------
# 4. Hand off to the shared machinery.
# ---------------------------------------------------------------------------
# shellcheck source=finalize_lib.sh
source "$SCRIPT_DIR/finalize_lib.sh"

echo "🧩 finalize_website"
finalize_run
