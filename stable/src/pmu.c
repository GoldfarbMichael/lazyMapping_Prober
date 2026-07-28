// pmu.c
// -----------------------------------------------------------------------------
// Program a core's programmable performance counter via MSR (using the msr-tools
// `wrmsr` binary, exactly as Shlomi's l1i.c does), so the counter can then be
// read inline with rdpmc (see pmu.h pmu_rdpmc). We deliberately reuse the shell
// `wrmsr` path rather than open /dev/cpu/N/msr ourselves: it is the proven,
// already-working setup from the lab's L1i experiments.
// -----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "pmu.h"

#define MSR_IA32_PERFEVTSEL0      0x186
#define MSR_IA32_PERF_GLOBAL_CTRL 0x38F

// Shell out to `wrmsr -p<cpu> <msr> <value>` (msr-tools). Aborts on failure.
static void pmu_write_msr(int cpu, unsigned int msr, uint64_t value) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wrmsr -p%d %#x %#lx", cpu, msr, value);
    if (system(cmd)) {
        fprintf(stderr, "PMU Error: \"%s\" failed.\n"
                        "Run as root with msr-tools installed and `modprobe msr`.\n", cmd);
        exit(1);
    }
}

void pmu_setup(int cpu, int pmc_idx, uint64_t event_val) {
    // 1. Program PERFEVTSEL(pmc_idx) with the event/umask/flags.
    pmu_write_msr(cpu, MSR_IA32_PERFEVTSEL0 + pmc_idx, event_val);
    // 2. Globally enable fixed counters 0-2 and programmable counters 0-3
    //    (same GLOBAL_CTRL mask Shlomi/femtobench use to wake the PMCs).
    uint64_t global_ctrl = ((uint64_t)7 << 32) | ((1u << 4) - 1);
    pmu_write_msr(cpu, MSR_IA32_PERF_GLOBAL_CTRL, global_ctrl);
}
