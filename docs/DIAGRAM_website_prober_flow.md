# Stage 4b sampling flow — native Mastik prober + real-website victim

Timer modes 10 (`-wn`, native clock) and 11 (`-wc`, Chrome mock clock).
Source: `stable/src/website_prober.c`, `stable/src/web_victim.c`, `stable/src/fp_ctl.c`.

**The one idea to understand.** The prober is a single thread, and the sampler does not return
for TST seconds — so "start sampling, then open the tab" is not something it can express. But
an HTTP round trip splits in two: `connect`+`write` are microseconds on loopback, while the
`read` is what waits for Chrome to create a target and spawn a renderer. So the prober sends
the request, samples for the whole trace, and reads the reply afterwards. **The kernel is the
concurrent agent — there is no helper process or thread.** The page load therefore commits a
few ms *into* the trace, which is the ordering the JS arm gets for free by having a separate
sampler process.

```mermaid
sequenceDiagram
    autonumber
    participant BR as batch_runner.sh
    participant M as main.c
    participant P as prober core 0
    participant K as kernel socket
    participant C as Chrome CDP 9222
    participant W as website

    rect rgb(238,244,252)
    Note over BR,C: SETUP - once per invocation
    BR->>M: sudo env SITES_FILE=.. CHROME_UID=.. ./MastikElite -wc 0 50 1C_4TST_90K_2288cycles
    M->>M: setup_browser_environment
    M->>P: runWebsite_batches timer_mode=11
    P->>P: load_mapping_and_eSetsFrom_BIN_file - real L3 eviction sets
    P->>P: parse_NoC / parse_cycles / parse_TST _from_dirname
    P->>P: web_site_list_load
    P->>P: timer_mode_subdir 11 yields chrome_clock_website
    P->>P: pin_to_core 0
    P->>P: eviction_sets_to_Clusters then calloc matrix
    P->>C: web_launch_chrome - fork exec, all-cores mask, drop to CHROME_UID
    P->>C: web_wait_cdp - GET /json/version
    C-->>P: webSocketDebuggerUrl
    P->>C: calibration - one synchronous web_victim_open of about:blank
    C-->>P: round trip in ms, logged once for the whole run
    end

    rect rgb(245,240,250)
    Note over P,W: PER SAMPLE - outer loop iteration, inner loop site
    P->>C: web_cdp_alive - liveness check OUTSIDE the trace window
    C-->>P: alive
    P->>P: snprintf path then system mkdir -p
    P->>K: web_victim_open_send - connect plus build request, BEFORE t=0
    P->>K: write PUT /json/new with raw query - ONE syscall, about 2 us
    Note over P: t = 0. Bytes are only QUEUED. Nothing has opened yet.

    par prober samples, core 0 saturated, uninterruptible
        P->>P: get_spatioTemporal_memoryGram_ChromeMock Clusters NoC TST_cycles SST_cycles matrix path
        Note right of P: chrome_mock_timer once per quantum, sweeps each cluster, counts accesses
    and kernel and Chrome proceed on their own
        K->>C: delivers the request over loopback
        C->>W: navigate - new renderer, DNS, TLS, HTML, JS, paint
        W-->>C: page loads
        C-->>K: JSON target descriptor, about 500 bytes
        Note over K: reply WAITS in the receive buffer for the rest of the trace
    end

    Note over P,C: Chrome evicts LLC lines while the prober sweeps. The slowdown IS the signal.

    P->>P: sampler writes the CSV and returns after TST seconds
    P->>K: web_victim_open_recv - read returns immediately, reply already buffered
    K-->>P: target id via fp_json_str, then close
    alt tab opened
        P->>C: web_victim_close - GET /json/close/ID, kills the renderer
    else open failed
        P->>P: unlink path - victim-less trace, discard it
    end
    P->>P: sleep_us cooldown then memset matrix
    end

    rect rgb(240,248,240)
    Note over P,C: TEARDOWN
    P->>C: kill SIGTERM then web_pkill_armed
    P->>P: free matrix, free_Clusters, l3_release
    P-->>BR: 0 if every trace landed, else 1
    end
```

## Why the reply can wait

Chrome's `/json/new` response is a few hundred bytes against a default receive buffer of
128 KB+, and its DevTools endpoint holds connections open (it ignores our `Connection: close` —
see the comment in `fp_ctl.c`), so the reply simply sits there until read. `SO_RCVTIMEO` bounds
a single `read()` call; it is not a deadline on the socket's lifetime, so holding the socket
idle across the trace costs nothing. If Chrome ever *did* abort the connection, the read fails,
the sample is marked failed and its CSV unlinked — a loud failure, not silent bad data.

## Alignment

Per-sample alignment cannot be observed in this design: the reply is only read after the trace.
It also does not need to be. The offset is a property of the browser and the loopback path, not
of any particular site, so it is measured **once per run** with a throwaway synchronous open of
`about:blank` and reported as `[web] CDP round trip: N ms`. What that window contains — target
creation and renderer spawn — is essentially identical for every site, so missing it costs
little discriminability: it is a shifted origin common to all classes, not per-class noise.

## History

An earlier version forked a persistent "navigator" child that issued the CDP call over a pipe
while the parent sampled. It achieved the same ordering with ~90 lines of pipes, a line
protocol and desync handling, and it perturbed the measurement *more* at t=0 (pipe write, plus
a scheduler IPI to wake the child, plus the child's own connect and write). Deferring the read
on a single socket does the same job with one syscall.
