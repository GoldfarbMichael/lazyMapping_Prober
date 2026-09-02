#!/usr/bin/env bash
# Real-browser WEBSITE fingerprinting sweep (Stage 4) — front-end to sweep_lib.sh.
#
# The Stage 4 twin of run_fingerprint_sweep.sh. Same coordinator, same JS sampler, same
# label/finalize contract; the victim changes from a stress-ng process on core 1 to a real
# website loaded in a SECOND TAB of the SAME Chrome (the Shusterman et al. threat model).
#
# Each run writes one memorygram CSV per sample under
#   ../JavaScript/data/website_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles/<site-slug>/<n>.csv
# (the server auto-increments <n>, so re-running APPENDS rather than overwrites).
#
# THREE DIFFERENCES FROM STAGE 3 WORTH KNOWING BEFORE YOU RUN IT
#
# 1. NO SUDO, ANYWHERE. WebsiteOrchestrator does no CPU pinning, so it needs no root -- which
#    means Chrome keeps its sandbox. That matters here in a way it did not in Stage 3: this
#    arm navigates to arbitrary live websites, and a root Chrome with --no-sandbox would turn
#    any drive-by into a root compromise. No sudoers entry and no xhost grant are needed.
#
# 2. NO CPU PINNING. Chrome is free to spread across all cores. The channel can therefore no
#    longer be claimed LLC-only: the two renderers may transiently co-reside on one core and
#    share its private L1/L2, and the sampler may migrate mid-trace. With two busy renderers
#    on 8 otherwise-idle cores that is a tail effect, but the honest claim for this arm is
#    "microarchitectural contention". Stages 1-3 remain the controlled upper bound.
#
# 3. THE CLASS LIST IS A FILE, NOT A COMPILED ARRAY. Edit sites.txt and comment sites out with
#    '#' -- no rebuild. That file is the main lever on wall time; see the budget note below.
#
# WALL-TIME BUDGET (~8 s/sample: 6 s TST + 0.5 s ack margin + 0.5 s cooldown + ~1 s victim setup)
# across the full 7-NoC sweep:
#     20 sites x 50 samples  ->  7,000 samples  ~16 h
#     38 sites x 50 samples  -> 13,300 samples  ~30 h
#    100 sites x 50 samples  -> 35,000 samples  ~78 h
#    100 sites x 100 samples -> 70,000 samples ~155 h
# 100 sites across all 7 NoCs is not practical in one pass. Suggested shape: comment sites.txt
# down to ~20-38 sites for the full NoC sweep (that curve is the actual thesis result), then do
# one 100-site run at the best NoC for a literature-comparable WF number.
#
# Run this AS YOUR NORMAL USER. It starts the server as you (so the CSVs are owned by you) and
# finalizes with your ssh keys.
set -uo pipefail

cd "$(dirname "$0")"   # stable/
SCRIPT_DIR="$(pwd)"

# ============================================
# Configuration — the experiment parameters
# ============================================
# Spatial sweep: one orchestrator run per NoC. Powers of two in [1,64] (the Lazy Mapping regime).
NOCS=(1 2 4 8 16 32 64)
# NOCS=(1)

# NOCS=(16)

# Total sampling time per trace, in seconds ("{N}TST"). Unlike stress-ng, a page load is a
# TRANSIENT: the discriminative signal is concentrated in the first seconds after navigation,
# which is why the orchestrator aligns t=0 to navigation start. 6 s comfortably covers a load.
TST=2
# Accesses between timer polls ("{N}K"); 0 selects the DYNAMIC-K sweep.
K=180
# Cycles per address ("{N}cycles"); sizes the JS cluster quantum, so it sets temporal resolution:
# rows T = floor(TST_ms / (Q * NoC)).
CYCLES_PER_ADDRESS=2288
# Samples collected per SITE, per NoC.
SAMPLES_PER_CLASS=100
# Cooldown between samples, MICROSECONDS (500000 = 0.5 s).
SAMPLE_COOLDOWN_US=500000
# Settle time between NoC runs (after tearing down that run's Chrome profile).
NOC_COOLDOWN_S=5

# The class list: '<slug><TAB><url>' per line, '#' to comment a site out. The SLUG is the class
# label -- it becomes the directory name and the .h5 label_map key.
SITES_FILE="${SITES_FILE:-$SCRIPT_DIR/sites.txt}"

# ============================================
# Stage identity / runner (sweep_lib.sh contract)
# ============================================
SWEEP_TITLE="REAL-BROWSER WEBSITE FINGERPRINTING SWEEP (STAGE 4)"
STAGE_TAG="website"              # the tag website_orchestrator.c prepends to the config dir
LOG_PREFIX="web"
CLASS_NOUN="site"

ORCH_BIN="./WebsiteOrchestrator"
ORCH_MAKE_TARGET="WebsiteOrchestrator"
ORCH_SUDO=""                     # no pinning -> no root -> Chrome keeps its sandbox
ORCH_EXTRA_ARGS=("$SITES_FILE")
CHROME_PROFILE="/tmp/chrome-website"   # MUST differ from Stage 3's: Chrome reuses a running
                                       # instance when --user-data-dir matches, which would open
                                       # the sampler as a tab in the other run's window.
EXTRA_PKILL=()
NEEDS_XHOST=0                    # Chrome runs as us, so it already has the :0 cookie

# ---- validate the site list before anything is built or launched ----
# The orchestrator re-validates in full (slug charset, duplicates, lengths); this is the cheap
# gate that keeps a typo from costing a build + a Chrome launch.
if [ ! -r "$SITES_FILE" ]; then
    echo "[sweep] ERROR: sites file not readable: $SITES_FILE" >&2; exit 2
fi
CLASS_COUNT=$(grep -cE '^[[:space:]]*[^#[:space:]]' "$SITES_FILE" || true)
if [ "${CLASS_COUNT:-0}" -lt 2 ]; then
    echo "[sweep] ERROR: $SITES_FILE has ${CLASS_COUNT:-0} uncommented site(s); need >= 2" >&2
    exit 2
fi

# ============================================
# Server / environment
# ============================================
SERVER_DIR="$(cd "$SCRIPT_DIR/../JavaScript" 2>/dev/null && pwd)" || {
    echo "[sweep] ERROR: JavaScript/ not found next to $SCRIPT_DIR" >&2; exit 1; }
DATA_TREE="$SERVER_DIR/data"
SERVER_LOG="$SCRIPT_DIR/website_server.log"
SERVER_CORE=2
CONDA_SH="/home/ubu/anaconda3/etc/profile.d/conda.sh"
CONDA_ENV="PC37"
LOG_DIR="$SCRIPT_DIR/web_logs"

# ============================================
# Finalize / backup (post-sweep)
# ============================================
DO_FINALIZE="${DO_FINALIZE:-1}"
FINALIZER="$SCRIPT_DIR/finalize_website.sh"
REMOTE_HOST="${REMOTE_HOST:-132.72.67.152}"
REMOTE_USER="${REMOTE_USER:-michael}"
REMOTE_DIR="${REMOTE_DIR:-/home/michael/michaels_backup_data}"
LOCAL_H5_DIR="${LOCAL_H5_DIR:-$SCRIPT_DIR/h5}"
PYTHON_BIN="${PYTHON_BIN:-/home/ubu/anaconda3/envs/PC37/bin/python3}"
DRY_RUN="${DRY_RUN:-0}"

# shellcheck source=sweep_lib.sh
source "$SCRIPT_DIR/sweep_lib.sh"
run_sweep
