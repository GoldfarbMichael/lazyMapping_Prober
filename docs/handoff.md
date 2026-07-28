# Handoff — add a `buddy-touch` option to the lazy-map sweep

**Repo:** `lazyMapping_Prober` (the C prober; NOT the memorygrams analysis repo this file happens to live in — copy this into the prober repo root before starting).
**Files:** `stable/src/lazy_map.{c,h}`, `stable/src/coverage_validator.c`, `stable/Makefile` (build only).
**Scope:** the `jsmap` coverage experiment only. Do **not** touch the timed memorygram path (`get_spatioTemporal_memoryGram_ChromeMock_jsmap` in `mastikElite.c`).

---

## 1. Why (context, short)

We are diagnosing why the lazy-map (method 3 / JS-style) LLC coverage drops as NoC grows
(mean diagonal eviction ≈ 0.96 for NoC ≤ 16, 0.87 at NoC=32, 0.58 at NoC=64), while method 1
(real Mastik eviction sets) stays flat at ~1.0.

Working model: a lazy "eviction set" is **marginal** — ~12 conflicting lines, each touched once,
scattered through the sweep — so under Coffee Lake's non-LRU replacement policy a single offset
only seats ~7/12 ways (`K(0)≈7`). Full eviction is *completed* by neighbouring in-cluster offsets
via the 128 B adjacent-line prefetcher **reinforcing** the target set's lines. As NoC rises the
cluster spans fewer offsets, so less reinforcement reaches the diagonal set → coverage falls.

