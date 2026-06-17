#!/usr/bin/env bash
# Sweep the JS lazy-mapping coverage validator across NoC values, N iterations each.
#
# Usage: ./run_coverage_sweep.sh <iterations_per_noc> [noc]
#   <iterations_per_noc>  number of runs per NoC (required, >=1)
#   [noc]                 optional: run ONLY this NoC (power of two in 2..64).
#                         Omitted -> sweep 2 4 8 16 32 64.
#
# Each run -> CoverageValidator writes data/coverage/NoC{nn}/{iter}.csv
# (+ the shared data/coverage/set_labels.csv). The C tool auto-picks the spin window
# from NoC (12800/NoC us).
#
# Run this AS YOUR NORMAL USER (not via sudo): it needs xhost for your X session, and
# calls sudo itself for each CoverageValidator run (which needs hugepages/pagemap).
# A run whose output already exists is SKIPPED, so re-running resumes after a failure.
#
# For UNATTENDED runs (nohup &), set up passwordless sudo so it never needs a tty -- Ubuntu
# binds the sudo ticket to the controlling terminal (tty_tickets), so a cached credential
# dies the moment the terminal closes. Create /etc/sudoers.d/coverage_validator with
# (adjust user/path):
#     ubu ALL=(root) NOPASSWD: /home/ubu/Desktop/Michael/lazyMapping_Prober/stable/CoverageValidator
#     ubu ALL=(root) NOPASSWD: /usr/bin/pkill -f user-data-dir=/tmp/chrome-validate
# then `sudo chmod 0440` it and `sudo visudo -c` to validate.
# Requires the Flask coordinator already running on :8080.
set -uo pipefail

cd "$(dirname "$0")"   # stable/

ITERS="${1:-}"
ONLY_NOC="${2:-}"
if ! [[ "$ITERS" =~ ^[0-9]+$ ]] || [ "$ITERS" -lt 1 ]; then
    echo "usage: $0 <iterations_per_noc> [noc]" >&2
    exit 2
fi

# ALL_NOCS=(2 4 8 16 32 64)
ALL_NOCS=(64 32 16 8 4 2)

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

server_up() {
    if command -v curl >/dev/null 2>&1; then
        curl -s -o /dev/null --max-time 2 "http://127.0.0.1:8080/ctl/poll"
    else
        (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null
    fi
}

# ---- pre-flight ----
echo "[sweep] granting root access to X display :0 (xhost)"
xhost +SI:localuser:root >/dev/null 2>&1 || \
    echo "[sweep] WARNING: xhost failed -- Chrome may not reach :0"

echo "[sweep] building CoverageValidator"
make CoverageValidator || { echo "[sweep] build failed" >&2; exit 1; }

if ! server_up; then
    echo "[sweep] ERROR: Flask coordinator not reachable on :8080." >&2
    echo "        start it first:  python3 ../JavaScript/server.py" >&2
    exit 1
fi
echo "[sweep] server reachable on :8080"

# Verify sudo won't block on a password. With the NOPASSWD sudoers entry this passes and
# the sweep can run fully unattended (nohup); without it, a detached job would stall/fail
# once the tty-bound credential expires.
if ! sudo -n true 2>/dev/null; then
    echo "[sweep] WARNING: passwordless sudo not available. Foreground runs will prompt;"
    echo "        unattended (nohup) runs WILL fail after the tty closes -- add the"
    echo "        NOPASSWD sudoers entry (see header) for unattended sweeps." >&2
fi

echo "[sweep] NoCs: ${NOCS[*]}   iterations each: $ITERS"

# ---- sweep ----
fail=0
for noc in "${NOCS[@]}"; do
    for ((i = 0; i < ITERS; i++)); do
        out="data/coverage/NoC$(printf '%02d' "$noc")/$(printf '%03d' "$i").csv"
        if [ -s "$out" ]; then
            echo "[sweep] skip NoC=$noc iter=$i (exists: $out)"
            continue
        fi
        echo "============================================================"
        echo "[sweep] NoC=$noc iter=$i   ($(date '+%Y-%m-%d %H:%M:%S'))"
        echo "============================================================"
        if ! sudo ./CoverageValidator "$noc" "$i"; then
            echo "[sweep] WARNING: run failed (NoC=$noc iter=$i) -- continuing"
            fail=$((fail + 1))
        fi
        # clean up the validate Chrome profile so the next run starts fresh (root-owned,
        # since CoverageValidator forked it as root)
        sudo pkill -f 'user-data-dir=/tmp/chrome-validate' >/dev/null 2>&1 || true
        sleep 3
    done
done

# the sudo'd binary wrote root-owned CSVs; hand them back to the invoking user
sudo chown -R "$(id -u):$(id -g)" data/coverage 2>/dev/null || true

echo "[sweep] done ($fail failed run(s)). data under: $(pwd)/data/coverage"
exit 0
