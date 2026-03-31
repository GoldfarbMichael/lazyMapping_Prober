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


// Define your massive test battery here. 
// Note: The array MUST be NULL-terminated for execvp.
// StressorConfig stress_battery[] = {
//     // The Scientifically Validated L3 Thrasher
//     { .stressor_name = "cache", .exec_args = {"stress-ng", "--cache", "1", "--cache-flush", "--cache-level", "3", NULL} },
    
//     // Algorithmic & Memory Access Stressors
//     { .stressor_name = "bsearch",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", NULL} },
//     { .stressor_name = "heapsort",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", NULL} },
//     { .stressor_name = "hsearch",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", NULL} },
//     // { .stressor_name = "icache",    .exec_args = {"stress-ng", "--icache", "1", NULL} },
//     { .stressor_name = "judy",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", NULL} },
//     { .stressor_name = "lockbus",   .exec_args = {"stress-ng", "--lockbus", "1", NULL} },
//     { .stressor_name = "lsearch",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", NULL} },
//     { .stressor_name = "malloc",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", NULL} },
//     { .stressor_name = "matrix",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", NULL} },
//     // { .stressor_name = "matrix-3d", .exec_args = {"stress-ng", "--matrix-3d", "1", "--matrix-3d-size", "1024", NULL} },
//     { .stressor_name = "membarrier",.exec_args = {"stress-ng", "--membarrier", "1", NULL}},
//     { .stressor_name = "memcpy",    .exec_args = {"stress-ng", "--memcpy", "1", NULL} },
//     { .stressor_name = "mergesort", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", NULL} },
//     { .stressor_name = "qsort",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", NULL} },
//     { .stressor_name = "radixsort", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", NULL} },
//     { .stressor_name = "shellsort", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", NULL} },
//     { .stressor_name = "skiplist",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", NULL} },
//     { .stressor_name = "str",       .exec_args = {"stress-ng", "--str", "1", NULL} },
//     { .stressor_name = "stream",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", NULL} },
//     { .stressor_name = "tree",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", NULL} },
//     { .stressor_name = "tsearch",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", NULL} },
//     { .stressor_name = "vecmath",   .exec_args = {"stress-ng", "--vecmath", "1", NULL} },
//     { .stressor_name = "wcs",       .exec_args = {"stress-ng", "--wcs", "1", NULL} },
//     { .stressor_name = "zlib",      .exec_args = {"stress-ng", "--zlib", "1", NULL} }
// };

