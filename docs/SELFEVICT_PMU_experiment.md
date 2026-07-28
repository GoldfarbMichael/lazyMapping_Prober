# Self-Eviction, Measured Directly with Hardware Performance Counters

**What this document is.** A from-scratch explanation of the self-eviction experiment
added this session: *why* we built it, *what a performance counter even is*, and a
file-by-file breakdown of every change. It assumes you have **never** worked with CPU
performance counters, so every term is defined the first time it appears (there is also a
glossary at the end).

---

## 1. Why we built this

`docs/FINDINGS_coverage_insertion_policy.md` §2 already concluded *why* the lazy-map
coverage drops as NoC grows: the CPU's L3 cache uses a **scan-resistant insertion policy**,
and the victim's scattered lines end up **evicting each other** ("self-eviction") instead of
evicting the attacker's primed lines. But that conclusion was **inferred indirectly** — from
the *shape* of a histogram (the `native_shuffled` ways-evicted distribution). Nobody had
*directly measured* the victim throwing out its own lines.

This experiment does exactly that: it puts a **hardware counter** on the victim sweep and
counts, in silicon, how many of the victim's memory accesses miss the L3 cache **because the
victim just evicted its own line**. If self-eviction is real, that number is large; and it
should grow with NoC in lockstep with the coverage drop.

> **One-line summary of the claim being tested:** on a warm cluster (its lines already
> loaded), a *second* sweep should mostly hit the cache and produce ~0 misses. If instead it
> produces *many* misses, the lines didn't stay resident — the sweep evicted them itself.
> That is self-eviction, measured directly.

---

## 2. Crash course: what is a "performance counter"?

Modern Intel CPUs contain a small hardware block **inside every core** called the
**PMU — Performance Monitoring Unit**. Its job is to *count microarchitectural events*
(cache misses, branch mispredictions, instructions retired, …) while your code runs, with
**zero software overhead** — the hardware just ticks a register up by one each time the event
happens.

The registers that do the counting are called **PMCs — Performance Monitoring Counters**.
There are two kinds:

- **Fixed-function counters** — hardwired to one specific event each (e.g. "cycles").
- **Programmable / general-purpose counters** — *you* choose which event they count. A
  Skylake-family core (our i7-9700K is one) has **4 programmable counters per core**. We use
  exactly one of them.

To use a programmable counter you do three things:

1. **Tell it which event to count.** You write an *event code* into a special control
   register (see MSR / PERFEVTSEL below).
2. **Turn the counters on** (a global enable switch).
3. **Read the counter** before and after the code you care about; the difference is the
   event count for that region.

### 2.1 Events: "event number" + "umask"

An event is identified by two small numbers: an **event select** (the event family) and a
**unit mask (umask)** (which variant of it). Together they name one precise event. The one we
use is:

```
MEM_LOAD_RETIRED.L3_MISS   =   event 0xD1 , umask 0x20
```

Read it in plain English:

