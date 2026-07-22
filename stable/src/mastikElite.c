#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <mastik/l3.h>
#include <mastik/util.h>
#include <mastik/impl.h>
#include <math.h>
#include <ctype.h>


#include "utils.h"
#include "mastikElite.h"
#include "lazy_map.h"

#define ACCESSES_TILL_TIMER_POLL 90
// Fixed seed for the one-time cluster shuffle (shuffled-cluster fingerprinting A/B). Constant
// so the fixed scattered layout is reproducible; edit it for the 2-3 seed robustness check.
#define SHUFFLE_SEED 12345
// Dynamic-K sweep (selected when the config label's K field == 0, e.g. "..._0K_..."): instead
// of polling the mock clock every fixed K accesses, start each cluster quantum with a large
// batch (~4 full cluster sweeps) and shrink the batch toward the deadline, polling only ~5-9
// times per quantum. Mirrors JS sweepClusterDynamicK (JavaScript/main.js). Each chrome_mock_timer
// call is rdtscp + Murmur3 + a 128-bit divide, so far fewer calls => much lower timer overhead.
#define MIN_DYNAMIC_K 90          // floor: never batch fewer than this many accesses per poll
// DYNAMIC_K_ALPHA = 0.5 damping on the remaining-time estimate, applied as integer "/ 2" below.

// Define your massive test battery here.
// Note: The array MUST be NULL-terminated for execvp.