StressorConfig stress_battery[] = {
    // The Scientifically Validated L3 Thrasher
    { .stressor_name = "cache", .exec_args = {"stress-ng", "--cache", "1", "--cache-flush", "--cache-level", "3", "--timeout", "20", NULL} },
    
    // Algorithmic & Memory Access Stressors
    { .stressor_name = "bsearch",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "heapsort",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "hsearch",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "judy",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "lockbus",   .exec_args = {"stress-ng", "--lockbus", "1", "--timeout", "20", NULL} },
    { .stressor_name = "lsearch",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "malloc",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", "--timeout", "20", NULL} },
    { .stressor_name = "matrix",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "membarrier",.exec_args = {"stress-ng", "--membarrier", "1", "--timeout", "20", NULL}},
    { .stressor_name = "memcpy",    .exec_args = {"stress-ng", "--memcpy", "1", "--timeout", "20", NULL} },
    { .stressor_name = "mergesort", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "qsort",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "radixsort", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "shellsort", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "skiplist",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "str",       .exec_args = {"stress-ng", "--str", "1", "--timeout", "20", NULL} },
    { .stressor_name = "stream",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", "--timeout", "20", NULL} },
    { .stressor_name = "tree",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "tsearch",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", "--timeout", "20", NULL} },
    { .stressor_name = "vecmath",   .exec_args = {"stress-ng", "--vecmath", "1", "--timeout", "20", NULL} },
    { .stressor_name = "wcs",       .exec_args = {"stress-ng", "--wcs", "1", "--timeout", "20", NULL} },
    { .stressor_name = "zlib",      .exec_args = {"stress-ng", "--zlib", "1", "--timeout", "20", NULL} }
};

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
            while (chrome_mock_timer(g_tsc_freq_hz, g_context_seed, g_secret_seed) < end_cluster) {
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




int runStressNG_batches(double tst_sec, int batch_size, int start_iteration, char *output_dir, const char *backing_file, const char *BIN_file, int timer_mode) {
    l3pp_t l3 = NULL;
    void **e_sets = NULL;

    load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, backing_file, BIN_file);
    printf("\n=== Testing get_spatioTemporal_memoryGram ===\n");
    // Calculate matrix dimensions
    int NoC = parse_NoC_from_dirname(output_dir);
    int setsPerCluster = l3_getSets(l3)/NoC;

    uint64_t TST_cycles = g_tsc_freq_hz * tst_sec;  
    //print TST_Cycles and g_tsc_freq_hz
    printf("TST_cycles: %lu, g_tsc_freq_hz: %lu\n", TST_cycles, g_tsc_freq_hz);
    uint64_t SST_cycles = 200*1.5*setsPerCluster*l3_getAssociativity(l3);
    
    // For chrome mock timer (mode 1), enforce minimum 500us window by adjusting SST_cycles
    if (timer_mode == 1) {
        uint64_t min_SST_cycles_for_500us = (500 * g_tsc_freq_hz) / 1000000;
        if (SST_cycles < min_SST_cycles_for_500us) {
            printf("[TIMER_MODE=1] SST_cycles adjusted: %lu -> %lu (minimum 500us window)\n", SST_cycles, min_SST_cycles_for_500us);
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


    Clusters_t *Clusters = eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), NoC);
    if (!Clusters) {
    fprintf(stderr, "Failed to create clusters\n");
    return 1;
    }
    
    // Pre-allocate and initialize matrix to 0
    uint32_t *matrix = (uint32_t *)calloc(totalSweeps_forCluster * NoC, sizeof(uint32_t));
    if (!matrix) {
        fprintf(stderr, "FATAL: Matrix allocation failed.\n");
        free_Clusters(Clusters);
        l3_release(l3);
        return 1;
    }
    printf("Matrix allocated and initialized to 0\n");

    for (size_t s_idx = 0; s_idx < NUM_STRESSORS; s_idx++) {
        printf("\n==================================================\n");
        printf("[*] Starting Battery: %s\n", stress_battery[s_idx].stressor_name);
        printf("==================================================\n");

        for (int iteration = start_iteration; iteration < start_iteration + batch_size; iteration++) {
            
            // 1. DYNAMIC FILE NAMING
            char dynamic_output_path[256];
            snprintf(dynamic_output_path, sizeof(dynamic_output_path), "data/%s/%s/%s/%d.csv", 
                    (timer_mode == 1) ? "chrome_clock" : "native_clock",
                    output_dir, stress_battery[s_idx].stressor_name, iteration);

            // Ensure output directory exists
            char mkdir_cmd[512];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p $(dirname %s)", dynamic_output_path);
            system(mkdir_cmd);

            printf("  -> [Iter %d/%d] Collecting %s...\n", 
                iteration - start_iteration + 1, batch_size, dynamic_output_path);

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
                case 1:  // Use chrome_mock_timer()
                    get_spatioTemporal_memoryGram_ChromeMock(Clusters, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path);
                    break;
                default:
                    fprintf(stderr, "ERROR: Unknown timer_mode %d. Use 0 (rdtscp64) or 1 (chrome_mock_timer)\n", timer_mode);
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
    }
    // FREE resources at the end of this config's iteration
    free(matrix);
    free_Clusters(Clusters);
    printf("[INFO] Data collection complete, Matrix and Clusters freed.\n");
    
    return 0;
}





void free_Clusters(Clusters_t *Clusters) {
    if (!Clusters) return;
    free(Clusters->clusterHeads);
    free(Clusters);
}