// label_parse.c -- config-label parsers ({NoC}C_{TST}TST_{K}K_{CYCLES}cycles).
//
// Moved verbatim out of mastikElite.c so the website orchestrator can parse its label without
// linking Mastik (mmap, hugepages, pagemap): that is what lets WebsiteOrchestrator build on
// Windows. mastikElite.h includes label_parse.h, so existing callers are unchanged.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "label_parse.h"

/**
 * Parses a directory name to extract the number of clusters (NoC).
 * 
 * Expected format: [NUMBER]C_[REST]
 * Example: "2048C_15TST_DynamicSST" returns 2048
 * 
 * @param dirname Directory or filename string to parse
 * @return The number before "C_", or -1 if parsing fails
 */
int parse_NoC_from_dirname(const char *dirname) {
    if (!dirname) return -1;
    
    // Find "C_" in the string
    const char *c_pos = strstr(dirname, "C_");
    if (!c_pos || c_pos == dirname) return -1;
    
    // Look backwards from c_pos to find where the number starts
    const char *num_end = c_pos - 1;
    
    // Skip backwards over digits
    while (num_end >= dirname && isdigit(*num_end)) {
        num_end--;
    }
    
    // num_end now points to the last non-digit character before the number
    const char *num_start = num_end + 1;
    
    // Validate we found digits
    if (num_start >= c_pos) return -1;
    
    // Extract and convert
    int noc = (int)strtol(num_start, NULL, 10);
    return (noc > 0) ? noc : -1;
}

/**
 * Parses the K field (accesses between mock-clock polls) from a config dir name.
 *
 * Format: {NoC}C_{TST}TST_{K}K_{cycles}cycles, e.g. "16C_2TST_90K_2288cycles" -> 90.
 * K == 0 (e.g. "16C_2TST_0K_2288cycles") selects the dynamic-K sweep.
 *
 * @return the parsed K (>= 0), or -1 if the K field is absent/malformed. Note 0 is a
 *         VALID result here (unlike parse_NoC), so callers must distinguish 0 from -1.
 */
int parse_K_from_dirname(const char *dirname) {
    if (!dirname) return -1;

    const char *k_pos = strstr(dirname, "K_");
    if (!k_pos || k_pos == dirname) return -1;

    const char *num_end = k_pos - 1;
    while (num_end >= dirname && isdigit((unsigned char)*num_end)) {
        num_end--;
    }
    const char *num_start = num_end + 1;
    if (num_start >= k_pos) return -1;   // no digits before "K_"

    return (int)strtol(num_start, NULL, 10);
}

/**
 * Parses the TST field (total sampling time, seconds) from a config dir name.
 *
 * Format: {NoC}C_{TST}TST_{K}K_{cycles}cycles, e.g. "16C_4TST_90K_2288cycles" -> 4.
 * Until this parser existed the "{N}TST" field was purely decorative: the sampling window came
 * from the TST_SEC compile-time constant, so a dir labelled 4TST could hold 2 s traces. Parsing
 * it here puts TST on the same footing as NoC/K/cycles -- the label now truthfully describes
 * every dimension of the sampling, and the output tree (data/<clock>/<label>/...) separates
 * TST values automatically.
 *
 * Integer seconds only (the label carries no decimal point). A sub-second TST would need a
 * different label encoding.
 *
 * @return the parsed seconds (> 0), or -1 if the field is absent/malformed. As with cycles,
 *         0 is NOT valid (a zero-length trace is meaningless).
 */
int parse_TST_from_dirname(const char *dirname) {
    if (!dirname) return -1;

    const char *t_pos = strstr(dirname, "TST");
    if (!t_pos || t_pos == dirname) return -1;

    const char *num_end = t_pos - 1;
    while (num_end >= dirname && isdigit((unsigned char)*num_end)) {
        num_end--;
    }
    const char *num_start = num_end + 1;
    if (num_start >= t_pos) return -1;   // no digits before "TST"

    int tst = (int)strtol(num_start, NULL, 10);
    return (tst > 0) ? tst : -1;
}

/**
 * Parses the cycles-per-address field from a config dir name.
 *
 * Format: {NoC}C_{TST}TST_{K}K_{CYCLES}cycles, e.g. "16C_2TST_90K_2288cycles" -> 2288.
 * This is the SAME quantity JS main.js parses as CYCLES_PER_ADDRESS (LABEL_RE) and feeds to
 * computeQuantumMs() (Q = cyclesPerAddress * setsPerCluster * ways). Keeping C and JS on one
 * label means both sides derive the same cluster quantum from the same config name.
 *
 * @return the parsed value (> 0), or -1 if the field is absent/malformed. Unlike K, 0 is NOT a
 *         valid result here (a zero-cycle quantum is meaningless).
 */
int parse_cycles_from_dirname(const char *dirname) {
    if (!dirname) return -1;

    const char *c_pos = strstr(dirname, "cycles");
    if (!c_pos || c_pos == dirname) return -1;

    const char *num_end = c_pos - 1;
    while (num_end >= dirname && isdigit((unsigned char)*num_end)) {
        num_end--;
    }
    const char *num_start = num_end + 1;
    if (num_start >= c_pos) return -1;   // no digits before "cycles"

    int cycles = (int)strtol(num_start, NULL, 10);
    return (cycles > 0) ? cycles : -1;
}
