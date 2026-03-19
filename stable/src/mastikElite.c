#include <stddef.h>
#include <stdio.h>
#include <unistd.h> 
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mastik/l3.h>
#include <mastik/util.h>
#include <mastik/impl.h>
#include <math.h>
#include "utils.h"
#include "mastikElite.h"

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




void free_Clusters(Clusters_t *Clusters) {
    if (!Clusters) return;
    free(Clusters->clusterHeads);
    free(Clusters);
}