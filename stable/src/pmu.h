#ifndef PMU_H
#define PMU_H

#include <stdint.h>

// -----------------------------------------------------------------------------
// Minimal programmable-PMC helper: program a core's PERFEVTSELx via MSR and read
// the counter with rdpmc. Extracted/generalized from Shlomi's L1i work
// (Shlomi'sCode/l1i.c: l1i_write_msr / l1i_MSR_setup + the rdpmc bracket), which
// he ran on Comet Lake. Comet Lake and this machine's Coffee Lake (i7-9700k) are
// both Skylake-client cores, so the PERFEVTSEL/GLOBAL_CTRL MSRs and the event
// encodings are identical; only the event value differs (his was an L1i
// instruction event -- dropped here; ours is a DATA-load L3-miss event).
//
// Architectural MSRs (Intel SDM Vol.4): IA32_PERFEVTSEL0 = 0x186,
// IA32_PERF_GLOBAL_CTRL = 0x38F. Programmable counter i is read by rdpmc with
// ecx = i (bit 30 clear selects general-purpose counters).
//
// PERFEVTSEL layout used by pmu_l3miss_evtsel(): event(7:0) | umask(15:8) |
// USR(16) | OS(17) | EN(22). We count MEM_LOAD_RETIRED.L3_MISS (event 0xD1,
// umask 0x20) in user mode only -> demand-load L3 misses, excluding prefetch
// fills. On Skylake-client this event is 0xD1/0x20 (Intel SDM Vol.3B perfmon
// tables); it is PEBS-capable but counted here as a plain event (no PEBS buffer),
// which is valid for aggregate counts.
//
// Prerequisites (set once, as root, see the run script preamble):
//   * `modprobe msr` + msr-tools installed  (pmu_setup shells out to `wrmsr`)
//   * `echo 2 > /sys/devices/cpu/rdpmc`      (CR4.PCE=1 so ring-3 rdpmc is legal)
//   * `echo 0 > /proc/sys/kernel/nmi_watchdog` (frees the PMC the watchdog holds)
// -----------------------------------------------------------------------------

// MEM_LOAD_RETIRED.L3_MISS, USR-only, enabled. == 0x4120D1.
#define PMU_EVT_L3MISS_USR  (0xD1u | (0x20u << 8) | (1u << 16) | (1u << 22))

// Program PERFEVTSEL(pmc_idx) on `cpu` with event_val and globally enable the
// programmable counters. Aborts (exit 1) if the wrmsr shell-out fails.
void pmu_setup(int cpu, int pmc_idx, uint64_t event_val);

// Read general-purpose PMC `idx` (fenced). Value is monotonic; bracket a region
// as: b = pmu_rdpmc(0); <region>; delta = pmu_rdpmc(0) - b.
static inline uint64_t pmu_rdpmc(unsigned idx) {
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdpmc\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        : "c"(idx)
        : "memory");
    return ((uint64_t)hi << 32) | lo;
}

#endif // PMU_H
