from flask import Flask, request, send_from_directory, jsonify
from flask_cors import CORS
import os
import json
import time

app = Flask(__name__)
CORS(app, resources={
    r"/collect": {"origins": ["http://127.0.0.1:8080", "http://localhost:8080"]},
    r"/set-metadata": {"origins": ["http://127.0.0.1:8080", "http://localhost:8080"]}
})

DATA_ROOT = "data"
# Resolved from /set-metadata; data is written to data/{config}/{workload}/{n}.csv
current_config = "manual"    # default fallback (e.g. "64C_2TST_45K_2288cycles")
current_workload = "manual"  # default fallback (e.g. "qsort")

# Coverage-validation coordinator state: one-way (C prober -> browser). The C prober
# publishes which cluster the browser should sweep; the browser polls and sweeps it
# continuously, never acking. -1 = idle (baseline), -2 = stop. Default IDLE so a page
# that loads before the prober's first /ctl/set stays idle instead of exiting.
ctl_cluster = -1

# Fingerprinting coordinator state: bidirectional handshake (C orchestrator <-> browser).
# The browser builds the lazy mapping once and reports "ready"; thereafter the C side
# issues sample requests (bumping "seq") while a stress-ng workload runs on another core,
# the browser samples the memorygram with NO network in the loop, POSTs the CSV to
# /collect, then acks /fp/done with that seq. The seq token makes the handshake race-free
# (exactly one /collect per /fp/cmd). "workload" rides along so /collect's path is right.
fp = {"seq": 0, "cmd": "idle", "workload": "", "ready": False, "done_seq": -1}


def next_index(directory):
    """Next CSV index for a directory: max existing numeric stem + 1 (0 if none).
    Robust to gaps from deletions."""
    if not os.path.isdir(directory):
        return 0
    indices = []
    for name in os.listdir(directory):
        stem, ext = os.path.splitext(name)
        if ext == ".csv" and stem.isdigit():
            indices.append(int(stem))
    return max(indices) + 1 if indices else 0


@app.route("/favicon.ico")
def favicon():
    return "", 204  # Return empty response with "No Content" status


# serve the HTML
@app.get("/")
def index():
    # assumes: JavaScript/
    #          ├─ index.html
    #          ├─ main.js
    #          ├─ worker.js
    #          └─ server.py
    return send_from_directory("", "index.html")


# Sweep-time calibration page: same index.html (main.js detects the path and runs the
# /checkSweepingTime measurement instead of the memorygram experiment).
@app.get("/checkSweepingTime")
def check_sweeping_time():
    return send_from_directory("", "index.html")


# serve other static assets (main.js, worker.js, ...) from this directory
@app.get("/<path:filename>")
def static_files(filename):
    return send_from_directory("", filename)


@app.route("/set-metadata", methods=["POST"])
def set_metadata():
    # No filesystem work here (runs before sampling) -- just record where the
    # next CSV will go. Counting + writing happen at /collect, after sampling.
    global current_config, current_workload
    data = request.get_json(force=True, silent=True) or {}

    current_config = data.get("config", "manual")
    current_workload = data.get("workload", "manual")

    print(f"Metadata set - config: {current_config}, workload: {current_workload}")
    return jsonify(status="ok", config=current_config, workload=current_workload), 200


@app.route("/collect", methods=["POST"])
def collect():
    # Runs AFTER sampling: body is the raw CSV memorygram (header + one row per
    # time slot). All filesystem work (dir scan for the next index + write) is
    # done here so none of it overlaps the sampling window.
    csv_text = request.get_data(as_text=True)

    directory = os.path.join(DATA_ROOT, current_config, current_workload)
    os.makedirs(directory, exist_ok=True)

    n = next_index(directory)
    log_file = os.path.join(directory, f"{n}.csv")
    with open(log_file, "w") as f:
        f.write(csv_text)

    print(f"CSV written to: {log_file} ({len(csv_text)} bytes)")
    return jsonify(status="ok", path=log_file), 200


