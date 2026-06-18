#!/usr/bin/env bash
# Real-browser stress-ng fingerprinting sweep.
#
# Starts the Flask coordinator (pinned to core 2), then runs FingerprintOrchestrator for
# every NoC in {1,2,4,8,16,32,64}. Each run opens Chrome on core 0 (the JS sampler), runs
# the stress-ng battery on core 1, and writes one memorygram CSV per sample under
#   ../JavaScript/data/realbrowser_{NoC}C_2TST_90K_2288cycles/<stressor>/<n>.csv
# (the server auto-increments <n>, so re-running APPENDS rather than overwrites).
#
# Run this AS YOUR NORMAL USER (not via sudo): it needs xhost for your X session and starts
# the server as you (so the CSVs are owned by you). It calls `sudo` only for the orchestrator
# (which pins cores + launches Chrome as root). Add a passwordless sudoers entry for it, e.g.
# /etc/sudoers.d/fingerprint_orchestrator (adjust user/path):
#     ubu ALL=(root) NOPASSWD: /home/ubu/Desktop/Michael/lazyMapping_Prober/stable/FingerprintOrchestrator
# then `sudo chmod 0440` it and `sudo visudo -c` to validate.
set -uo pipefail

cd "$(dirname "$0")"   # stable/

NOCS=(1 2 4 8 16 32 64)
SERVER_DIR="../JavaScript"
SERVER_LOG="$(pwd)/fingerprint_server.log"
SERVER_CORE=2
CONDA_SH="/home/ubu/anaconda3/etc/profile.d/conda.sh"   # sourced to enable `conda activate`
CONDA_ENV="base"                                         # env that has flask installed

SERVER_PID=""          # set if WE start the server (so cleanup only kills our own)

server_up() {
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8080/fp/state"
}

cleanup() {
    echo "[sweep] cleanup: stopping stressors / chrome / server"
    sudo pkill -9 stress-ng >/dev/null 2>&1 || true
    sudo pkill -f 'user-data-dir=/tmp/chrome-fingerprint' >/dev/null 2>&1 || true
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

# ---- pre-flight ----
# Grant root access to :0 using the SAME display/cookie the orchestrator gives Chrome.
# Without this Chrome can't reach :0 (your ~/.Xauthority is the :1 Xtigervnc cookie) and
# every CSV would be noise. Machine-specific (uid 1000).
echo "[sweep] granting root access to X display :0 (xhost)"
if DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority xhost +SI:localuser:root >/dev/null 2>&1; then
    echo "[sweep] xhost grant OK"
else
    echo "[sweep] WARNING: xhost grant FAILED -- Chrome will not reach :0 and data will be" >&2
    echo "        noise. Fix X access before trusting results." >&2
fi

echo "[sweep] building FingerprintOrchestrator"
make FingerprintOrchestrator || { echo "[sweep] build failed" >&2; exit 1; }

# Verify passwordless sudo for the orchestrator (so the sweep runs unattended).
if ! sudo -n true 2>/dev/null; then
    echo "[sweep] WARNING: passwordless sudo not available -- runs will prompt, and an" >&2
    echo "        unattended (nohup) sweep WILL stall. Add the NOPASSWD sudoers entry (see header)." >&2
fi

# ---- start the server (reuse one if already up) ----
if server_up; then
    echo "[sweep] server already reachable on :8080 -- reusing it (not starting a new one)"
else
    echo "[sweep] starting Flask coordinator (conda env '$CONDA_ENV', core $SERVER_CORE) -> $SERVER_LOG"
    if [ ! -f "$CONDA_SH" ]; then
        echo "[sweep] ERROR: conda.sh not found at $CONDA_SH (set CONDA_SH in this script)" >&2
        exit 1
    fi
    # set +u inside the subshell: conda's scripts reference unbound vars (e.g. $PS1) and
    # would abort under the script's `set -u`.
    # We invoke the env's python by its EXPLICIT prefix ($CONDA_PREFIX/bin/python, set by
    # `conda activate`) rather than the bare `python`: if the caller's shell has another env
    # (e.g. PC37) active with an inconsistent PATH, a plain `python` can resolve to the wrong
    # interpreter (one without flask). $CONDA_PREFIX always points at the activated env.
    # shellcheck disable=SC1090
    ( cd "$SERVER_DIR" \
        && set +u \
        && source "$CONDA_SH" \
        && conda activate "$CONDA_ENV" \
        && exec taskset -c "$SERVER_CORE" "$CONDA_PREFIX/bin/python" server.py ) >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 30); do server_up && break; sleep 0.5; done
    if ! server_up; then
        echo "[sweep] ERROR: server did not come up on :8080 (see $SERVER_LOG)" >&2
        exit 1
    fi
    echo "[sweep] server up (pid $SERVER_PID)"
fi

echo "[sweep] NoCs: ${NOCS[*]}"

# ---- sweep ----
fail=0
for noc in "${NOCS[@]}"; do
    echo "============================================================"
    echo "[sweep] NoC=$noc   ($(date '+%Y-%m-%d %H:%M:%S'))"
    echo "============================================================"
    if ! sudo ./FingerprintOrchestrator "$noc"; then
        echo "[sweep] WARNING: orchestrator failed for NoC=$noc -- continuing" >&2
        fail=$((fail + 1))
    fi
    # Tear down the run's Chrome so the next NoC starts from a clean profile, then settle.
    sudo pkill -f 'user-data-dir=/tmp/chrome-fingerprint' >/dev/null 2>&1 || true
    sleep 5
done

echo "[sweep] done ($fail failed NoC run(s)). data under: $(cd "$SERVER_DIR" && pwd)/data"
exit 0
