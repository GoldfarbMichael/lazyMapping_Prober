// website_prober.h
// -----------------------------------------------------------------------------
// Stage 4b: WEBSITE fingerprinting sampled by the NATIVE Mastik prober.
//
// Same victim as Stage 4 (a real website in a real Chrome tab, driven over CDP), but the
// observer is the C prober using FULL Mastik mapping (real L3 eviction sets -> Clusters_t)
// instead of the JS lazy map. It is the full-mapping reference the JS website arm lacks:
// Stages 1-2 established that bound only for stress-ng workloads.
//
// Two variants, selected by timer_mode, reusing the EXISTING Mastik-cluster samplers
// unchanged (no new sampling code):
//   10 (-wn) -> get_spatioTemporal_memoryGram            (native rdtscp64 clock)
//   11 (-wc) -> get_spatioTemporal_memoryGram_ChromeMock (Chrome mock clock)
//
// No Flask server, no JS, no /fp/* handshake: this process is both sampler and orchestrator.
//
// WHY NOT IN mastikElite.c
// CoverageValidator links mastikElite.o but NOT fp_ctl.o. A CDP call from mastikElite.c
// would break that target's link, so the web-victim dependency stays confined to this file,
// which only the MastikElite target compiles. Same reasoning that split website_orchestrator.c
// from fingerprint_orchestrator.c.
// -----------------------------------------------------------------------------

#ifndef WEBSITE_PROBER_H
#define WEBSITE_PROBER_H

// Collect `batch_size` samples per site (one "iteration" = one round-robin pass over the
// whole site list), writing
//     data/<clock_subdir>/<output_dir>/<slug>/<iteration>.csv
// exactly as runStressNG_batches does, with the site slug in place of the stressor name.
//
// `output_dir` is the config label ({NoC}C_{TST}TST_{K}K_{CYCLES}cycles) and, as everywhere
// else in the project, is authoritative for NoC/TST/cycles.
// Returns 0 if every sample landed, 1 otherwise (a nonzero exit stops the sweep script from
// finalizing and deleting a partially-collected tree).
int runWebsite_batches(double tst_sec, int batch_size, int start_iteration,
                       char *output_dir, const char *backing_file, const char *BIN_file,
                       int timer_mode, const char *sites_file);

#endif // WEBSITE_PROBER_H
