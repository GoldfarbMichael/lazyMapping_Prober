# Stage 3 — Real-Browser stress-ng Fingerprinting

How the spatio-temporal LLC primitive is run **inside a real Chrome browser** (JavaScript
sampling) while a C orchestrator drives the experiment and injects the stress-ng workload.
This reproduces the native C battery (`runStressNG_batches` in `stable/src/mastikElite.c`)
but moves the memorygram sampling from C into the browser, under real browser timing.

---

## 1. Components

| Component | Role | Where |
|-----------|------|-------|
| **Chrome + `main.js`** | The *observer*. Builds the lazy mapping once and samples the spatio-temporal memorygram. | `JavaScript/main.js`, `?mode=fingerprint` |
| **Flask server** | The *coordinator* + data sink. Relays commands/status between C and JS, and writes the CSVs. | `JavaScript/server.py` |
| **C orchestrator** | The *conductor*. Launches Chrome, runs the stress-ng battery, sequences samples. | `stable/src/fingerprint_orchestrator.c` → `FingerprintOrchestrator` |
| **stress-ng** | The *victim* workload being fingerprinted (38 stressors). | forked by the orchestrator |
| **Sweep script** | Starts the server and runs the orchestrator across all NoC values. | `stable/run_fingerprint_sweep.sh` |

### Core assignment
- **Core 0** — Chrome (the JS sampler). All Chrome child processes inherit this pin.
- **Core 1** — stress-ng (the victim).
- **Core 2** — the orchestrator and the Flask server (kept off the measurement/victim cores).

The channel is the **shared inclusive LLC (L3)**: the victim on core 1 contends on the same
physical cache sets the browser probes on core 0, so the browser indirectly observes the
victim's cache activity. No shared memory between the processes is required.

---

## 2. The measurement (JS side)

The browser builds **one lazy mapping** on page load and reuses it for the entire run
(`LazyMapping`): a page-aligned buffer sized to the real LLC geometry, partitioned into
`NoC` spatial clusters by the translation-invariant cache-index bits 6–11, with each
cluster stored as a circular pointer-chase list.

For each sample, `sampleMemorygram()` produces a **memorygram** — a matrix of
`[time slots × clusters]`. For every time slot it sweeps each cluster for a fixed quantum
`Q`, counting completed accesses (higher count = less contention). One CSV per sample:
header `G0..G{NoC-1}`, one row per time slot.

**Key constraint: no network I/O during sampling.** `sampleMemorygram()` is synchronous and
issues zero `fetch` calls, so HTTP traffic never perturbs a measurement window. All network
exchange happens strictly *between* samples.

---

## 3. Coordination protocol (`/fp/*` endpoints)

A bidirectional handshake over the Flask server, using a monotonic **`seq` token** so each
sample request produces exactly one CSV (race-free).

Server state: `fp = {seq, cmd, workload, ready, done_seq}`.

| Endpoint | Caller | Purpose |
|----------|--------|---------|
| `POST /fp/reset` | C | At startup, before launching Chrome — clears stale state. |
| `POST /fp/ready` | JS | Mapping built; observer is armed. |
| `GET /fp/poll` | JS | Read the current command `{seq, cmd, workload}`. |
| `POST /fp/cmd {cmd,workload,config}` | C | `sample` → bump `seq` + set the `/collect` path; `stop` → end. |
| `POST /fp/done {seq}` | JS | Sample taken and CSV saved. |
| `GET /fp/state` | C | Read `{ready, seq, done_seq}`. |
| `POST /collect` | JS | Body = the memorygram CSV; server writes it to disk. |

The browser only acts on a `sample` command whose `seq` is newer than the last one it
handled (`seq > lastSeq`), which prevents it from re-sampling while the command is still
`"sample"` between its `done` ack and the orchestrator's next request.

---

## 4. Per-sample flow

```
C: POST /fp/reset                         (clear stale state)
C: launch Chrome (?mode=fingerprint) on core 0
JS: build mapping -> POST /fp/ready
C: poll /fp/state until ready

   ── for each sample ──
C:  fork + exec stress-ng, pinned to core 1
C:  wait ~50 ms (let the stressor reach steady state)
C:  POST /fp/cmd {sample, workload}  -> gets seq
JS: /fp/poll sees the new seq -> sampleMemorygram()   (NO network)
JS: POST /collect (CSV)  ->  POST /fp/done {seq}
C:  (sleep ~FP_TST so the orchestrator is idle during the window),
    then poll /fp/state until done_seq == seq
C:  SIGKILL the stressor + reap
C:  cooldown 5 s (let the L3 return to baseline)
   ─────────────────────

C: POST /fp/cmd {stop}  ->  tear down Chrome
```

---

## 5. Round-robin ordering (anti-drift)

Sampling is **stressor-by-stressor**, not batch-by-stressor:

```
for iter in 0..NUM_SAMPLES-1:        # outer = iteration
    for stressor in battery:         # inner = stressor
        collect one sample
```

So each stressor's `NUM_SAMPLES` samples are spread across the whole run instead of taken
in one contiguous block. This prevents the classifier from fingerprinting a slowly drifting
machine state instead of the stressor itself.

---

## 6. Configuration & output

- **NoC** is a command-line argument to the orchestrator (`./FingerprintOrchestrator <NoC>`),
  a power of two in `[1, 64]`. One lazy mapping per invocation.
- **`NUM_SAMPLES`** (samples per stressor) is a `#define` in the orchestrator (default 50).
- Sampling params baked into the Chrome URL label: `FP_TST` (sampling seconds),
  `FP_K`, `FP_CYCLES` → label `fp_{NoC}C_{TST}TST_{K}K_{cycles}cycles`.
- The orchestrator passes a `config` string `realbrowser_{NoC}C_{TST}TST_{K}K_{cycles}cycles`
  with each sample command; the server writes to:

  ```
  JavaScript/data/realbrowser_{NoC}C_{TST}TST_{K}K_{cycles}cycles/<stressor>/<n>.csv
  ```

  with `<n>` auto-incremented (re-runs append). This is distinct from the native C tool's
  `stable/data/{native,chrome}_clock/...`.

---

## 7. Running it

```bash
cd stable
./run_fingerprint_sweep.sh        # starts the server (conda base, core 2),
                                  # then sweeps NoC = 1 2 4 8 16 32 64
```

The script grants root access to the `:0` X display (`xhost`), builds the orchestrator,
starts the Flask coordinator, and runs `sudo ./FingerprintOrchestrator <NoC>` for each NoC.
Run it as the normal user (it `sudo`s only the orchestrator, which pins cores and launches
Chrome as root). A single Ctrl-C stops the sweep and tears everything down.

To run one NoC directly (server must already be up):

```bash
sudo ./FingerprintOrchestrator 64
```

Health check while running: `curl -s localhost:8080/fp/state` — `seq` climbs by one per
sample and `done_seq` tracks it.

---

## 8. Known caveat

In the native C tool the measuring prober sits alone on core 0. Here, **all** of Chrome's
processes (renderer + GPU + browser + network) share core 0, so the measuring renderer
contends with Chrome's own processes. This is inherent to real-browser operation and
reduces SNR versus the native upper bound — it should be noted when comparing Stage 3
results against Stages 1–2.
