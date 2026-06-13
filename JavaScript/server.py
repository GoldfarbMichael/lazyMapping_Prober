from flask import Flask, request, send_from_directory, jsonify
from flask_cors import CORS
import os

app = Flask(__name__)
CORS(app, resources={
    r"/collect": {"origins": ["http://127.0.0.1:8080", "http://localhost:8080"]},
    r"/set-metadata": {"origins": ["http://127.0.0.1:8080", "http://localhost:8080"]}
})

DATA_ROOT = "data"
# Resolved from /set-metadata; data is written to data/{config}/{workload}/{n}.csv
current_config = "manual"    # default fallback (e.g. "64C_2TST_45K_2288cycles")
current_workload = "manual"  # default fallback (e.g. "qsort")

# Coverage-validation coordinator state: the C prober (driver) sets the cluster the
# browser should hammer; the browser polls it. -1 = idle/baseline, -2 = stop.
ctl_cluster = -2


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


# ---- Coverage-validation coordinator ----
# The C prober drives the cluster index; the browser (mode=validate) polls it.

@app.route("/ctl/set", methods=["POST"])
def ctl_set():
    # C driver sets which cluster the browser should hammer (-1 idle, -2 stop).
    global ctl_cluster
    data = request.get_json(force=True, silent=True) or {}
    ctl_cluster = int(data.get("cluster", -2))
    print(f"ctl: cluster -> {ctl_cluster}")
    return jsonify(status="ok", cluster=ctl_cluster), 200


@app.route("/ctl/poll", methods=["GET"])
def ctl_poll():
    # Browser polls the current cluster to hammer.
    return jsonify(cluster=ctl_cluster), 200


if __name__ == "__main__":
    # run on http://localhost:8080
    app.run(host="0.0.0.0", port=8080, debug=True)
