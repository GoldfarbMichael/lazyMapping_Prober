"use strict";

// =====================================================================
// Lazy Mapping - Memorygram (BASIS ONLY, single-threaded)
//
// Runs entirely on the main thread - no Web Worker. The sampling
// primitive (cluster construction + sweep/probe) is stubbed and will be
// added in later steps.
// =====================================================================

// ----- Server / experiment config -----
const COLLECTION_SERVER_URL = "http://localhost:8080/collect";
const METADATA_SERVER_URL = "http://localhost:8080/set-metadata";
const CTL_POLL_URL = "http://localhost:8080/ctl/poll"; // coverage-validation coordinator

// ----- Experiment label (drives sampling params + storage path) -----
// Format: {workload}_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles
//   e.g. ?label=qsort_64C_2TST_45K_2288cycles
// Right-anchored so the workload may itself contain underscores.
const params = new URLSearchParams(window.location.search);
const LABEL_RE = /^(.+)_(\d+)C_(\d+)TST_(\d+)K_(\d+)cycles$/;

function parseLabel(raw) {
    const m = raw && raw.match(LABEL_RE);
    if (!m) return null;
    return {
        workload: m[1],
        config: `${m[2]}C_${m[3]}TST_${m[4]}K_${m[5]}cycles`,
        noc: parseInt(m[2], 10),
        tstSec: parseInt(m[3], 10),
        k: parseInt(m[4], 10),
        cycles: parseInt(m[5], 10)
    };
}

const parsed = parseLabel(params.get("label"));
if (!parsed) {
    console.warn(
        `Label missing or malformed (expected {workload}_{NoC}C_{TST}TST_{K}K_{CYCLES}cycles); ` +
        `using defaults and writing under data/manual/manual/.`
    );
}

// ----- Sampling config (from label, with hardcoded fallbacks) -----
const WORKLOAD = parsed ? parsed.workload : "manual";
const CONFIG = parsed ? parsed.config : "manual";
const NUM_OF_CLUSTERS = parsed ? parsed.noc : 64;          // NoC: spatial granularity
const MEASUREMENT_TIME_MS = (parsed ? parsed.tstSec : 2) * 1000; // TST seconds -> ms
const CYCLES_PER_ADDRESS = parsed ? parsed.cycles : 2288;  // est. CPU cycles per address
const K = parsed ? parsed.k : 45;                          // accesses between timer polls

// ----- LLC geometry (SET THESE TO YOUR CPU; not part of the label) -----
const LLC_WAYS = 12;               // associativity (lines per eviction set)
const LLC_SETS = 16384;            // 2^14 (2048 sets per slice x 8 slices)
const LLC_SIZE_MB = 12;            // cross-checked against LLC_SETS * LLC_WAYS

// ----- Timing model (per-machine; not part of the label) -----
const CLOCK_SPEED = 3.6e9;         // CPU clock in Hz (cycles/sec); NOT the browser timer
const MIN_QUANTUM_MS = 0.5;        // floor: never sweep a cluster for less than 500 us

// Q (ms): budget to sweep one cluster, sized to ~one full cluster traversal
// (setsPerCluster * ways addresses) at CLOCK_SPEED, then floored to MIN_QUANTUM_MS.
function computeQuantumMs(noc, llcSets, llcWays, cyclesPerAddress) {
    const setsPerCluster = llcSets / noc;
    const qCycles = cyclesPerAddress * setsPerCluster * llcWays;
    const qMs = (qCycles / CLOCK_SPEED) * 1000;
    return Math.max(qMs, MIN_QUANTUM_MS);
}
const QUANTUM_MS = computeQuantumMs(NUM_OF_CLUSTERS, LLC_SETS, LLC_WAYS, CYCLES_PER_ADDRESS);

// T = number of time slots (memorygram rows). One time slot = one full sweep over
// all NoC clusters, so it lasts Q * NoC ms.  T = floor(MEASUREMENT_TIME / (Q * NoC)).
const SLOT_DURATION_MS = QUANTUM_MS * NUM_OF_CLUSTERS;
const NUM_TIME_SLOTS = Math.floor(MEASUREMENT_TIME_MS / SLOT_DURATION_MS);

console.log(
    `Params: workload=${WORKLOAD} config=${CONFIG} NoC=${NUM_OF_CLUSTERS} ` +
    `TST=${MEASUREMENT_TIME_MS}ms K=${K} cycles=${CYCLES_PER_ADDRESS} ` +
    `Q=${QUANTUM_MS.toFixed(3)}ms T=${NUM_TIME_SLOTS}`
);
const statusEl = document.getElementById("status");