StressorConfig stress_battery[] = {
    // The Scientifically Validated L3 Thrasher
    { .stressor_name = "cache", .exec_args = {"stress-ng", "--cache", "1", "--cache-flush", "--cache-level", "3","--timeout", "20", NULL} },
    
    // Algorithmic & Memory Access Stressors
    { .stressor_name = "bsearch",   .exec_args = {"stress-ng", "--bsearch", "1","--timeout", "20", NULL} },
    { .stressor_name = "heapsort",  .exec_args = {"stress-ng", "--heapsort", "1","--timeout", "20", NULL} },
    { .stressor_name = "hsearch",   .exec_args = {"stress-ng", "--hsearch", "1","--timeout", "20", NULL} },
    { .stressor_name = "judy",      .exec_args = {"stress-ng", "--judy", "1", "--timeout", "20",NULL} },
    { .stressor_name = "lockbus",   .exec_args = {"stress-ng", "--lockbus", "1","--timeout", "20", NULL} },
    { .stressor_name = "lsearch",   .exec_args = {"stress-ng", "--lsearch", "1", "--timeout", "20",NULL} },
    { .stressor_name = "malloc",    .exec_args = {"stress-ng", "--malloc", "1", "--timeout", "20",NULL} },
    { .stressor_name = "matrix",    .exec_args = {"stress-ng", "--matrix", "1", "--timeout", "20",NULL} },
    { .stressor_name = "membarrier",.exec_args = {"stress-ng", "--membarrier", "1", "--timeout", "20",NULL} },
    { .stressor_name = "memcpy",    .exec_args = {"stress-ng", "--memcpy", "1","--timeout", "20", NULL} },
    { .stressor_name = "mergesort", .exec_args = {"stress-ng", "--mergesort", "1", "--timeout", "20",NULL} },
    { .stressor_name = "qsort",     .exec_args = {"stress-ng", "--qsort", "1", "--timeout", "20",NULL} },
    { .stressor_name = "radixsort", .exec_args = {"stress-ng", "--radixsort", "1", "--timeout", "20",NULL} },
    { .stressor_name = "shellsort", .exec_args = {"stress-ng", "--shellsort", "1", "--timeout", "20",NULL} },
    { .stressor_name = "skiplist",  .exec_args = {"stress-ng", "--skiplist", "1", "--timeout", "20",NULL} },
    { .stressor_name = "str",       .exec_args = {"stress-ng", "--str", "1", "--timeout", "20",NULL} },
    { .stressor_name = "stream",    .exec_args = {"stress-ng", "--stream", "1", "--timeout", "20",NULL} },
    { .stressor_name = "tree",      .exec_args = {"stress-ng", "--tree", "1", "--timeout", "20",NULL} },
    { .stressor_name = "tsearch",   .exec_args = {"stress-ng", "--tsearch", "1", "--timeout", "20",NULL} },
    { .stressor_name = "vecmath",   .exec_args = {"stress-ng", "--vecmath", "1", "--timeout", "20",NULL} },
    { .stressor_name = "wcs",       .exec_args = {"stress-ng", "--wcs", "1", "--timeout", "20",NULL} },
    { .stressor_name = "zlib",      .exec_args = {"stress-ng", "--zlib", "1", "--timeout", "20",NULL} },


    { .stressor_name = "bsearch_max",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "heapsort_max",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "hsearch_max",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "judy_max",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "lsearch_max",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "malloc_max",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", "--timeout", "20", NULL} },
    { .stressor_name = "matrix_max",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "mergesort_max", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "qsort_max",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "radixsort_max", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "shellsort_max", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "skiplist_max",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "stream_max",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", "--timeout", "20", NULL} },
    { .stressor_name = "tree_max",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "tsearch_max",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", "--timeout", "20", NULL} }
};

// StressorConfig stress_battery[] = {
//     // The Scientifically Validated L3 Thrasher
//     { .stressor_name = "cache", .exec_args = {"stress-ng", "--cache", "1", "--cache-flush", "--cache-level", "3", "--timeout", "20", NULL} },
    
//     // Algorithmic & Memory Access Stressors
//     { .stressor_name = "bsearch",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "heapsort",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "hsearch",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "judy",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "lockbus",   .exec_args = {"stress-ng", "--lockbus", "1", "--timeout", "20", NULL} },
//     { .stressor_name = "lsearch",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "malloc",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", "--timeout", "20", NULL} },
//     { .stressor_name = "matrix",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "membarrier",.exec_args = {"stress-ng", "--membarrier", "1", "--timeout", "20", NULL}},
//     { .stressor_name = "memcpy",    .exec_args = {"stress-ng", "--memcpy", "1", "--timeout", "20", NULL} },
//     { .stressor_name = "mergesort", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "qsort",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "radixsort", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "shellsort", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "skiplist",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "str",       .exec_args = {"stress-ng", "--str", "1", "--timeout", "20", NULL} },
//     { .stressor_name = "stream",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", "--timeout", "20", NULL} },
//     { .stressor_name = "tree",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "tsearch",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "vecmath",   .exec_args = {"stress-ng", "--vecmath", "1", "--timeout", "20", NULL} },
//     { .stressor_name = "wcs",       .exec_args = {"stress-ng", "--wcs", "1", "--timeout", "20", NULL} },
//     { .stressor_name = "zlib",      .exec_args = {"stress-ng", "--zlib", "1", "--timeout", "20", NULL} }
// };

// StressorConfig stress_battery[] = {
//     // The Scientifically Validated L3 Thrasher    
//     // Algorithmic & Memory Access Stressors
//     { .stressor_name = "bsearch_max",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "heapsort_max",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "hsearch_max",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "judy_max",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "lsearch_max",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "malloc_max",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", "--timeout", "20", NULL} },
//     { .stressor_name = "matrix_max",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "mergesort_max", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "qsort_max",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "radixsort_max", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "shellsort_max", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "skiplist_max",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "stream_max",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", "--timeout", "20", NULL} },
//     { .stressor_name = "tree_max",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", "--timeout", "20", NULL} },
//     { .stressor_name = "tsearch_max",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", "--timeout", "20", NULL} },

// };

size_t stress_battery_count(void) {
    return NUM_STRESSORS;
}

void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("FATAL: sched_setaffinity failed. Are you running as root?");
        exit(1);
    }
}

void cleanup_handler(int sig) {
    fprintf(stderr, "\n[CLEANUP] Received signal %d, killing child processes...\n", sig);
    system("pkill -9 stress-ng");  // Kill all stress-ng processes
    exit(1);
}



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

int newBackedMapping_and_saveEsets_toBinFile(l3pp_t *l3, const char *backing_file, const char *BIN_file){
    *l3 = prepareBackedL3(backing_file);
    if(!*l3) {
        fprintf(stderr, "Failed to prepare backed L3_A structure.\n");
        return 1;
    }

    int numOfsets = l3_getSets(*l3);
    printf("Number of sets in L3: %d\n", numOfsets);

    // --- Retrieve Eviction Sets --- 
    void **e_sets = l3_get_eviction_sets(*l3);
    if (e_sets == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
        l3_release(*l3);
        return 1;
    }


    printf("Discovering sets and saving to: %s\n", BIN_file);
    if (save_physical_mapping(*l3, e_sets, BIN_file) != 0) {
        fprintf(stderr, "Save failed.\n");
        return 1;
    }
    return 0;
}


int create2_newBackedMappings_and_syncSetIndexes(l3pp_t *l3,l3pp_t *l3B, const char *backing_fileA, const char *backing_fileB, const char *BIN_fileA, const char *BIN_fileB){

    *l3 = prepareBackedL3(backing_fileA);
    if(!*l3) {
        fprintf(stderr, "Failed to prepare backed L3 structure.\n");
        return 1;
    }

    // --- Retrieve Eviction Sets ---
    void **e_setsA = l3_get_eviction_sets(*l3);
    if (e_setsA == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
        l3_release(*l3);
        return 1;
    }

    // save mappingA to .bin file
    printf("Discovering sets and saving to: %s\n", BIN_fileA);
    if (save_physical_mapping(*l3, e_setsA, BIN_fileA) != 0) {
        fprintf(stderr, "Save failed.\n");
        return 1;
    }

    sleep(2);

    *l3B = prepareBackedL3(backing_fileB);
    if(!*l3B) {
        fprintf(stderr, "Failed to prepare backed L3 structure.\n");
        return 1;
    }

    // --- Retrieve Eviction Sets ---
    void **e_setsB = l3_get_eviction_sets(*l3B);
    if (e_setsB == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
        l3_release(*l3B);
        return 1;
    }


    int *transTable = get_transTable(*l3, *l3B, e_setsA, e_setsB, 16, 1024, l3_getAssociativity(*l3),0);
    sync_eSetsB_to_eSetsA(e_setsB, 1024, 16, transTable);
    printf("\nfnished Syncing\n\n");
    get_transTable(*l3, *l3B, e_setsA, e_setsB, 16, 1024, 12,1);
    save_physical_mapping(*l3B, e_setsB, BIN_fileB);
    return 0;
}


int load_mapping_and_eSetsFrom_BIN_file(l3pp_t *l3, void ***e_sets, const char *backing_file, const char *BIN_file){

    *l3 = prepareBackedL3(backing_file);
    if(!*l3) {
        fprintf(stderr, "Failed to prepare backed L3 structure.\n");
        return 1;
    }

    size_t buf_size = 24 * 1024 * 1024;
    void *buffer = map_hugepage_file(backing_file, buf_size);
    if (buffer == MAP_FAILED) return 1;

    // Load PAs
    int count;
    uint64_t *phys_mapA = load_physical_mapping(BIN_file, &count);

    for (int i = 0; i < 12; i++) {
            printf("  Way %d: 0x%lx\n", i, phys_mapA[i]);
    }

    *e_sets = fill_eviction_sets(buffer, buf_size, phys_mapA, l3_getSets(*l3), l3_getAssociativity(*l3));
    if (*e_sets == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction setsS.\n");
        return 1;
    }
    return 0;
}



/*
 * Converts an array of Mastik eviction sets into a cluster_t array.
 * Clusters are determined by bits 7-11 of the address (merging adjacent lines).
 * * e_sets: Array of pointers to linked lists (from get_eviction_sets_via_offsets)
 * num_sets: Size of the e_sets array (e.g., from l3_getSets)
 */
Clusters_t* eviction_sets_to_Clusters(void ***e_sets, int num_sets, int NoC) {
    if (!e_sets || !*e_sets) return NULL;

    Clusters_t *Clusters = (Clusters_t *)malloc(sizeof(Clusters_t));
    if (!Clusters) {
        perror("malloc Clusters");
        return NULL;
    }

    int Clustersize = num_sets / NoC;
    Clusters->Clustersize = Clustersize;
    memset(Clusters->counts, 0, sizeof(Clusters->counts));

    // Allocate array of cluster head pointers
    Clusters->clusterHeads = (void **)malloc(NoC * sizeof(void *));
    if (!Clusters->clusterHeads) {
        perror("malloc clusterHeads");
        free(Clusters);
        return NULL;
    }
    memset(Clusters->clusterHeads, 0, NoC * sizeof(void *));

    // Allocate tail array to track the tail of each cluster's circular list
    void **clusterTails = (void **)malloc(NoC * sizeof(void *));
    if (!clusterTails) {
        perror("malloc clusterTails");
        free(Clusters->clusterHeads);
        free(Clusters);
        return NULL;
    }
    memset(clusterTails, 0, NoC * sizeof(void *));

    int shiftRight = -1;
    int andTarget = -1;
    
    // Determine cluster mapping method
    if (NoC <= 64) {
        // Address-based: use bit shifting
        int log2NoC = (int)log2((double)NoC);
        shiftRight = 12 - log2NoC;
        andTarget = NoC - 1;
        printf("[DEBUG] NoC=%d (address-based), log2(NoC)=%d, shiftRight=%d, andTarget=0x%x\n", NoC, log2NoC, shiftRight, andTarget);
    } else {
        // Set index-based: modulo operation
        printf("[DEBUG] NoC=%d (set index-based), using modulo clustering\n", NoC);
    }

    // Iterate through all eviction sets
    for (int i = 0; i < num_sets; i++) {
        if ((*e_sets)[i] == NULL) continue;

        void *curr = (*e_sets)[i];
        int headIdx;
        
        // Determine cluster index based on method
        if (NoC <= 64) {
            headIdx = ((uintptr_t)curr >> shiftRight) & andTarget;
        } else {
            headIdx = i % NoC;  // Set index modulo NoC
        }
        
        // Validate: traverse the circular linked list
        int valid = 1;
        void *head = curr;
        void *tail = NULL;
        do {
            uintptr_t addr_val = (uintptr_t)curr;
            int cluster_idx;
            
            if (NoC <= 64) {
                cluster_idx = (addr_val >> shiftRight) & andTarget;
            } else {
                // For modulo-based: all addresses in an e_set should map to same cluster
                // We only validate the set index here
                cluster_idx = headIdx;
            }
            
            if (cluster_idx != headIdx || cluster_idx >= NoC) {
                printf("ClusterID doesn't match head Cluster id OR cluster id >= NoC --> e_set[%d]\n", i);
                valid = 0;
                break;
            }
            tail = curr;  // Track the tail as we traverse
            curr = LNEXT(curr);
        } while (curr != head);

        // If valid, merge this eviction set into the cluster's circular list
        if (valid) {
            int cluster = headIdx;
            
            if (Clusters->clusterHeads[cluster] == NULL) {
                // First e_set for this cluster
                Clusters->clusterHeads[cluster] = (*e_sets)[i];
                clusterTails[cluster] = tail;
            } else {
                // Merge: Use cached tail pointer to connect lists
                void *existing_tail = clusterTails[cluster];
                void *new_head = (*e_sets)[i];
                
                // Connect existing_tail -> new_head
                LNEXT(existing_tail) = new_head;
                // Connect new_tail -> existing_head
                LNEXT(tail) = Clusters->clusterHeads[cluster];
                
                // Update tail for this cluster
                clusterTails[cluster] = tail;
            }
            
            Clusters->counts[cluster]++;
        }
    }
    
    free(clusterTails);
    return Clusters;
}

/**
 * Performs spatio-temporal memory sampling to create a memoryGram.
 * 
 * This function traverses the eviction sets organized in clusters and measures
 * cache access patterns over time. It records the number of successful accesses
 * within a given time window for each cluster.
 * 
 * @param Clusters Pre-constructed Clusters_t instance from eviction_sets_to_Clusters()
 * @param num_sets Total number of eviction sets
 * @param NoC Number of clusters
 * @param TST_cycles Total sampling time in CPU cycles
 * @param SST_cycles Single sample time window in CPU cycles
 * @param matrix Pre-allocated uint32_t array that represents an array of size [total_samples_per_cluster * NoC].
 *               MUST be initialized to 0 before calling this function.
 *               No allocation/initialization is performed here to avoid overhead.
 * @param filename Output CSV file to write the matrix data
 */
void get_spatioTemporal_memoryGram(Clusters_t *Clusters, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename){
    if (!Clusters) {
        fprintf(stderr, "FATAL: Clusters is NULL.\n");
        return;
    }

    if (!matrix) {
        fprintf(stderr, "FATAL: matrix is NULL. Must be pre-allocated and initialized to 0.\n");
        return;
    }

    // 1. Calculate matrix dimensions
    uint64_t total_samples_per_cluster = TST_cycles / (NoC * SST_cycles);
    
    // 2. Spatio-Temporal Sampling Phase (Strictly NO I/O here)
    for (uint64_t s = 0; s < total_samples_per_cluster; s++) {
        for (int c = 0; c < NoC; c++) {
            
            // Check if cluster has addresses
            if (Clusters->clusterHeads[c] == NULL) {
                continue;
            }
            
            // Get the head of the circular linked list for this cluster
            void *head = Clusters->clusterHeads[c];
            register void *curr = head;
            register uint32_t count = 0;
            
            // Set the timer constraint for this cluster
            uint64_t start_cluster = rdtscp64();
            uint64_t end_cluster = start_cluster + SST_cycles;
            
            // Polling Loop - traverse cluster's circular linked list using LNEXT
            while (rdtscp64() < end_cluster) {
                // Access the cache line at curr
                maccessMy(curr);
                curr = LNEXT(curr);
                count++;
            }
            
            // Store the access count
            matrix[s * NoC + c] = count;
        }
    }

    // 3. I/O Phase (Post-Measurement)
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "FATAL: Could not open output file %s\n", filename);
        free_Clusters(Clusters);
        return;
    }

    // Write CSV Header
    for (int g = 0; g < NoC; g++) {
        fprintf(fp, "G%d%s", g, (g == NoC - 1) ? "" : ",");
    }
    fprintf(fp, "\n");

    // Write Matrix Data
    for (uint64_t t = 0; t < total_samples_per_cluster; t++) {
        for (int g = 0; g < NoC; g++) {
            fprintf(fp, "%u%s", matrix[t * NoC + g], (g == NoC - 1) ? "" : ",");
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf("Successfully wrote %lu samples for %d clusters to %s\n", total_samples_per_cluster, NoC, filename);
}




void get_spatioTemporal_memoryGram_ChromeMock(Clusters_t *Clusters, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename){
    if (!Clusters) {
        fprintf(stderr, "FATAL: Clusters is NULL.\n");
        return;
    }

    if (!matrix) {
        fprintf(stderr, "FATAL: matrix is NULL. Must be pre-allocated and initialized to 0.\n");
        return;
    }

    // 1. Calculate matrix dimensions
    uint64_t total_samples_per_cluster = TST_cycles / (NoC * SST_cycles);
    uint64_t SST_us = (SST_cycles *1000000)/g_tsc_freq_hz;  // Convert SST_cycles to microseconds for mock timer
    
    // 2. Spatio-Temporal Sampling Phase (Strictly NO I/O here)
    for (uint64_t s = 0; s < total_samples_per_cluster; s++) {
        for (int c = 0; c < NoC; c++) {
            
            // Check if cluster has addresses
            if (Clusters->clusterHeads[c] == NULL) {
                continue;
            }
            
            // Get the head of the circular linked list for this cluster
            void *head = Clusters->clusterHeads[c];
            register void *curr = head;
            register uint32_t count = 0;
            
            // Set the timer constraint for this cluster
            uint64_t start_cluster = chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed);
            uint64_t end_cluster = start_cluster + SST_us;
            
            // Polling Loop - traverse cluster's circular linked list using LNEXT
            // while (chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed) < end_cluster) {
            //     // Access the cache line at curr
            //     maccessMy(curr);
            //     curr = LNEXT(curr);
            //     count++;
            // }
            int check_count = 0;

            while (1) {
                maccessMy(curr);
                curr = LNEXT(curr);
                count++;
                
                if (++check_count == ACCESSES_TILL_TIMER_POLL) {
                    if (chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed) >= end_cluster) {
                        break;
                    }
                    check_count = 0;
                }
            }


            // Store the access count
            matrix[s * NoC + c] = count;
        }
    }

    // 3. I/O Phase (Post-Measurement)
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "FATAL: Could not open output file %s\n", filename);
        free_Clusters(Clusters);
        return;
    }

    // Write CSV Header
    for (int g = 0; g < NoC; g++) {
        fprintf(fp, "G%d%s", g, (g == NoC - 1) ? "" : ",");
    }
    fprintf(fp, "\n");

    // Write Matrix Data
    for (uint64_t t = 0; t < total_samples_per_cluster; t++) {
        for (int g = 0; g < NoC; g++) {
            fprintf(fp, "%u%s", matrix[t * NoC + g], (g == NoC - 1) ? "" : ",");
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf("Successfully wrote %lu samples for %d clusters to %s\n", total_samples_per_cluster, NoC, filename);
}




/**
 * Chrome-mock-clock spatio-temporal sampler for the JS-STYLE lazy-map victim (timer_mode==2).
 *
 * Structurally identical to get_spatioTemporal_memoryGram_ChromeMock (same SST_us window,
 * same ACCESSES_TILL_TIMER_POLL batched chrome_mock_timer polling, same CSV writer), but the
 * inner sweep is the JS 32-bit index chase over the LazyMap's mmap buffer (curr = buf[curr])
 * instead of the Mastik pointer-chase (maccessMy + LNEXT). This is the additive victim source
 * for the A/B against the Mastik-e_set path; the e_set sampler above is left untouched.
 *
 * @param m     Pre-built LazyMap (build_lazy_mapping); m->heads[c] = head element index of cluster c.
 * @param NoC   Number of clusters.
 * @param TST_cycles Total sampling time (CPU cycles); sets total time slots.
 * @param SST_cycles Single-cluster sweep window (CPU cycles); converted to the mock-clock us window.
 * @param matrix Pre-allocated, zero-initialized uint32_t[total_slots * NoC]; matrix[s*NoC+c] = count.
 * @param filename Output CSV path.
 * @param K     Accesses between mock-clock polls. K > 0 = fixed cadence (poll every K accesses);
 *              K == 0 = dynamic-K (adaptive batch, ~5-9 polls/quantum) mirroring JS
 *              sweepClusterDynamicK. Comes from the config label's K field.
 */
void get_spatioTemporal_memoryGram_ChromeMock_jsmap(LazyMap *m, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename, int K){
    if (!m || !m->buf) {
        fprintf(stderr, "FATAL: LazyMap is NULL/unbuilt.\n");
        return;
    }

    if (!matrix) {
        fprintf(stderr, "FATAL: matrix is NULL. Must be pre-allocated and initialized to 0.\n");
        return;
    }

    // 1. Calculate matrix dimensions
    uint64_t total_samples_per_cluster = TST_cycles / (NoC * SST_cycles);
    uint64_t SST_us = (SST_cycles * 1000000) / g_tsc_freq_hz;  // Convert SST_cycles to microseconds for mock timer

    // Dynamic-K initial batch = ~4 full cluster sweeps (JS: nodeCounts[0]*4), computed ONCE.
    // nodeCounts is equal across clusters for pow2 NoC. Only used when K == 0.
    uint32_t initialK = (uint32_t)(m->nodeCounts[0] * 4);
    if (initialK < MIN_DYNAMIC_K) initialK = MIN_DYNAMIC_K;

    // 2. Spatio-Temporal Sampling Phase (Strictly NO I/O here)
    for (uint64_t s = 0; s < total_samples_per_cluster; s++) {
        for (int c = 0; c < NoC; c++) {

            const uint32_t *buf = m->buf;
            register uint32_t curr = m->heads[c];
            register uint32_t count = 0;

            // Set the timer constraint for this cluster
            uint64_t start_cluster = chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed);
            uint64_t end_cluster = start_cluster + SST_us;

            if (K != 0) {
                // FIXED-K: poll the mock clock every K accesses (K from the config label).
                int check_count = 0;
                while (1) {
                    curr = buf[curr];   // JS index chase: the access AND the next-node link
                    count++;
                    if (++check_count == K) {
                        if (chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed) >= end_cluster) {
                            break;
                        }
                        check_count = 0;
                    }
                }
            } else {
                // DYNAMIC-K: start with a large batch, then size each subsequent batch to a
                // damped (x0.5) fraction of the time remaining, floored at MIN_DYNAMIC_K, so the
                // clock is polled only ~5-9 times per quantum. Mirrors JS sweepClusterDynamicK.
                uint64_t prev = start_cluster;
                uint32_t k = initialK;
                for (;;) {
                    for (uint32_t i = 0; i < k; i++) curr = buf[curr];  // hot chase, no timer poll
                    count += k;
                    uint64_t now = chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed);
                    if (now >= end_cluster) break;
                    uint64_t batch_us = now - prev;
                    prev = now;
                    uint64_t remaining = end_cluster - now;
                    if (batch_us > 0) {
                        // k_next = max(MIN_DYNAMIC_K, floor((k/batch_us) * remaining * 0.5))
                        uint64_t kn = ((uint64_t)k * remaining) / batch_us / 2;
                        k = (kn > (uint64_t)MIN_DYNAMIC_K) ? (uint32_t)kn : (uint32_t)MIN_DYNAMIC_K;
                    } else {
                        k = MIN_DYNAMIC_K;  // batch finished within the clock's ~100us clamp
                    }
                }
            }

            // Store the access count
            matrix[s * NoC + c] = count;
        }
    }

    // 3. I/O Phase (Post-Measurement)
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "FATAL: Could not open output file %s\n", filename);
        return;
    }

    // Write CSV Header
    for (int g = 0; g < NoC; g++) {
        fprintf(fp, "G%d%s", g, (g == NoC - 1) ? "" : ",");
    }
    fprintf(fp, "\n");

    // Write Matrix Data
    for (uint64_t t = 0; t < total_samples_per_cluster; t++) {
        for (int g = 0; g < NoC; g++) {
            fprintf(fp, "%u%s", matrix[t * NoC + g], (g == NoC - 1) ? "" : ",");
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf("Successfully wrote %lu samples for %d clusters to %s\n", total_samples_per_cluster, NoC, filename);
}


/**
 * NATIVE-CLOCK twin of get_spatioTemporal_memoryGram_ChromeMock_jsmap (timer_mode==3).
 *
 * Same JS-style lazy-map victim (32-bit index chase over the mmap buffer, curr = buf[curr]),
 * same batched-K sweep structure, same CSV writer -- the ONLY difference is that the clock is
 * rdtscp64() instead of chrome_mock_timer(). That makes mode 3 vs. mode 2 a CLOCK-ONLY A/B
 * (and mode 3 vs. mode 0 a VICTIM-ONLY A/B).
 *
 * The batched-K structure is retained deliberately rather than adopting the per-access poll of
 * get_spatioTemporal_memoryGram: matching the chrome-mock sampler's sweep shape is what keeps
 * the comparison attributable to the clock alone.
 *
 * @param m     Pre-built LazyMap (build_lazy_mapping); m->heads[c] = head element index of cluster c.
 * @param NoC   Number of clusters.
 * @param TST_cycles Total sampling time (CPU cycles); sets total time slots.
 * @param SST_cycles Single-cluster sweep window (CPU cycles) -- used directly, no unit conversion.
 * @param matrix Pre-allocated, zero-initialized uint32_t[total_slots * NoC]; matrix[s*NoC+c] = count.
 * @param filename Output CSV path.
 * @param K     Accesses between clock polls. K > 0 = fixed cadence; K == 0 = dynamic-K (adaptive
 *              batch) mirroring JS sweepClusterDynamicK. Comes from the config label's K field.
 */
void get_spatioTemporal_memoryGram_jsmap(LazyMap *m, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename, int K){
    if (!m || !m->buf) {
        fprintf(stderr, "FATAL: LazyMap is NULL/unbuilt.\n");
        return;
    }

    if (!matrix) {
        fprintf(stderr, "FATAL: matrix is NULL. Must be pre-allocated and initialized to 0.\n");
        return;
    }

    // 1. Calculate matrix dimensions
    uint64_t total_samples_per_cluster = TST_cycles / (NoC * SST_cycles);

    // Dynamic-K initial batch = ~4 full cluster sweeps (JS: nodeCounts[0]*4), computed ONCE.
    // nodeCounts is equal across clusters for pow2 NoC. Only used when K == 0.
    uint32_t initialK = (uint32_t)(m->nodeCounts[0] * 4);
    if (initialK < MIN_DYNAMIC_K) initialK = MIN_DYNAMIC_K;

    // 2. Spatio-Temporal Sampling Phase (Strictly NO I/O here)
    for (uint64_t s = 0; s < total_samples_per_cluster; s++) {
        for (int c = 0; c < NoC; c++) {

            const uint32_t *buf = m->buf;
            register uint32_t curr = m->heads[c];
            register uint32_t count = 0;

            // Set the timer constraint for this cluster (cycles throughout)
            uint64_t start_cluster = rdtscp64();
            uint64_t end_cluster = start_cluster + SST_cycles;

            if (K != 0) {
                // FIXED-K: poll the clock every K accesses (K from the config label).
                int check_count = 0;
                while (1) {
                    curr = buf[curr];   // JS index chase: the access AND the next-node link
                    count++;
                    if (++check_count == K) {
                        if (rdtscp64() >= end_cluster) {
                            break;
                        }
                        check_count = 0;
                    }
                }
            } else {
                // DYNAMIC-K: start with a large batch, then size each subsequent batch to a
                // damped (x0.5) fraction of the time remaining, floored at MIN_DYNAMIC_K. The
                // formula k_next = max(MIN_DYNAMIC_K, (k/batch_elapsed) * remaining * 0.5) is
                // unit-agnostic, so it carries over from the mock clock's us to rdtscp's cycles
                // unchanged. Mirrors JS sweepClusterDynamicK.
                uint64_t prev = start_cluster;
                uint32_t k = initialK;
                for (;;) {
                    for (uint32_t i = 0; i < k; i++) curr = buf[curr];  // hot chase, no timer poll
                    count += k;
                    uint64_t now = rdtscp64();
                    if (now >= end_cluster) break;
                    uint64_t batch_cycles = now - prev;
                    prev = now;
                    uint64_t remaining = end_cluster - now;
                    if (batch_cycles > 0) {
                        uint64_t kn = ((uint64_t)k * remaining) / batch_cycles / 2;
                        k = (kn > (uint64_t)MIN_DYNAMIC_K) ? (uint32_t)kn : (uint32_t)MIN_DYNAMIC_K;
                    } else {
                        // Effectively dead with rdtscp's cycle resolution (the mock clock's 100us
                        // clamp is what made this reachable there); kept as a div-by-zero guard.
                        k = MIN_DYNAMIC_K;
                    }
                }
            }

            // Store the access count
            matrix[s * NoC + c] = count;
        }
    }

    // 3. I/O Phase (Post-Measurement)
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "FATAL: Could not open output file %s\n", filename);
        return;
    }

    // Write CSV Header
    for (int g = 0; g < NoC; g++) {
        fprintf(fp, "G%d%s", g, (g == NoC - 1) ? "" : ",");
    }
    fprintf(fp, "\n");

    // Write Matrix Data
    for (uint64_t t = 0; t < total_samples_per_cluster; t++) {
        for (int g = 0; g < NoC; g++) {
            fprintf(fp, "%u%s", matrix[t * NoC + g], (g == NoC - 1) ? "" : ",");
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf("Successfully wrote %lu samples for %d clusters to %s\n", total_samples_per_cluster, NoC, filename);
}


// Output-tree subdir for a timer_mode: 0=native clock (Mastik e_sets), 1=Chrome mock clock
// (Mastik e_sets), 2=Chrome mock clock (JS-style lazy map), 3=native clock (JS-style lazy map).
// Keeps each victim/clock combination in a distinct tree so previously collected data is never
// touched.
static const char* timer_mode_subdir(int timer_mode) {
    switch (timer_mode) {
        case 1:  return "chrome_clock";
        case 2:  return "chrome_clock_jsmap";
        case 3:  return "native_clock_jsmap";
        default: return "native_clock";
    }
}


int runStressNG_batches(double tst_sec, int batch_size, int start_iteration, char *output_dir, const char *backing_file, const char *BIN_file, int timer_mode, int shuffleClusters) {
    l3pp_t l3 = NULL;
    void **e_sets = NULL;

    load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, backing_file, BIN_file);
    printf("\n=== Testing get_spatioTemporal_memoryGram ===\n");
    // Calculate matrix dimensions
    int NoC = parse_NoC_from_dirname(output_dir);
    int setsPerCluster = l3_getSets(l3)/NoC;

    // K = accesses between clock polls (from the config label's "{K}K" field).
    // K == 0 selects the dynamic-K jsmap sweep; a missing/malformed field defaults to the
    // fixed 90-access cadence. Only used by the jsmap samplers (timer_mode 2 mock clock,
    // timer_mode 3 native clock).
    int K = parse_K_from_dirname(output_dir);
    if (K < 0) K = ACCESSES_TILL_TIMER_POLL;
    printf("K (clock poll cadence): %d %s\n", K, (K == 0) ? "(dynamic-K)" : "(fixed)");

    // Cycles per address: the "{N}cycles" field of the config label, the SAME quantity JS
    // main.js parses as CYCLES_PER_ADDRESS and feeds to computeQuantumMs(). Drives the cluster
    // quantum for EVERY timer_mode, so the dir name now truthfully describes the sampling.
    int cpa = parse_cycles_from_dirname(output_dir);
    if (cpa <= 0) {
        cpa = (timer_mode == 0) ? 300 : 2288;   // legacy hardcoded defaults (200*1.5 / 2288)
        printf("[WARN] No {N}cycles field in '%s'; falling back to %d cycles/address\n", output_dir, cpa);
    }
    printf("Cycles per address: %d (from config label)\n", cpa);

    uint64_t TST_cycles = g_tsc_freq_hz * tst_sec;
    //print TST_Cycles and g_tsc_freq_hz
    printf("TST_cycles: %lu, g_tsc_freq_hz: %lu\n", TST_cycles, g_tsc_freq_hz);
    uint64_t SST_cycles = (uint64_t)cpa * setsPerCluster * l3_getAssociativity(l3);

    // 500us floor (JS MIN_QUANTUM_MS): applied ONLY to the Chrome-mock-clock modes (1 Mastik
    // e_sets, 2 JS-style lazy map), where the clock's 100us clamp makes a sub-500us quantum
    // meaningless. The native-clock modes (0 and 3) have cycle resolution and stay unfloored.
    if (timer_mode == 1 || timer_mode == 2) {
        uint64_t min_SST_cycles_for_500us = (500 * g_tsc_freq_hz) / 1000000;
        if (SST_cycles < min_SST_cycles_for_500us) {
            printf("[TIMER_MODE=%d] SST_cycles adjusted: %lu -> %lu (minimum 500us window)\n", timer_mode, SST_cycles, min_SST_cycles_for_500us);
            SST_cycles = min_SST_cycles_for_500us;
        }
    }
    
    uint64_t totalSweeps_forCluster = TST_cycles / (NoC * SST_cycles);
    printf("Sets PER CLUSTER %d, NoC %d, SST_Cycles: %lu\n", setsPerCluster, NoC, SST_cycles);
    printf("Total samples per cluster: %lu\n", totalSweeps_forCluster);
    printf("Matrix size: %lu x %d\n", totalSweeps_forCluster, NoC);

    // 1. PIN THE PROBER (PARENT)
    // Assuming Core 0 is isolated or at least stable
    printf("[INFO] Pinning Mastik prober to Core 0...\n");
    pin_to_core(0);    


    // Build the victim source. Default (timer_mode 0/1) = the Mastik loaded-eviction-set
    // clusters (unchanged). timer_mode 2 (mock clock) and 3 (native clock) = the JS-faithful
    // lazy map (always shuffled pages), built in a fresh mmap buffer exactly as the browser
    // does. The loaded e_sets are unused for those modes, but l3 is still used above for
    // identical TST/SST sizing.
    int use_jsmap = (timer_mode == 2 || timer_mode == 3);
    Clusters_t *Clusters = NULL;
    LazyMap jmap;
    memset(&jmap, 0, sizeof(jmap));
    if (use_jsmap) {
        if (build_lazy_mapping(&jmap, NoC, l3_getSets(l3), l3_getAssociativity(l3), /*shufflePages=*/1)) {
            fprintf(stderr, "Failed to build JS lazy mapping\n");
            l3_release(l3);
            return 1;
        }
        printf("Built JS-style lazy map victim (mmap %zu MB, %d clusters, shuffled pages)\n",
               jmap.bytes / (1024 * 1024), NoC);
    } else {
        Clusters = eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), NoC);
        if (!Clusters) {
            fprintf(stderr, "Failed to create clusters\n");
            l3_release(l3);
            return 1;
        }
        // Shuffled-cluster A/B (coverage->accuracy test): line-shuffle each cluster's ring
        // ONCE, before sampling, so every sample reuses the SAME fixed scattered layout (matches
        // how the jsmap victim is built once). Membership is preserved -- only traversal order
        // changes. Gated to timer_mode 1 (Chrome-mock Mastik e-sets); fixed seed = reproducible.
        if (shuffleClusters && timer_mode == 1) {
            srand(SHUFFLE_SEED);
            for (int c = 0; c < NoC; c++)
                shuffle_cluster_nodes(Clusters, c, l3_getAssociativity(l3));
            printf("[INFO] Clusters line-shuffled ONCE (seed %d) -> data/chrome_clock_shuffled/\n",
                   SHUFFLE_SEED);
        }
    }

    // Pre-allocate and initialize matrix to 0
    uint32_t *matrix = (uint32_t *)calloc(totalSweeps_forCluster * NoC, sizeof(uint32_t));
    if (!matrix) {
        fprintf(stderr, "FATAL: Matrix allocation failed.\n");
        if (use_jsmap) free_lazy_mapping(&jmap); else free_Clusters(Clusters);
        l3_release(l3);
        return 1;
    }
    printf("Matrix allocated and initialized to 0\n");

    // Loop order: OUTER over iterations (rounds), INNER over stressors. Each round samples
    // every stressor exactly once before advancing to the next round, so slow machine-state
    // drift (thermal, background load) is decorrelated from stressor identity instead of being
    // baked into a block of consecutive same-stressor samples (which would fingerprint the
    // machine's temporal state, not the stressor). Output filenames are unchanged
    // (data/<clock>/<config>/<stressor>/<iteration>.csv); only the temporal collection order differs.
    for (int iteration = start_iteration; iteration < start_iteration + batch_size; iteration++) {
        printf("\n==================================================\n");
        printf("[*] Round: iteration %d (sampling all %zu stressors once)\n", iteration, (size_t)NUM_STRESSORS);
        printf("==================================================\n");

        for (size_t s_idx = 0; s_idx < NUM_STRESSORS; s_idx++) {

            // 1. DYNAMIC FILE NAMING
            char dynamic_output_path[256];
            // Shuffled Mastik e-set runs go to a distinct tree so data/chrome_clock/ is never
            // overwritten; only applies to timer_mode 1 (where the shuffle is active).
            const char *clock_subdir = (shuffleClusters && timer_mode == 1)
                                       ? "chrome_clock_shuffled" : timer_mode_subdir(timer_mode);
            snprintf(dynamic_output_path, sizeof(dynamic_output_path), "data/%s/%s/%s/%d.csv",
                    clock_subdir,
                    output_dir, stress_battery[s_idx].stressor_name, iteration);

            // Ensure output directory exists
            char mkdir_cmd[512];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p $(dirname %s)", dynamic_output_path);
            system(mkdir_cmd);

            printf("  -> [Round %d/%d | %s] Collecting %s...\n",
                iteration - start_iteration + 1, batch_size,
                stress_battery[s_idx].stressor_name, dynamic_output_path);

            // 2. FORK THE NOISE INJECTOR
            pid_t pid = fork();
            if (pid < 0) {
                perror("FATAL: Fork failed");
                return 1;
            }

            if (pid == 0) {
                // CHILD PROCESS: Pin to Core 1 and execute stressor
                pin_to_core(1);
                execvp(stress_battery[s_idx].exec_args[0], stress_battery[s_idx].exec_args);
                perror("FATAL: execvp failed in child");
                return 1;
            }

            // 3. WAIT FOR STEADY STATE
            // Let the OS handle page faults and let the stressor hit its loop
            usleep(50000); // 500ms

            // 4. MEASURE (The Probe)
            // Note: We pass the dynamically generated filename here
            // Choose measurement function based on timer_mode
            switch(timer_mode) {
                case 0:  // Use rdtscp64()
                    get_spatioTemporal_memoryGram(Clusters, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path);
                    break;
                case 1:  // Use chrome_mock_timer() with Mastik loaded-e_set clusters
                    get_spatioTemporal_memoryGram_ChromeMock(Clusters, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path);
                    break;
                case 2:  // Use chrome_mock_timer() with the JS-style lazy-map victim (K=0 -> dynamic-K)
                    get_spatioTemporal_memoryGram_ChromeMock_jsmap(&jmap, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path, K);
                    break;
                case 3:  // Use rdtscp64() with the JS-style lazy-map victim (K=0 -> dynamic-K)
                    get_spatioTemporal_memoryGram_jsmap(&jmap, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path, K);
                    break;
                default:
                    fprintf(stderr, "ERROR: Unknown timer_mode %d. Use 0 (rdtscp64), 1 (chrome mock), 2 (chrome mock + JS lazy map), or 3 (rdtscp64 + JS lazy map)\n", timer_mode);
                    kill(pid, SIGKILL);
                    waitpid(pid, NULL, 0);
                    return 1;
            }

            // 5. TERMINATE NOISE
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0); // Reap zombie

            // 6. THE COOLDOWN PHASE (CRITICAL)
            // You MUST let the CPU return to an idle baseline before the next loop,
            // otherwise the L3 cache will bleed noise into the next iteration.
            usleep(1000000); // 1 FULL SECOND COOLDOWN

            // Zero out the matrix memory for the next iteration to prevent logical bleeding
            memset(matrix, 0, totalSweeps_forCluster * NoC * sizeof(uint32_t));
        }

        // Inter-round cooldown: let package temperature settle toward baseline so thermal
        // drift doesn't accumulate across rounds (cheap: once per round, not per sample).
        if (iteration + 1 < start_iteration + batch_size) {
            printf("[COOLDOWN] Inter-round cooldown 8s...\n");
            sleep(8);
        }
        
    }
    // FREE resources at the end of this config's iteration
    free(matrix);
    if (use_jsmap) free_lazy_mapping(&jmap); else free_Clusters(Clusters);
    printf("[INFO] Data collection complete, Matrix and victim freed.\n");
    
    return 0;
}





