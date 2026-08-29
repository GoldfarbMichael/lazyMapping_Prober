# Stage 4 — Real-browser website fingerprinting

Stage 3 fingerprinted **stress-ng** with a real-browser sampler. Stage 4 keeps that sampler
byte-identical and swaps the victim for a **real website loaded in a second tab of the same
Chrome**. Holding the measurement side fixed is the point: it makes the stress-ng → website
comparison a clean workload swap rather than two unrelated experiments.

---

## 1. Components

| Component | Role |
|---|---|
| Chrome tab A + `JavaScript/main.js` (`?mode=fingerprint`) | the **observer**: builds one lazy mapping, samples the memorygram |
| Chrome tab B (created/destroyed per sample) | the **victim**: a real website, loaded live |
| `JavaScript/server.py` | coordinator + data sink (`/fp/*`, `/collect`) |
| `stable/src/website_orchestrator.c` | the conductor: sequences samples, drives the victim tab over CDP |
| `stable/sites.txt` | the class list — `<slug><TAB><url>`, `#` to comment out |
| `stable/run_website_sweep.sh` | one orchestrator run per NoC, then finalize |

Both tabs live in **one** Chrome. This is the Shusterman et al. (USENIX Sec '19) threat model:
attacker JS and victim page are tabs in the same browser. `--site-per-process` guarantees the
victim gets its own renderer, as it would in a real deployment.

---

## 2. What differs from Stage 3 — and why

### 2.1 No CPU pinning, therefore no root

Stage 3 pins Chrome to core 0 and stress-ng to core 1. Stage 4 pins **nothing**: Chrome is free
to spread across all cores.

Consequences, in both directions:

* **It needs no root.** `sched_setaffinity` was the only thing requiring it. So Chrome keeps its
  **sandbox** (no `--no-sandbox`), there is no `xhost +SI:localuser:root` step, and no NOPASSWD
  sudoers entry. That matters here in a way it did not in Stage 3: this arm navigates to
  arbitrary live websites, and a root Chrome with `--no-sandbox` would turn any drive-by into a
  root compromise.
* **The channel is no longer strictly LLC-only.** Verified topology (`lscpu -e`): SMT off, 8
  distinct physical cores, private L1/L2 (`index2/shared_cpu_list` = self), L3 shared across
  0-7. Unpinned, the two renderers may transiently co-reside on one core and share its private
  L1/L2, and the sampler may migrate mid-trace. With two busy renderers on 8 otherwise-idle
  cores that is a tail effect, but the honest claim for this arm is **microarchitectural
  contention**, not LLC contention — the same caveat that applies to the cache-occupancy
  channel in the literature. **Stages 1-3 remain the controlled upper bound.**

### 2.2 A page load is a transient, so the trace must be aligned to navigation

stress-ng is stationary: any window of a steady state is equivalent, which is why Stage 3 can
start the stressor, wait 50 ms, and then sample. A page load is not — the discriminative signal
is concentrated in the first seconds after navigation.

So the per-sample order inverts, and gains a new gate:

```
1. GET :9222/json/version          CDP health check, OUTSIDE the trace window
2. POST /fp/cmd {sample}           attacker tab is still foreground here
3. block on /fp/state started_seq  sampler is now INSIDE its synchronous loop
4. PUT :9222/json/new?<url>        navigation starts; t = 0 of the trace
5. sleep(TST) + margin, block on done_seq
6. GET :9222/json/close/<id>       kills the victim renderer and its memory cache
7. cooldown
```

**Step 3 is load-bearing.** Creating the victim tab backgrounds the attacker tab, and Chrome
throttles background-tab timers (~1/min, then freezing). `sampleMemorygram()` is a synchronous
main-thread loop and is immune *once running*, but the `/fp/poll` idle loop that starts it is
not — a throttled poll could delay sampling by up to a minute and put the page load somewhere
in the middle of the trace, or outside it entirely. `main.js` therefore POSTs `/fp/started`
immediately before entering the loop, and the orchestrator waits for it before navigating.
The throttling-disable Chrome flags are belt-and-braces on top of that ordering.

Measured on this machine: **~45 ms** from sample request to navigation — 31 ms of it waiting
for the browser to pick the request up (`main.js` polls `/fp/poll` on a 20 ms cadence) and 14 ms
to open the tab. That is 0.75% of a 6 s trace. The orchestrator prints the figure once per run
and warns if it ever exceeds 150 ms, so drift over a 30 h run cannot go unnoticed.

That the alignment works is visible in the data (pilot, NoC=16, one sample each):

| slot | example.com | wikipedia | github |
|-----:|------------:|----------:|-------:|
| 0 | 199,969 | 178,571 | 173,408 |
| 4 | 258,210 | 164,228 | 191,880 |
| 8 | 269,899 | 180,844 | 155,149 |
| 12 | 269,842 | 231,356 | 128,239 |
| 24 | 264,600 | 232,009 | 142,459 |
| 44 | 261,540 | 231,412 | 130,658 |

Higher count = less contention. `example.com` shows a brief dip at the head and then idles;
`wikipedia` shows a longer transient that settles by slot ~12; `github` stays contended for the
whole 6 s. The transients sit at the **head** of the trace, which is what alignment buys.

### 2.3 The victim is driven over CDP, not fork/exec

No Selenium, no chromedriver, no puppeteer. Chrome's DevTools **HTTP** endpoint is enough, and
`fp_ctl.c`'s raw-socket helpers already speak it.

Two things that are easy to get wrong, both verified against Chrome 150.0.7871.124:

* **`PUT /json/new?<url>` takes the URL as the RAW QUERY STRING.** The natural-looking
  `?url=<url>` form is accepted and silently opens `about:blank`. The method must be PUT
  (it was GET before Chrome 111).
* **Chrome's DevTools server ignores `Connection: close`** and keeps the socket open. A read
  loop that waits for EOF therefore blocks forever. `fp_ctl.c` stops at `Content-Length`
  (with `SO_RCVTIMEO` as a backstop) — the same reason `curl` works against it.

### 2.4 The class list is a file, not a compiled array

`stable/sites.txt`, read at startup: `<slug><TAB><url>`, `#` comments. Comment sites out to
trim a run — no rebuild.

The **slug is the class label**. It becomes the directory name at `/collect` and the key in the
`.h5` `label_map` the off-machine classifier reads, so it must match `[A-Za-z0-9._-]+`.
A URL must never be used as a slug: `os.path.join("data", cfg, "https://x.com")` builds a
nested `https:/x.com/` directory that `csv_to_h5.py`'s one-level class scan cannot see. The
orchestrator rejects bad slugs, duplicates, and malformed lines with a line number before
Chrome is launched; `server.py` re-validates independently at `/collect`, `/set-metadata`, and
`/fp/cmd` (the last **without** advancing `seq`, so a rejection cannot silently burn a sample).

---

## 3. Output and finalize

```
JavaScript/data/website_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles/<site-slug>/<n>.csv
```

The `website_` tag keeps this tree disjoint from Stage 3's `realbrowser_` tree and from the bare
`{NoC}C_...` dir a manual single-shot browser run writes under the same root. `finalize_website.sh`
packs it (`CLOCK_LABEL="website"`); `finalize_lib.sh` and `csv_to_h5.py` are unchanged.

Every CSV in one config dir must have identical `(T, G)` — a hard `csv_to_h5.py` invariant.
Sites with different load durations still satisfy it, because `T = floor(TST_ms / (Q · NoC))` is
fixed by the sampling tuple, not by the victim.

---

## 4. Shared machinery

Stage 3 and Stage 4 now share their plumbing, mirroring the existing `finalize_lib.sh` split:

| Shared | Stage 3 front-end | Stage 4 front-end |
|---|---|---|
| `src/fp_ctl.c` — HTTP + JSON, port-parameterized | `fingerprint_orchestrator.c` | `website_orchestrator.c` |
| `sweep_lib.sh` — `run_sweep()` | `run_fingerprint_sweep.sh` (107 lines) | `run_website_sweep.sh` |
| `finalize_lib.sh` — `finalize_run()` | `finalize_realbrowser.sh` | `finalize_website.sh` |

Extracting `fp_ctl.c` also fixed a latent Stage 3 bug: `request_sample` built its JSON body in a
`char[256]` with no escaping, so a long or quoted workload truncated into a body Flask parses as
`{}` — the workload then falls back to `"manual"` and **every class collapses into one
directory**. Buffers are now sized from the actual strings and values are escaped.

---

## 5. Running it

```bash
cd stable
./run_website_sweep.sh                 # full sweep + finalize
DO_FINALIZE=0 ./run_website_sweep.sh   # collect only, keep CSVs

# single NoC, manual (server must already be up):
./WebsiteOrchestrator 16C_6TST_180K_2288cycles 50 500000 sites.txt

curl -s localhost:8080/fp/state        # health check
curl -s localhost:9222/json/list       # what tabs are open
```

### Wall-time budget

~8 s/sample (6 s TST + 0.5 s ack margin + 0.5 s cooldown + ~1 s victim setup), full 7-NoC sweep:

| classes × samples | samples | wall time |
|---|---:|---:|
| 20 × 50 | 7,000 | ~16 h |
| 38 × 50 | 13,300 | ~30 h |
| 100 × 50 | 35,000 | ~78 h |
| 100 × 100 | 70,000 | ~155 h |

`sites.txt` ships with 106 sites. 100 sites across all 7 NoCs is not practical in one pass —
comment it down to ~20-38 sites for the full NoC sweep (that curve is the thesis result), then
do one 100-site run at the best NoC for a literature-comparable WF number.

---

## 6. Known limitations

1. **No pinning** (§2.1): the claim is microarchitectural, not strictly LLC, contention.
2. **Live loads**: network, CDN and ad variance are confounds. Round-robin ordering (outer loop
   = iteration, inner = site) spreads that drift across classes instead of aliasing it onto one.
3. **Cache state is only partially reset between samples.** `--disk-cache-size=1` plus a fresh
   renderer per tab suppresses the HTTP disk cache and the per-page memory cache, but DNS
   cache, TLS session resumption and connection reuse stay warm for the life of the browser.
   A fully cold load would need a profile reset per sample, which is incompatible with keeping
   the attacker tab (and its one lazy mapping) alive.
4. **All of Chrome's processes contend with the sampler**, inherited from Stage 3 §8 and worse
   here, since the victim renderer is now inside the same browser.
