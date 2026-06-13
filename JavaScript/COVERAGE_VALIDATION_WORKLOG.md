# Worklog — Coverage & Orthogonality Validation of the JS Lazy Mapping

Covers everything built **after** the lazy-mapping/sweep implementation (which you already
reviewed). Goal of this phase: empirically check whether the JS lazy clusters are
**orthogonal** (different clusters occupy disjoint physical LLC sets) and give good
**coverage** (together they hit most of the LLC, few cold sets), using Mastik's real
eviction sets as ground truth.

Full design lives in the plan file: `~/.claude/plans/crystalline-whistling-walrus.md`.

---

## 1. The core idea & the "sync" answer

- JS (browser) and the C/Mastik tool are **separate processes with different virtual→physical
  mappings**; they cannot share a buffer. We do **not** share memory.
- They share the **physical LLC**, which is **inclusive** on the i7-9700k (Coffee Lake). So a
  line evicted from the LLC is evicted from all cores — classic cross-core Prime+Probe
  (Liu S&P'15, Maurice NDSS'17). The two processes contend on the same physical sets.
- **"Sync" = temporal coordination only**: who hammers which cluster, and when. Done via the
  Flask server, at cluster granularity (16 handshakes), never inside the tight probe loop.
- Having **JS** be the one that touches memory is deliberate: it validates the *actual V8
  buffer's* physical behaviour (does V8 page-align? are clusters physically clean?), which an
  all-C replication could never test.

### Measurement (classic P+P, address-by-address)
For each lazy cluster `c`: the browser hammers cluster `c` continuously; the C prober scans
every Mastik set one at a time and counts JS-induced misses. A set that misses while `c` is
hammered ⇒ cluster `c` physically contends on that set.
- Per set: `l3_unmonitorall` → `l3_monitor_manual(i, e_sets[i])` → `l3_bprobecount` (the
  `get_transTable` pattern). `bprobecount` is the **combined prime+probe** — backward
  traversal both re-primes and counts misses (replacement-policy trick). One set's 12 lines
  at a time = low noise (vs. priming the whole cache).
- `get_active_group()` reports each cluster's dominant Mastik **group** (Mastik partitions
  16384 sets into 16 groups of 1024) and the **% concentration**.

### Interpretation
- Start at **NoC = 16** vs **16 Mastik groups** → a 16×16 cluster→group activity matrix.
- **Ideal = a permutation**: each cluster maps to one distinct group with high % → orthogonal
  + full coverage. Cross-cluster overlap ⇒ misalignment / unclean clusters. Many 0-miss sets
  ⇒ coverage holes. Either is a real, publishable finding.

---

## 2. What was built (files)

### `JavaScript/server.py` — coordinator (ADDED, nothing removed)
- New state `ctl_cluster` (default `-2`).
- `POST /ctl/set {cluster}` — C driver sets which cluster the browser hammers (`-1` = idle
  baseline, `-2` = stop).
- `GET /ctl/poll` — browser reads the current cluster.
- `/collect`, `/set-metadata`, hierarchical CSV storage unchanged.

### `JavaScript/main.js` — validate/hammer mode (ADDED)
- `CTL_POLL_URL`, `?mode=validate` dispatch at the bottom (normal sampling path untouched).
- `runValidation()`: builds the same `LazyMapping`, then loops — poll `/ctl/poll`; hammer the
  returned cluster for 100 ms bursts (`hammerCluster`, reusing the `curr=buffer[curr]` chase);
  `-1` stays idle; `-2` stops.

### `stable/src/coverage_validator.c` — the C prober (NEW FILE)
- Pins to **core 0**; loads the real mapping via
  `load_mapping_and_eSetsFrom_BIN_file("/dev/hugepages/map_A", "mapping_A.bin")`.
- **Launches Chrome itself**: `fork` → pin **core 1** → `DISPLAY=:0 google-chrome
  --user-data-dir=/tmp/chrome-validate ".../?mode=validate&label=cov_16C_2TST_45K_2288cycles"`.
- Per cluster: `ctl_set(c)` (raw-socket HTTP POST, no libcurl dependency) → ramp → scan all
  sets (`probe_set`: monitor one set, warm prime, then `bprobecount` after a `DELAY_CYCLES`
  spin, median of `REPS`) → `get_active_group`.
- Baseline pass (JS idle) for the noise floor → `ctl_set(-2)` → kill Chrome.
- Outputs: `coverage_miss_matrix.csv` (NoC rows + a baseline row × numSets) and
  `coverage_set_labels.csv` (`set_idx, pa, bits8_11` from the BIN physical addresses, so the
  miss matrix can be re-grouped by true physical bits 8-11 in Python).
- Tunables (`#define`): `REPS=3`, `DELAY_CYCLES=150000` (~40 µs), `RAMP_MS=400`,
  `MASTIK_GROUPS=16`, cores 0/1.

### `stable/Makefile` — (ONLY ADDED)
- `COV_TARGET/COV_SRCS/COV_OBJS`, a `CoverageValidator` link rule (reuses
  utils/tests/mastikElite/murmur3 objects + its own main), and extended `clean`. The
  `MastikElite` target and all existing vars/rules are untouched.

---

## 3. Verified vs. NOT verified

Verified here:
- Flask `/ctl/set` ↔ `/ctl/poll` handshake (curl: poll `-2` → set 5 → poll 5 → set `-1`).
- `main.js` serves the `runValidation` dispatch; JS syntax clean.
- `coverage_validator.c` **compiles and links** clean (`make CoverageValidator`).

NOT verified (needs root + the hugepage BIN mapping + the X/VNC display — only you have these):
- The actual cross-process run and the resulting miss matrix / concentration numbers.
- That Chrome's **renderer** (not just the launcher) lands on core 1.

---

## 4. How to run (your side)

```bash
# server already runs on :8080 (mode=validate is served)
cd stable
sudo ./CoverageValidator 16          # opens Chrome on the VNC display, scans clusters 0..15 + baseline
# -> writes stable/coverage_miss_matrix.csv and stable/coverage_set_labels.csv
```
Console shows `cluster c -> dominant Mastik group g` with `% activity` per cluster.

---

## 5. Open knobs / risks to check tomorrow

1. **Renderer pinning** — affinity inheritance to Chrome's renderer process isn't guaranteed
   across versions. Check `ps -eLo psr,comm | grep -i chrome`; if it's not on core 1, the
   launch needs an explicit per-renderer pin.
2. **`DELAY_CYCLES` / `REPS`** — first guesses for the prime→probe window. Weak concentration
   ⇒ increase the delay (more JS eviction per set).
3. **set-id groups vs physical bits 8-11** — `get_active_group` groups by Mastik set-id/1024,
   which may not equal the lazy cluster's bits-8-11 field. The `coverage_set_labels.csv` lets
   you re-group by true physical bits 8-11 in Python for an apples-to-apples check (the
   definitive orthogonality test).
4. **Baseline subtraction** — attribute misses to JS by subtracting/thresholding the baseline
   row before computing coverage/orthogonality.
5. Not yet written (offered): the Python analysis (load both CSVs → baseline-subtract →
   cluster×group heatmap + permutation/coverage/orthogonality metrics).