- **MEM_LOAD_RETIRED** — count **memory load instructions that retired**. "Retired" means the
  instruction actually **completed** (it wasn't a speculative guess the CPU later threw away).
  So this ignores speculation noise.
- **.L3_MISS** — of those loads, only the ones whose data was **not** present **anywhere
  on-die** and had to be fetched from **main memory (DRAM)**. Because this CPU's L3 is
  **shared and inclusive**, "not on-die" is a strong condition: a line held in *any* L3 slice,
  or in *any* other core's private L1/L2 (which the inclusive L3 therefore also tracks), counts
  as an **L3 hit**, not a miss — even when the data is physically forwarded from another core's
  cache (a cross-core "HITM", which Skylake classifies under the separate L3-**hit** event
  `MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM`). So on the single-socket i7-9700K an L3 **miss** means
  DRAM. (The "another core / remote socket" data source for an L3 miss only exists on
  **multi-socket** systems — `MEM_LOAD_L3_MISS_RETIRED.REMOTE_*` — and does not apply here.)
  That is exactly a **last-level-cache miss caused by a real load your program executed**.

**Why this specific event and not a generic "LLC miss" event?** Because it counts **demand**
loads only. A "demand" load is an actual load instruction in your code. The opposite is a
**prefetch** — the hardware guessing you'll need a line soon and fetching it speculatively.
Generic LLC-miss events also count prefetch-driven fills, which would pollute our number. Our
victim sweep is pure demand loads, so `MEM_LOAD_RETIRED.L3_MISS` measures *precisely* the
victim's own missed loads and nothing else.

**Precisely what "demand-only" excludes — a prefetch's fetch is invisible to this event, but
its effect on a later demand load is not.** `MEM_LOAD_RETIRED.*` fires once per **retiring
load micro-op** — i.e., a real load instruction in your code that completed. A hardware
prefetch is not an instruction; it's a fill the L2 prefetcher logic issues on its own, with no
retiring load attached to it. So if touching line `L` makes the **adjacent-line prefetcher**
(one of Coffee Lake's two L2 prefetchers, controlled by MSR `0x1A4` bit 2) speculatively fetch
`L`'s 128-byte buddy `L^1`, that fetch — hit or miss — generates **no** `MEM_LOAD_RETIRED`
event, because there is no retiring load for it to attach to.

It shows up *indirectly*, though: if the sweep's own pointer chase later reaches `L^1` (it is
one of the cluster's own nodes), that touch **is** a real load instruction, and it will be
classified normally — an **L3 hit** if the prefetch already staged the line (so it is correctly
*not* counted as a miss), or an **L3 miss** if it hadn't. This is exactly the mechanism
FINDINGS §2 identifies: at low NoC a cluster spans multiple 64B offsets, so adjacent-line
reinforcement keeps buddy lines resident and the sweep's later demand loads to them hit,
keeping `M_self` low; at NoC=64 a cluster is a single offset with no in-cluster buddy, so there
is no reinforcement and those demand loads miss, raising `M_self`. This is the *intended*
behavior — `M_self` should reflect real residency including the prefetcher's real effect on it,
not some idealized count with hardware prefetching subtracted out.

**Provenance of `0xD1 / 0x20`.** This is a **core** PMU event (a thread executes it), so it is
defined in Intel's *core* event list, not the *uncore* one. Verified against the authoritative
tables — Intel SDM Vol. 3B perfmon tables, Intel's `perfmon` repo `SKL/events/skylake_core.json`,
and the Linux kernel's `tools/perf/pmu-events/arch/x86/skylake/cache.json`, which all list
`MEM_LOAD_RETIRED.L3_MISS` as EventCode `0xD1`, UMask `0x20`. (The `skylake_uncore.json` file is
the wrong place to look: it holds CBo/CHA-slice and memory-controller events, which are counted
by a separate system-wide uncore PMU through different MSRs and are **not** per-thread, so they
cannot attribute a miss to our sweep.) Our i7-9700K is a Coffee Lake part, which Intel's mapfile
maps to the Skylake-client (`SKL`) event set — the same encodings.

**Turning a JSON event entry into the PERFEVTSEL value (the general recipe).** Any of these
event lists gives, per event, an `EventCode` and a `UMask`. To make it a counter config you
pack `EventCode` into bits 7:0, `UMask` into bits 15:8, then OR-in the mode bits you want —
`USR` (bit 16), `OS` (bit 17), `EN` (bit 22), and optionally `CMASK`/`INV`/`EDGE`/`ANYTHREAD`
from the JSON's matching fields. That packed number is what you write to `IA32_PERFEVTSEL0`.
For us: `0xD1 | (0x20<<8) | (1<<16) | (1<<22) = 0x4120D1`.

### 2.2 MSR, PERFEVTSEL, and `wrmsr`

An **MSR — Model-Specific Register** is a special CPU register you can't touch with normal
instructions; you read/write it with the privileged `rdmsr`/`wrmsr` instructions (ring 0 /
kernel only). The counters are configured through MSRs:

- **`IA32_PERFEVTSEL0` (MSR address `0x186`)** — the **event-select register** for
  programmable counter 0. You write the event code here to say "counter 0, count *this*."
  (Counter 1 is `0x187`, counter 2 is `0x188`, … i.e. `0x186 + index`.)
- **`IA32_PERF_GLOBAL_CTRL` (MSR address `0x38F`)** — a master on/off switch: a bitmask that
  enables/disables all the counters at once.

The value we write into `PERFEVTSEL0` packs several fields into one number:

| bits  | field | our value | meaning |
|-------|-------|-----------|---------|
| 7:0   | event select | `0xD1` | MEM_LOAD_RETIRED |
| 15:8  | umask        | `0x20` | .L3_MISS |
| 16    | **USR**      | `1`    | count while CPU is in **user mode** (ring 3) |
| 17    | OS           | `0`    | do **not** count kernel-mode events |
| 22    | **EN**       | `1`    | enable this counter |

Packed together that is `0x4120D1`. We set **USR=1, OS=0** so we count **only our own
user-space sweep**, not unrelated kernel activity — a cleaner, victim-only number.

Linux exposes MSRs to user tools through the device files `/dev/cpu/<N>/msr` (you must first
`modprobe msr` to create them), and the **`msr-tools`** package provides the `wrmsr`
command-line tool. So programming counter 0 on core 0 is literally:

```
wrmsr -p0 0x186 0x4120d1      # core 0, PERFEVTSEL0 = our event
```

This is the proven approach from your labmate Shlomi's L1i code; we reuse his exact
mechanism (his event was different — see §3.1).

### 2.3 `rdpmc`: reading the counter cheaply

`rdpmc` ("read performance-monitoring counter") is a CPU instruction that reads a counter's
current value **from user space** — no system call, just a couple of nanoseconds. We read the
counter with `rdpmc` immediately before and after the sweep; `after − before` = the number of
L3-miss events during the sweep.

Two host settings must be right for this to work:

- **CR4.PCE** — a bit in the CPU's control register CR4 ("**P**erformance **C**ounter
  **E**nable"). If it's 0, `rdpmc` from user space raises a fault. It is **not** about being
  root — even root running user-space code needs this bit set. Linux controls it through the
  file `/sys/devices/cpu/rdpmc`; writing **`2`** means "always allow user-space `rdpmc`."
- **NMI watchdog** — a kernel lockup-detector that *itself* grabs one programmable counter. If
  it's running it can fight us for the counter and corrupt our reading. We turn it off with
  `echo 0 > /proc/sys/kernel/nmi_watchdog`.

Both are done once, as root, in the run script's preamble.

---

## 3. The measurement design (what the code actually does)

Chosen design: a **victim-only, two-pass self-eviction test** — no attacker, no priming, the
cleanest possible isolation of self-eviction.

For each spatial cluster `c` of the lazy map:

```
1. flush_lazy_cluster(c)        # remove all of cluster c's lines from every cache level
2. b = rdpmc()                  # read counter
   sweep_lazy_once(c)           # PASS 1 (cold): touch every line once
   M_cold = rdpmc() - b         # misses on the cold pass  (≈ nodeCount — see below)
3. (optional extra warm-up sweeps if warmPasses > 1)
4. b = rdpmc()                  # read counter
   sweep_lazy_once(c)           # PASS 2 (warm): touch every line again
   M_self = rdpmc() - b         # misses on the warm pass  = SELF-EVICTED lines
```

**Why the two passes prove self-eviction:**

- **Pass 1 is cold** — we just flushed the cluster, so *every* line is absent from cache and
  *every* access misses. Therefore `M_cold ≈ nodeCount` (the number of lines in the cluster).
  This is our **sanity check**: it proves the counter is alive and correctly scaled. If
  `M_cold` came back as 0 or some wild number, the counter setup is broken and nothing else
  can be trusted.
- **Pass 2 is warm** — pass 1 just loaded all these lines. If the cache *kept* them, pass 2
  should mostly **hit** → `M_self ≈ 0`. But if the sweep, while loading its later lines,
  **evicted its own earlier lines**, those lines are gone by pass 2 and must be re-fetched →
  `M_self` is large. **`M_self` counts precisely the lines the victim evicted from itself.**

**The prediction (this is the whole point):**

| NoC | expected `M_self / nodeCount` | why |
|-----|------------------------------|-----|
| 8   | ≈ 0 | a cluster spans several 64-byte offsets; the hardware adjacent-line prefetcher accidentally keeps same-set lines resident → little self-eviction |
| 64  | large | a cluster is a *single* offset; no accidental reinforcement → the scan-resistant policy makes the cluster's same-set lines evict each other |

and crucially, `M_self / nodeCount` should **track `1 − coverage`** across the whole NoC
sweep. If it does, self-eviction and the coverage drop are the *same phenomenon* measured two
different ways — a direct hardware confirmation of the §2 theory.

> **Subtlety worth knowing:** the number of lines mapping to each physical cache set is a
> **constant 12 at every NoC** (it is `16384·12 / NoC` lines spread over `16384 / NoC` sets =
> 12 per set always). So the NoC dependence is **not** "more crowding per set at high NoC." It
> is the **prefetcher-reinforcement** effect above. The `warmPasses` knob lets you add more
> warm sweeps before measuring; per FINDINGS §4, continuous hammering reaches a better
> steady-state occupancy, so `M_self` should *fall* as `warmPasses` rises — reproducing that
> result with hardware counts.

---

## 4. File-by-file breakdown of the changes

### 4.1 NEW — `stable/src/pmu.h` and `stable/src/pmu.c` (the counter wrapper)

A tiny, self-contained module that does the three PMU steps from §2. Extracted and
generalized from Shlomi's `Shlomi'sCode/l1i.c`.

`pmu.h` provides:

- **`PMU_EVT_L3MISS_USR`** — the packed event value `0x4120D1` (`0xD1 | 0x20<<8 | USR | EN`),
  with a comment table explaining every bit.
- **`pmu_setup(cpu, pmc_idx, event_val)`** — programs the event-select register and flips the
  global enable. (Implemented in `pmu.c`.)
- **`static inline uint64_t pmu_rdpmc(unsigned idx)`** — reads programmable counter `idx` with
  `rdpmc`, fenced by `lfence` on both sides so the read isn't reordered around the code we're
  measuring. Returns the full 64-bit count. Usage: `b = pmu_rdpmc(0); …; delta = pmu_rdpmc(0) - b`.

`pmu.c` provides:

- **`pmu_write_msr(cpu, msr, value)`** (private) — shells out to `wrmsr -p<cpu> <msr>
  <value>`, exactly as Shlomi does. We deliberately reuse the `wrmsr` tool rather than open
  `/dev/cpu/N/msr` ourselves because it's the lab's already-working, proven path. Aborts with
  a helpful message if `wrmsr` fails (e.g. not root, or `msr` module not loaded).
- **`pmu_setup(...)`** — writes `IA32_PERFEVTSEL0+idx` (`0x186`) = `event_val`, then writes
  `IA32_PERF_GLOBAL_CTRL` (`0x38F`) = `(7<<32) | 0xF` (enable the 3 fixed + 4 programmable
  counters — the same mask Shlomi/femtobench use to "wake up" the PMCs).

**Coffee Lake vs Comet Lake note (important, and why reuse is safe):** Shlomi ran on Comet
Lake; we run on Coffee Lake. Both are **Skylake-client** cores, so the PERFEVTSEL/GLOBAL_CTRL
MSR addresses **and** the `0xD1/0x20` event encoding are **identical**. Only the event *value*
changes from his experiment (his measured an L1-instruction event — see §3.1 caveat below).

### 4.2 CHANGED — `stable/src/lazy_map.c` / `lazy_map.h` (flush helper)

Added **`flush_lazy_cluster(const LazyMap *m, int c)`**: walks cluster `c`'s retained node
array and calls `clflush` on each line (the x86 instruction that evicts a specific line from
**all** cache levels), then `mfence` so the flushes complete before we start the timed sweep.
This is what makes pass 1 genuinely "cold." Also added the include of `<mastik/low.h>` (where
`clflush`/`mfence` live) and the prototype in the header.

Nothing else in `lazy_map.c` changed — the existing `sweep_lazy_once` is reused unmodified for
both the warm and measured passes (called with the plain single-pass arguments
`passes=1, accessesPerLine=1, sameAddr=0, buddyTouch=0`).

### 4.3 CHANGED — `stable/src/coverage_validator.c` (the new `selfevict` mode)

- Added `#include "pmu.h"`.
- Added **`run_selfevict_experiment(noc, iterIdx, shuffle, warmPasses)`** implementing the §3
  loop: pin to the prober core, `pmu_setup(...)` once, build the lazy map (reusing the
  existing `build_lazy_mapping`), then per cluster do flush → cold pass (`M_cold`) → warm-ups
  → measured pass (`M_self`). It prints a live summary line
  (`mean M_cold/node ≈ 1.0 sanity`, `mean M_self/node`) and writes a CSV.
- Wired a new `"selfevict"` branch into `main()`'s argument parsing:
  `argv[3]="selfevict"`, `argv[4]="shuffle"|"noshuffle"`, `argv[5]=warmPasses` (default 1).

**Important:** unlike the `native`/`jsmap` coverage modes, `selfevict` has **no attacker** —
it never loads `mapping_A`/`mapping_B` and needs **no hugepages**. It builds a fresh lazy map
and measures the victim against itself. Its only privileged need is `wrmsr`.

**Output** → `stable/data/coverage/selfevict[_shuffled]/NoC<nn>/<iter>.csv`, one row per
cluster:

```
cluster,nodeCount,M_cold,M_self
0,3072,3071,1503
1,3072,3072,1488
...
```

### 4.4 CHANGED — `stable/Makefile`

Added `src/pmu.c` to `COV_SRCS` so `pmu.o` is compiled and linked into the `CoverageValidator`
binary. (The `MastikElite` and `FingerprintOrchestrator` targets are untouched — they don't
use the PMU.)

### 4.5 NEW — `stable/run_selfevict.sh` (the runner + host setup)

A dedicated launcher that mirrors `run_coverage_native.sh` but adds the **one-time PMU host
setup** described in §2.3, done as root before the sweep:

```
modprobe msr                          # create /dev/cpu/*/msr for wrmsr
echo 2 > /sys/devices/cpu/rdpmc       # CR4.PCE=1: allow user-space rdpmc
echo 0 > /proc/sys/kernel/nmi_watchdog# free the counter the watchdog holds
```

Then, for each NoC and iteration, it runs
`sudo ./CoverageValidator <noc> <iter> selfevict <shuffle> <warmPasses>` and hands the
root-written CSVs back to your user. Knobs via env vars: `SHUFFLE` (1=page-shuffled victim,
default), `WARMPASSES` (default 1). Existing outputs are skipped so re-runs resume.

Usage:

```
cd stable && ./run_selfevict.sh <iterations_per_noc> [noc]
# e.g. smoke test at NoC=64:   ./run_selfevict.sh 1 64
# full sweep, 5 iters each:    ./run_selfevict.sh 5
```

### 4.6 CHANGED — `stable/python/coverage_compare.py` (the overlay)

Added a **section §4** to the existing diagnostic. It loads the `selfevict_shuffled` CSVs,
computes **mean `M_self / nodeCount`** and **mean `M_cold / nodeCount`** per NoC, and prints
them next to **`1 − coverage`** (pulled from the existing lazy-map coverage tree). It also
prints the **Pearson correlation** between `1 − coverage` and `M_self / nodeCount`. If no
selfevict data exists yet, it prints a "run ./run_selfevict.sh first" note and moves on — the
rest of the script is unaffected.

---

## 5. How to run it end-to-end

```bash
cd stable

# 1. Smoke test (one cluster sweep at NoC=64). Watch the printed summary line:
#    mean M_cold/node should be ≈ 1.0  (counter is alive & correct)
#    mean M_self/node should be clearly > 0 at NoC=64
./run_selfevict.sh 1 64

# 2. Full NoC sweep (8,16,32,64), a few iterations each:
./run_selfevict.sh 5

# 3. Analyze: prints the M_self/node vs (1 - coverage) overlay + correlation
python3 python/coverage_compare.py
```

Requires: root (the script uses `sudo`), the `msr-tools` package (`apt install msr-tools`),
and a Skylake-family Intel CPU (the i7-9700K qualifies).

---

## 6. How to read the results

- **Sanity first:** `M_cold / node ≈ 1.0` at every NoC. If not, stop — the counter is
  misconfigured (see §7) and `M_self` is meaningless.
- **Main result:** `M_self / node` should be **near 0 at NoC=8** and **large at NoC=64**.
- **The clincher:** `M_self / node` should **track `1 − coverage`** across NoC (strong
  positive Pearson `r`). That means the coverage drop *is* self-eviction, now shown with a
  hardware counter rather than inferred from a histogram.
- **Steady-state cross-check (optional):** re-run with `WARMPASSES=8`; `M_self` should drop,
  reproducing FINDINGS §4 (continuous hammering recovers occupancy).

---

## 7. Caveats and the fallback

- **This bypasses the Linux `perf` subsystem.** We program the counter directly via MSR, so if
  the kernel (a context switch, or a re-enabled NMI watchdog) reprograms `PERFEVTSEL0`, the
  reading can be corrupted. Mitigations already in place: the sweep is **pinned to one core**,
  and the watchdog is **disabled** in the preamble. If readings still look wrong (e.g.
  `M_cold/node` far from 1.0), the clean fallback is `perf_event_open(pid=0)` — the kernel's
  proper per-thread counter API, which handles CR4.PCE and save/restore automatically — using
  the same `0xD1/0x20` event. Not needed unless the MSR path proves flaky.
- **`MEM_LOAD_RETIRED.*` events are "precise" (PEBS-capable).** We count them as plain events
  (no PEBS record buffer), which is fully valid for the aggregate totals we want.
- **Shlomi's original event was L1i (instructions), not L3 (data).** His experiment measured
  the *instruction* cache using executable code pages he jumped into. Our victim sweep is
  **data loads**, so we discarded his instruction-cache machinery entirely and kept only the
  reusable plumbing (MSR programming + `rdpmc`), repointed at the data-load L3-miss event. This
  is why the event value differs from his.

---

## 8. Glossary

| term | meaning |
|------|---------|
| **PMU** | Performance Monitoring Unit — per-core hardware that counts CPU events. |
| **PMC** | Performance Monitoring Counter — a register that increments per event. |
| **programmable / GP counter** | a PMC whose event you choose (4 per Skylake core). |
| **event select / umask** | the two small numbers that name one specific event. |
| **retired** | an instruction that actually completed (not speculatively discarded). |
| **demand load** | a load your code executed, vs a **prefetch** (hardware speculation). |
| **L3 miss** | data not found in L1/L2/L3; fetched from DRAM or another core. |
| **MSR** | Model-Specific Register — special CPU register set via `wrmsr` (ring 0). |
| **PERFEVTSEL0** (`0x186`) | the event-select MSR for programmable counter 0. |
| **PERF_GLOBAL_CTRL** (`0x38F`) | master enable bitmask for all counters. |
| **`wrmsr` / msr-tools** | CLI tool + package to write MSRs via `/dev/cpu/N/msr`. |
| **`rdpmc`** | instruction to read a PMC cheaply from user space. |
| **CR4.PCE** | control-register bit; must be 1 for user-space `rdpmc` (via `/sys/devices/cpu/rdpmc`). |
| **NMI watchdog** | kernel lockup detector that occupies a PMC; disabled so it doesn't clash. |
| **`clflush`** | x86 instruction that evicts one line from all cache levels. |
| **cold / warm** | cluster with lines absent / already loaded in cache. |
| **`M_cold` / `M_self`** | L3 misses on the cold pass (≈ nodeCount, sanity) / warm pass (= self-evicted lines). |
| **nodeCount** | number of lines in a cluster (`16384·12 / NoC`). |
| **NoC** | Number of Clusters — the lazy map's spatial granularity. |