@app.route("/saveSweepTimes", methods=["POST"])
def save_sweep_times():
    # /checkSweepingTime POSTs its full result here so the data is persisted to disk
    # (no client download needed -- the operator can disconnect the remote UI). Writes
    # both a complete .json (meta + summary + every sample) and a .csv (raw samples).
    payload = request.get_json(force=True, silent=True) or {}
    meta = payload.get("meta", {})
    summary = payload.get("summary", {})
    samples = payload.get("perPassMs", [])

    directory = os.path.join(DATA_ROOT, "sweep_times")
    os.makedirs(directory, exist_ok=True)
    base = f"sweeptime_NoC{meta.get('noc', 'NA')}_{time.strftime('%Y%m%d_%H%M%S')}"

    json_path = os.path.join(directory, base + ".json")
    with open(json_path, "w") as f:
        json.dump(payload, f, indent=2)

    csv_path = os.path.join(directory, base + ".csv")
    with open(csv_path, "w") as f:
        for k, v in {**meta, **summary}.items():
            f.write(f"# {k},{v}\n")   # metadata + summary as leading comment lines
        f.write("sample_index,per_pass_ms\n")
        for i, v in enumerate(samples):
            f.write(f"{i},{v}\n")

    print(f"Sweep times written: {json_path} / {csv_path} ({len(samples)} samples)")
    return jsonify(status="ok", json=json_path, csv=csv_path, samples=len(samples)), 200


# ---- Coverage-validation coordinator (one-way: C prober -> browser) ----

@app.route("/ctl/set", methods=["POST"])
def ctl_set():
    # C driver publishes which cluster the browser should sweep (-1 idle, -2 stop).
    global ctl_cluster
    data = request.get_json(force=True, silent=True) or {}
    ctl_cluster = int(data.get("cluster", -1))
    print(f"ctl: cluster -> {ctl_cluster}")
    return jsonify(status="ok", cluster=ctl_cluster), 200


@app.route("/ctl/poll", methods=["GET"])
def ctl_poll():
    # Browser reads the current cluster to sweep.
    return jsonify(cluster=ctl_cluster), 200


# ---- Fingerprinting coordinator (bidirectional: C orchestrator <-> browser) ----

@app.route("/fp/ready", methods=["POST"])
def fp_ready():
    # Browser: lazy mapping built, ready to receive sample requests.
    fp["ready"] = True
    print("fp: browser ready (mapping built)")
    return jsonify(status="ok"), 200


@app.route("/fp/poll", methods=["GET"])
def fp_poll():
    # Browser polls for the next command (between samples; never during sampling).
    return jsonify(seq=fp["seq"], cmd=fp["cmd"], workload=fp["workload"]), 200


@app.route("/fp/done", methods=["POST"])
def fp_done():
    # Browser acks that it sampled + saved the CSV for the given seq.
    data = request.get_json(force=True, silent=True) or {}
    fp["done_seq"] = int(data.get("seq", -1))
    print(f"fp: done seq -> {fp['done_seq']}")
    return jsonify(status="ok", done_seq=fp["done_seq"]), 200


@app.route("/fp/cmd", methods=["POST"])
def fp_cmd():
    # C orchestrator: request a sample (with its workload/config so /collect's path is
    # correct) or signal stop. "sample" bumps seq so the browser fires exactly once.
    global current_config, current_workload
    data = request.get_json(force=True, silent=True) or {}
    cmd = data.get("cmd", "idle")
    if cmd == "sample":
        # Set the /collect path globals BEFORE advancing seq so the browser never samples
        # against a stale workload/config.
        current_workload = data.get("workload", "manual")
        current_config = data.get("config", "manual")
        fp["workload"] = current_workload
        fp["cmd"] = "sample"
        fp["seq"] += 1
        print(f"fp: sample seq={fp['seq']} workload={current_workload} config={current_config}")
        return jsonify(status="ok", seq=fp["seq"]), 200
    fp["cmd"] = cmd  # "stop" / "idle"
    print(f"fp: cmd -> {cmd}")
    return jsonify(status="ok", cmd=cmd), 200


@app.route("/fp/state", methods=["GET"])
def fp_state():
    # C orchestrator reads readiness + the latest acked seq.
    return jsonify(ready=fp["ready"], seq=fp["seq"], done_seq=fp["done_seq"]), 200


@app.route("/fp/reset", methods=["POST"])
def fp_reset():
    # C orchestrator calls this at startup (BEFORE launching Chrome) so a stale "ready"
    # from a previous, now-dead browser can't make wait_ready() pass before the NEW
    # browser has built its mapping. Returns the coordinator to its initial state.
    fp.update(seq=0, cmd="idle", workload="", ready=False, done_seq=-1)
    print("fp: reset")
    return jsonify(status="ok"), 200


if __name__ == "__main__":
    # run on http://localhost:8080. threaded=True so the C driver and the browser can
    # have concurrent in-flight requests without head-of-line blocking each other.
    app.run(host="0.0.0.0", port=8080, debug=True, threaded=True)