void free_Clusters(Clusters_t *Clusters) {
    if (!Clusters) return;
    free(Clusters->clusterHeads);
    free(Clusters);
}

// Fisher-Yates shuffle of a cluster's circular node list, done ONCE before measuring.
// Randomizing the pointer-chase order breaks the ascending in-page address stream the HW
// prefetcher locks onto -- the native analog of JS build()'s shuffle(pages). It shuffles at
// the LINE level (across eviction sets), so even the fixed in-page offset stride WITHIN a
// Mastik eviction set is broken. It changes ONLY the traversal order, not cluster membership,
// so the set of primed physical sets -- and thus the true coverage on the diagonal -- is
// unchanged. Shared by the coverage validator (native shuffled A/B) and the fingerprinting
// shuffled-cluster experiment (runStressNG_batches, timer_mode 1).
void shuffle_cluster_nodes(Clusters_t *clusters, int c, int assoc) {
    void *head = clusters->clusterHeads[c];
    if (!head) return;
    int n = clusters->counts[c] * assoc;
    if (n < 2) return;

    void **nodes = malloc((size_t)n * sizeof(void *));
    if (!nodes) return;
    void *curr = head;
    for (int i = 0; i < n; i++) { nodes[i] = curr; curr = LNEXT(curr); }

    for (int i = n - 1; i > 0; i--) {          // Fisher-Yates
        int j = rand() % (i + 1);
        void *tmp = nodes[i]; nodes[i] = nodes[j]; nodes[j] = tmp;
    }

    for (int i = 0; i < n; i++) LNEXT(nodes[i]) = nodes[(i + 1) % n];  // re-link circular
    clusters->clusterHeads[c] = nodes[0];
    free(nodes);
}