**This task = the reinforcement test.** `buddy-touch` makes the sweep, for every line it visits,
*also* issue a demand access to that line's 128 B buddy — i.e. inject the reinforcement explicitly,
immediately (zero gap), and as a demand access (not a low-priority prefetch). Prediction to check
afterwards: at NoC=64 coverage should jump back up toward ~1.0, and at NoC=32 it should climb toward
the NoC=16 value. (Orthogonality/diag-mass is expected to drop — that's fine, expected, and not this
task's concern.)

This is a diagnostic knob, **default OFF**, so no existing run changes unless it's requested.

---

## 2. The addressing (get this exactly right)

`m->buf` is `uint32_t*`; a node `curr` is a 32-bit **element** index and a 64 B line spans
`curr..curr+15` (16 × uint32, `JS_ELEMS_PER_LINE == 16`). The in-page line offset is `v = (curr>>4)&0x3F`.
The 128 B pairing the hardware prefetcher enforces is `{2m, 2m+1}`, i.e. the buddy of line `v` is
`v ⊕ 1` — flipping bit 0 of `v`, which is **bit 4 of the element index**:

```
buddy_head_element = curr ^ JS_ELEMS_PER_LINE      // curr ^ 16
```

`page*1024` (bit 10+) is untouched, so `curr ^ 16` stays in the same page and lands on the buddy
line's head element (`+16` if `v` even, `-16` if odd — the XOR handles both). Reading
`buf[curr ^ 16]` touches that buddy cache line. **Not** `buf[curr + r]` (that's different *words* of
the *same* line — the already-tested `accessesPerLine`/"words" knob, which does nothing for coverage).

---

## 3. Changes

### 3a. `stable/src/lazy_map.h`

Add a trailing `int buddyTouch` param to the declaration and its doc comment:

```c
void sweep_lazy_once(const LazyMap *m, int c, int passes, int accessesPerLine,
                     int sameAddr, int buddyTouch);
```

Doc note to add: `buddyTouch - 1: also issue a demand access to each line's 128B buddy (curr ^ JS_ELEMS_PER_LINE), injecting immediate reinforcement. Default 0 = plain JS behaviour.`

### 3b. `stable/src/lazy_map.c`

Current body of `sweep_lazy_once`:

```c
for (int p = 0; p < passes; p++) {
    for (int i = 0; i < n; i++) {
        uint32_t next = buf[curr];               // 1st access (offset 0 = chase link)
        if (sameAddr) {
            for (int r = 1; r < accessesPerLine; r++)
                maccessMy((void *)(buf + curr));
        } else {
            for (int r = 1; r < accessesPerLine; r++)
                sink += buf[curr + r];
        }
        curr = next;
    }
}
```

Change the signature and add ONE line right after `next` is read:

```c
void sweep_lazy_once(const LazyMap *m, int c, int passes, int accessesPerLine,
                     int sameAddr, int buddyTouch) {
    ...
    for (int p = 0; p < passes; p++) {
        for (int i = 0; i < n; i++) {
            uint32_t next = buf[curr];                       // probe line (P, v) + chase link
            if (buddyTouch)
                sink += buf[curr ^ JS_ELEMS_PER_LINE];       // ALSO touch 128B buddy (P, v^1)
            if (sameAddr) {
                for (int r = 1; r < accessesPerLine; r++)
                    maccessMy((void *)(buf + curr));
            } else {
                for (int r = 1; r < accessesPerLine; r++)
                    sink += buf[curr + r];
            }
            curr = next;
        }
    }
    g_lazy_sink = curr + (uint32_t)sink;   // keep sink observed — prevents DCE
}
```

`sink` already feeds `g_lazy_sink`, so the buddy read won't be optimized away. Keep it.

### 3c. `stable/src/coverage_validator.c` — thread the flag through

Mirror the existing `sameAddr` plumbing. There are three spots plus the output path:

1. **`probe_set_jsmap(...)`** — add `int buddyTouch` param; pass it to `sweep_lazy_once`:
   ```c
   static uint16_t probe_set_jsmap(l3pp_t l3, int setIdx, void *head, const LazyMap *m, int c,
                                   int passes, int accessesPerLine, int sameAddr, int buddyTouch) {
       ...
       sweep_lazy_once(m, c, passes, accessesPerLine, sameAddr, buddyTouch);
       ...
   }
   ```

2. **`run_native_jsmap_experiment(...)`** — add `int buddyTouch` param. Two call sites inside:
   - the baseline timing loop: `sweep_lazy_once(&map, 0, passes, accessesPerLine, sameAddr, buddyTouch);`
   - the per-set scan: `... probe_set_jsmap(..., passes, accessesPerLine, sameAddr, buddyTouch);`

3. **Output tree suffix** (so buddy runs don't collide with existing data). In
   `run_native_jsmap_experiment`, the dir is built as `native_shuffled_p{P}a{A}[_same]` /
   `native_jsmap_p{P}a{A}[_same]`. Append `_buddy` when `buddyTouch`:
   ```c
   snprintf(baseDir, sizeof(baseDir), "%s_p%da%d%s%s", root, passes, accessesPerLine,
            sameAddr ? "_same" : "", buddyTouch ? "_buddy" : "");
   ```
   (Apply the same to the `set_labels.csv` path if it derives from `baseDir`.)

4. **`main()` argv** — `jsmap` mode currently reads:
   `argv[5]=passes, argv[6]=accessesPerLine, argv[7]="same"`. Add:
   ```c
   int buddyTouch = (argc > 8) && strcmp(argv[8], "buddy") == 0;
   ...
   return run_native_jsmap_experiment(noc, iterIdx, shuffle, passes, accessesPerLine, sameAddr, buddyTouch);
   ```
   Update the usage/comment block at the top of `main()` to document `argv[8]="buddy"`.

### 3d. Check for any other callers

Run `grep -rn "sweep_lazy_once" stable/src` before building. `lazy_map.h`'s comment claims the
function is shared with MastikElite's `timer_mode==2`; in practice the timed memorygram inlines its
own chase, but **if** you find any other caller, add a trailing `0` (buddy OFF) so it compiles and
its behaviour is unchanged. Do **not** enable buddy-touch anywhere in the timed/fingerprint path —
it doubles memory traffic and would require re-deriving the quantum Q.

---

## 4. Build

```bash
cd stable
make CoverageValidator
```

Fix any signature-mismatch compile errors (they'll point at missed call sites).

---

## 5. How to run the experiment

Binary usage for jsmap mode:
```
sudo ./CoverageValidator <NoC> <iter> jsmap <shuffle|noshuffle> <passes> <accessesPerLine> <same|words> [buddy]
```

The comparison that matters (buddy OFF vs ON, at the three NoC values that bracket the effect).
Use the same `passes`/`accessesPerLine` as the existing coverage data (default `passes=1`,
`accessesPerLine=3`; confirm against how the current coverage CSVs were generated), and shuffled
pages (matches the browser). Several iters each for averaging:

```bash
for noc in 16 32 64; do
  for it in $(seq 0 9); do
    sudo ./CoverageValidator $noc $it jsmap shuffle 1 3 words          # baseline (buddy OFF)
    sudo ./CoverageValidator $noc $it jsmap shuffle 1 3 words buddy    # buddy ON
  done
done
```

(There is a `stable/run_coverage_native.sh` — extend it if you prefer a script, but the loop above
is enough.)

---

## 6. Output location

CSVs land under, e.g.:
- baseline: `stable/data/coverage/native_jsmap_p1a3/NoC{nn}/{iter}.csv`
- buddy:    `stable/data/coverage/native_jsmap_p1a3_buddy/NoC{nn}/{iter}.csv`

(each: header row of set ids, then NoC cluster rows, then `BASELINE_ROWS` idle rows; plus a shared
`set_labels.csv` in each tree). The coverage analysis notebook (`coverage_analysis.ipynb`, in the
other repo) reads these and computes mean diagonal eviction + diag-mass per NoC.

---

## 7. Done =

- [ ] `sweep_lazy_once` takes `buddyTouch`; when set, touches `buf[curr ^ JS_ELEMS_PER_LINE]` once per node.
- [ ] Flag threaded through `probe_set_jsmap` → `run_native_jsmap_experiment` → `main()` argv[8]="buddy".
- [ ] Buddy runs write to a distinct `..._buddy` tree; non-buddy runs byte-for-byte unchanged.
- [ ] `make CoverageValidator` clean; `grep sweep_lazy_once` shows every caller updated.
- [ ] A short A/B of coverage CSVs exists for NoC ∈ {16,32,64}, buddy OFF vs ON.

**Do not** change the timed memorygram sampler, the `native`/`browser` modes, or default behaviour.

## 8. Gotchas

- `curr ^ 16` (== `curr ^ JS_ELEMS_PER_LINE`), not `curr + 16` and not `curr + r`. It must be the
  buddy **line**, not a word within the current line.
- Keep the buddy read feeding `sink`/`g_lazy_sink` or the compiler will delete it.
- Default OFF everywhere; this is a diagnostic, not a behaviour change.
- Optional companion (not required for this task): the same A/B with L2 prefetchers toggled via
  `wrmsr -p <victim_core> 0x1A4 <mask>` (bit1 = adjacent-line, bit0 = streamer) further isolates the
  mechanism, but the code change above is the deliverable.