function setStatus(text) {
    if (statusEl) {
        statusEl.textContent = text;
    }
    console.log(text);
}

// ----- Memory geometry constants -----
const BYTES_PER_LINE = 64;          // cache line size
const BYTES_PER_PAGE = 4096;        // 4 KB page
const BYTES_PER_ELEM = Uint32Array.BYTES_PER_ELEMENT; // 4
const ELEMS_PER_LINE = BYTES_PER_LINE / BYTES_PER_ELEM; // 16
const ELEMS_PER_PAGE = BYTES_PER_PAGE / BYTES_PER_ELEM; // 1024
const LINES_PER_PAGE = BYTES_PER_PAGE / BYTES_PER_LINE;  // 64 (the bit 6-11 values)

// Fisher-Yates in-place shuffle (defeats stream/stride prefetch when applied
// to the pointer-chase order).
function shuffle(arr) {
    for (let i = arr.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        const tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
    return arr;
}

// ----- Lazy mapping cluster construction -----
// Allocates a page-aligned buffer sized to the real LLC geometry
// (LLC_SETS * LLC_WAYS * 64 B) and partitions it into `numClusters` spatial
// clusters. Mirrors the C `eviction_sets_to_Clusters`:
//   - Synthesize LLC_SETS eviction sets, each of LLC_WAYS lines that share the
//     translation-invariant bits 6-11 (drawn from shuffled pages).
//   - Assign each eviction set to a cluster by the address formula
//     cluster = (addr >> (12 - log2(NoC))) & (NoC - 1).
//   - Concatenate a cluster's eviction sets into one circular pointer-chase
//     list, stored inside the buffer itself.
//
// Page alignment assumption: large typed-array backing stores are page-aligned
// in V8 (mmap-backed). Therefore element index i has page offset (i*4) % 4096,
// and its bit 6-11 value is (i >> 4) & 0x3F. This holds only for multi-MB
// buffers with byteOffset 0; it is an engine artifact, not an ECMAScript
// guarantee, and must be re-validated per browser engine.
class LazyMapping {
    constructor(numClusters, llcSets, llcWays) {
        // Guard: NoC must be a power of two in [1, 64] so that bits 6-11 alone
        // (64 distinguishable values) partition cleanly without using
        // translation-variant bits 12+.
        const isPow2 = (numClusters & (numClusters - 1)) === 0;
        if (!isPow2 || numClusters < 1 || numClusters > LINES_PER_PAGE) {
            throw new Error(
                `NoC must be a power of two in [1, ${LINES_PER_PAGE}], got ${numClusters}`
            );
        }
        // LLC_SETS must be a multiple of 64 (bit 6-11 values per page) so that
        // each bit-value has a whole number of pages and eviction sets, and a
        // multiple of NoC so eviction sets divide evenly into clusters.
        if (llcSets % LINES_PER_PAGE !== 0) {
            throw new Error(`LLC_SETS (${llcSets}) must be a multiple of ${LINES_PER_PAGE}`);
        }
        if (llcSets % numClusters !== 0) {
            throw new Error(`LLC_SETS (${llcSets}) must be a multiple of NoC (${numClusters})`);
        }

        this.numClusters = numClusters;
        this.numSets = llcSets;     // total eviction sets (Mastik l3_getSets)
        this.ways = llcWays;        // lines per eviction set (associativity)

        const totalLines = llcSets * llcWays;
        const bufferBytes = totalLines * BYTES_PER_LINE;
        this.buffer = new Uint32Array(bufferBytes / BYTES_PER_ELEM);
        this.numPages = bufferBytes / BYTES_PER_PAGE;

        // C-equivalent address-based clustering parameters.
        this.log2NoC = Math.round(Math.log2(numClusters));
        this.shiftRight = 12 - this.log2NoC;
        this.andTarget = numClusters - 1;

        // Clustersize = eviction sets per cluster (mirrors C Clusters->counts).
        this.clusterSize = llcSets / numClusters;

        this.heads = new Array(numClusters);
        this.nodeCounts = new Array(numClusters).fill(0);        // lines per cluster
        this.evictionSetCounts = new Array(numClusters).fill(0); // == clusterSize
        this._sink = 0; // observed after each sweep to defeat dead-code elimination

        this.build();
    }

    build() {
        const ways = this.ways;
        // Eviction sets per bit-6-11 value = pages / ways = LLC_SETS / 64.
        const evSetsPerBitValue = this.numPages / ways;
        const clusterNodes = Array.from({ length: this.numClusters }, () => []);

        for (let v = 0; v < LINES_PER_PAGE; v++) {
            // All lines of bit-value v map to the same cluster (the page term
            // contributes 0 after the shift/mask), matching the C validation.
            const cluster = ((v * BYTES_PER_LINE) >> this.shiftRight) & this.andTarget;

            // Shuffle pages so each eviction set's `ways` lines are drawn from
            // non-adjacent pages -> non-strided pointer chase (defeats prefetch).
            const pages = new Array(this.numPages);
            for (let p = 0; p < this.numPages; p++) pages[p] = p;
            shuffle(pages);

            for (let s = 0; s < evSetsPerBitValue; s++) {
                for (let w = 0; w < ways; w++) {
                    const page = pages[s * ways + w];
                    // page * ELEMS_PER_PAGE --> start of a page in the buffer
                    // v * ELEMS_PER_LINE --> offset to the line with the bit value v
                    clusterNodes[cluster].push(page * ELEMS_PER_PAGE + v * ELEMS_PER_LINE);
                }
                this.evictionSetCounts[cluster]++;
            }
        }

        // Concatenate each cluster's eviction sets into one circular list.
        for (let c = 0; c < this.numClusters; c++) {
            const nodes = clusterNodes[c]; //nodes is an array 
            for (let i = 0; i < nodes.length; i++) {
                this.buffer[nodes[i]] = nodes[(i + 1) % nodes.length];
            }
            this.heads[c] = nodes[0];
            this.nodeCounts[c] = nodes.length;
        }
    }

    // Timed sweep: pointer-chase this cluster's circular list for `quantumMs`,
    // polling the clock once every K accesses, and return the number of accesses
    // completed (the memorygram cell; higher = faster sweep = less contention).
    //
    // `curr = buffer[curr]` is a single dependent load that both probes the cache
    // line and yields the next node. The inner K-loop amortizes the cost of
    // performance.now() and removes the per-access modulo. The chase is latency-
    // bound, so per-iteration loop overhead is hidden under memory latency.
    sweepCluster(clusterIndex, quantumMs) {
        const buffer = this.buffer;
        const perf = performance;
        let curr = this.heads[clusterIndex];
        let count = 0;
        const finish = perf.now() + quantumMs;
        do {
            for (let k = 0; k < K; k++) {
                curr = buffer[curr]; // probe line + advance
            }
            count += K;
        } while (perf.now() < finish);
        this._sink = curr; // keep the chain observable (prevents dead-code elimination )
        return count;
    }
}

// ----- Memorygram sampling loop (placeholder) -----
// TODO (next step): replace with the real "Sampling Procedure" -
// for each time slot, sweep C0..C(NoC-1) and record completed accesses.
function sampleMemorygram() {
    const mapping = new LazyMapping(NUM_OF_CLUSTERS, LLC_SETS, LLC_WAYS);

    // --- Geometry cross-check (verification) ---
    const geomBytes = LLC_SETS * LLC_WAYS * BYTES_PER_LINE;
    if (geomBytes !== LLC_SIZE_MB * 1024 * 1024) {
        console.warn(
            `LLC geometry mismatch: LLC_SETS*LLC_WAYS*64 = ${geomBytes} B ` +
            `(${(geomBytes / 1048576).toFixed(2)} MB) != LLC_SIZE_MB ${LLC_SIZE_MB} MB. ` +
            `Set LLC_SETS/LLC_WAYS to your CPU.`
        );
    }

    // --- Structure check (verification) ---
    const expectedLinesPerCluster = (LLC_SETS * LLC_WAYS) / NUM_OF_CLUSTERS;
    const expectedEvSetsPerCluster = LLC_SETS / NUM_OF_CLUSTERS; // C "Clustersize"
    const totalNodes = mapping.nodeCounts.reduce((a, b) => a + b, 0);
    console.log("Cluster line counts:", mapping.nodeCounts);
    console.log("Cluster eviction-set counts:", mapping.evictionSetCounts);
    console.log(
        `Expected lines/cluster: ${expectedLinesPerCluster} | ` +
        `eviction-sets/cluster: ${expectedEvSetsPerCluster} | ` +
        `total lines: ${totalNodes} (should be ${LLC_SETS * LLC_WAYS})`
    );

    const memorygram = [];

    // Align sampling to a fresh timer edge: spin until the clock ticks, so the
    // first quantum starts right at a clock boundary (not mid-tick).
    const edge = performance.now();
    while (performance.now() === edge) { /* busy-wait for next edge */ }

    const sweepStart = performance.now();
    for (let t = 0; t < NUM_TIME_SLOTS; t++) {
        const row = new Array(NUM_OF_CLUSTERS).fill(0);
        for (let c = 0; c < NUM_OF_CLUSTERS; c++) {
            row[c] = mapping.sweepCluster(c, QUANTUM_MS);
        }
        memorygram.push(row);
    }
    const sweepMs = performance.now() - sweepStart;
    console.log(
        `Total sweep time: ${sweepMs.toFixed(2)} ms over ${NUM_TIME_SLOTS} slots ` +
        `(${(sweepMs / NUM_TIME_SLOTS).toFixed(3)} ms/slot)`
    );

    return {
        memorygram: memorygram,
        meta: {
            numClusters: NUM_OF_CLUSTERS,
            quantumMs: QUANTUM_MS,
            measurementTimeMs: MEASUREMENT_TIME_MS,
            slotDurationMs: SLOT_DURATION_MS,
            numTimeSlots: NUM_TIME_SLOTS
        }
    };
}

// ----- CSV serialization -----
// One memorygram -> one CSV: a header row "G0,G1,...,G{NoC-1}" followed by one
// line per time slot of comma-separated access counts. Matches the C tool's
// memorygram CSV (stable/data/.../cache/*.csv).
function memorygramToCsv(memorygram, noc) {
    const header = Array.from({ length: noc }, (_, i) => "G" + i).join(",");
    const rows = memorygram.map(row => row.join(","));
    return header + "\n" + rows.join("\n") + "\n";
}

// ----- Server communication -----
function sendMetadata() {
    return fetch(METADATA_SERVER_URL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            workload: WORKLOAD,
            config: CONFIG
        })
    }).catch(err => console.error("Metadata POST failed:", err));
}

// Send the raw CSV as the request body. text/plain keeps it a CORS-simple
// request (no preflight); the server writes the body verbatim to the .csv file.
function sendResult(csv) {
    return fetch(COLLECTION_SERVER_URL, {
        method: "POST",
        headers: { "Content-Type": "text/plain;charset=utf-8" },
        body: csv
    }).catch(err => console.error("Result POST failed:", err));
}

// ----- Coverage-validation mode (?mode=validate) -----
// The browser is the "victim": it continuously hammers whichever cluster the C
// coordinator (Mastik prober) currently asks for, in short bursts, re-polling
// between bursts. The C side sets the cluster, waits, then prime+probes the real
// LLC to see which physical sets this cluster contends on. No memorygram, no CSV.
const HAMMER_BURST_MS = 100;   // hammer the current cluster this long between polls
const CTL_CLUSTER_STOP = -2;   // coordinator signals "done"
const CTL_CLUSTER_IDLE = -1;   // baseline: hammer nothing (measure noise floor)

// Tight hammer of one cluster's circular list for `ms` (same chase as sweepCluster).
function hammerCluster(mapping, clusterIndex, ms) {
    const buffer = mapping.buffer;
    const perf = performance;
    let curr = mapping.heads[clusterIndex];
    const finish = perf.now() + ms;
    do {
        for (let k = 0; k < K; k++) {
            curr = buffer[curr];
        }
    } while (perf.now() < finish);
    mapping._sink = curr; // defeat dead-code elimination
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function runValidation() {
    const mapping = new LazyMapping(NUM_OF_CLUSTERS, LLC_SETS, LLC_WAYS);
    setStatus("validating");
    console.log("Validation mode: hammering clusters on demand from /ctl/poll");
    for (;;) {
        let cluster;
        try {
            const resp = await fetch(CTL_POLL_URL, { cache: "no-store" });
            cluster = (await resp.json()).cluster;
        } catch (err) {
            console.error("ctl/poll failed:", err);
            await sleep(200);
            continue;
        }
        if (cluster === CTL_CLUSTER_STOP) {
            setStatus("validate done");
            console.log("Coordinator signaled stop.");
            return;
        }
        if (cluster === CTL_CLUSTER_IDLE || cluster < 0 || cluster >= NUM_OF_CLUSTERS) {
            await sleep(HAMMER_BURST_MS); // baseline: stay idle
        } else {
            hammerCluster(mapping, cluster, HAMMER_BURST_MS);
        }
    }
}

// ----- Entry point -----
function startExperiment() {
    setStatus("running");
    const result = sampleMemorygram();
    const csv = memorygramToCsv(result.memorygram, NUM_OF_CLUSTERS);
    console.log(`Memorygram: ${result.memorygram.length} rows x ${NUM_OF_CLUSTERS} cols`);
    sendResult(csv).then(() => setStatus("done"));
}

if (params.get("mode") === "validate") {
    runValidation();
} else {
    sendMetadata().then(startExperiment);
}
