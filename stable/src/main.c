#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <mastik/l3.h>
#include <mastik/util.h>
#include <mastik/impl.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>

#include "utils.h"
#include "tests.h"
#include "mastikElite.h"

#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"

#define MAPPING_FILE_A "mapping_A.bin"
#define MAPPING_FILE_B "mapping_B.bin"


#define PROBE_ITERATIONS 30




void calculate_avg_monitor_and_bprobe_time(l3pp_t l3, void** e_sets) {
    int num_sets = l3_getSets(l3);
    uint64_t* times = (uint64_t*) calloc(num_sets, sizeof(uint64_t));
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    uint16_t* final_res = (uint16_t*) calloc(num_sets, sizeof(uint16_t));
    
    const int ITERATIONS = 100;
    double sum_avg_time_us = 0.0;
    double sum_avg_misses = 0.0;
    for(int iter = 0; iter < ITERATIONS; iter++){
        uint64_t start_cycles, end_cycles;
        // l3_unmonitorall(l3);
        // l3_monitor(l3, 0); // Dummy monitor to stabilize
        for(int i = 0; i < num_sets; i++){
            l3_unmonitorall(l3);
            start_cycles = rdtscp64();
            l3_monitor_manual(l3, i, e_sets[i]);
            end_cycles = rdtscp64();
            l3_bprobecount(l3, res);
            times[i] = (uint64_t)(end_cycles - start_cycles);
            final_res[i] = res[0];
        }
        uint64_t total_time_cycles = 0;
        uint16_t total_probes = 0;
        for(int i = 0; i < num_sets; i++){
            total_time_cycles += times[i];
            total_probes += final_res[i];
        }
        double avg_time_cycles = (double)total_time_cycles / num_sets;
        double avg_time_us = (double)((avg_time_cycles)/CLOCK_SPEED)*1e6; // in us
        double avg_misses = (double)total_probes / num_sets;
        sum_avg_time_us += avg_time_us;
        sum_avg_misses += avg_misses;
    }
    double avg_time_us_total = sum_avg_time_us / ITERATIONS;
    double avg_misses_total = sum_avg_misses / ITERATIONS;
    printf("Average MONITOR + BPROBE time per set: %.3f us\n", avg_time_us_total);
    printf("Average misses per set: %.3f\n", avg_misses_total);
    
}



// uint16_t *check_Cluster(l3pp_t l3, l3pp_t l3B, int* lazyIndexes, int setsPerCluster, void** e_setsA, void** e_setsB, int targetSet){
//     uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
//     uint16_t* final_res = (uint16_t*) calloc(setsPerCluster, sizeof(uint16_t));
//     uint16_t* dummyRes = (uint16_t*) calloc(1, sizeof(uint16_t));
//     memset(final_res, 0, setsPerCluster * sizeof(uint16_t));

//     // l3_unmonitorall(l3B);
//     // l3_monitor_manual(l3B, targetSet, e_setsB[targetSet]);
//     for(int lazySet = 0; lazySet < setsPerCluster; lazySet++){
//         l3_unmonitorall(l3);
//         int lazySet_index = lazyIndexes[lazySet];
//         l3_monitor_manual(l3, lazySet_index, e_setsA[lazySet_index]);

//         for(int i = 0; i < PROBE_ITERATIONS; i++){
//             l3_bprobecount(l3, dummyRes);

//             __asm__ volatile("mfence" ::: "memory");
//             // l3_probecount(l3B, dummyRes);
//             // l3_bprobecount(l3B, dummyRes);
//             // l3_probecount(l3B, dummyRes);
//             // Sweep through the cyclic linked list in e_setsB[targetSet]
//             if (e_setsB[targetSet] != NULL) {
//                 void *curr = e_setsB[targetSet];
//                 do {
//                     maccessMy(curr);
//                     maccessMy(curr);
//                     maccessMy(curr);
//                     maccessMy(curr);
//                     maccessMy(curr);

//                     curr = LNEXT(curr);
//                 } while (curr != e_setsB[targetSet]);
//             }

//             __asm__ volatile("mfence" ::: "memory");


//             l3_probecount(l3, res);

//             //store minimal result
//             if(i == 0 || res[0] < final_res[lazySet]){
//                 final_res[lazySet] = res[0];
//             }
//         }
//     }

//     free(res);
//     free(dummyRes);
//     return final_res;
// }



void dump_Clusters_to_file(Clusters_t *Clusters, int NoC, const char *filename) {
    if (!Clusters) {
        fprintf(stderr, "ERROR: Clusters is NULL\n");
        return;
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "ERROR: Could not open file %s for writing\n", filename);
        return;
    }

    // Write header
    fprintf(fp, "Cluster,AddressCount,Addresses\n");

    // Iterate through each cluster
    for (int i = 0; i < NoC; i++) {
        if (Clusters->clusterHeads[i] == NULL) continue;

        fprintf(fp, "%d,%d,", i, Clusters->counts[i]);

        // Traverse the circular linked list for this cluster using LNEXT
        void *head = Clusters->clusterHeads[i];
        void *curr = head;
        int addrCount=0;
        do {
            addrCount++;
            fprintf(fp, "0x%lx", (uintptr_t)curr);
            curr = LNEXT(curr);
            if (curr != head) {
                fprintf(fp, "|");
            }
        } while (curr != head);

        fprintf(fp, "\n");
        fprintf(fp, "addrCount:%d\n", addrCount);
    }

    fclose(fp);
    printf("Successfully dumped Clusters to %s\n", filename);
}


int main(int argc, char **argv) {



    // Register cleanup handler for Ctrl+C and termination
    signal(SIGINT, cleanup_handler);   // Ctrl+C
    signal(SIGTERM, cleanup_handler);  // Kill signal
    
    // l3pp_t l3 = NULL;
    // l3pp_t l3B = NULL;

    // create2_newBackedMappings_and_syncSetIndexes(&l3, &l3B, HUGEPAGE_PATH_A, HUGEPAGE_PATH_B, MAPPING_FILE_A, MAPPING_FILE_B);


    // l3pp_t l3 = NULL;
    // void **e_sets = NULL;

    // Default values
    double tst_sec = TST_SEC;
    int start_iteration = 0;
    int batch_size = SAMPLES_PER_STRESSOR;
    char *output_dir = "data";
    int timer_mode = 0;  // Default: 0 = native (rdtscp64), 1 = chrome

    // Parse command-line flags for timer mode
    int first_positional_arg = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            timer_mode = 1;  // Chrome mock timer
            printf("[INFO] Timer Mode: Chrome Mock (-c)\n");
        } else if (strcmp(argv[i], "-n") == 0) {
            timer_mode = 0;  // Native rdtscp64
            printf("[INFO] Timer Mode: Native rdtscp64 (-n)\n");
        } else if (argv[i][0] != '-') {
            // First non-flag argument starts positional args
            first_positional_arg = i;
            break;
        }
    }

    // Count positional arguments (non-flag arguments from first_positional_arg onwards)
    int positional_count = 0;
    for (int i = first_positional_arg; i < argc; i++) {
        if (argv[i][0] != '-') {
            positional_count++;
        }
    }

    if (positional_count >= 2) {
        // Argument format: <start_iteration> <batch_size> [output_dir] [-c|-n]
        start_iteration = atoi(argv[first_positional_arg]);
        batch_size = atoi(argv[first_positional_arg + 1]);
        
        if (positional_count >= 3) {
            output_dir = argv[first_positional_arg + 2];
        }
        
        printf("[INFO] Batch Mode Enabled:\n");
        printf("  Start Iteration: %d\n", start_iteration);
        printf("  Batch Size: %d\n", batch_size);
        printf("  Output Directory: %s\n", output_dir);
        
    } else if (argc > 1) {
        fprintf(stderr, "\n❌ USAGE ERROR:\n");
        fprintf(stderr, "Usage: %s [OPTS] <start_iteration> <batch_size> [output_dir]\n\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -c              : Use Chrome mock timer (jittered, 100us clamped)\n");
        fprintf(stderr, "  -n              : Use native rdtscp64 timer (default)\n\n");
        fprintf(stderr, "Arguments:\n");
        fprintf(stderr, "  start_iteration : Starting index for this batch (0-based)\n");
        fprintf(stderr, "  batch_size      : Number of iterations to run in this batch\n");
        fprintf(stderr, "  output_dir      : (Optional) Output directory (default: 'data')\n\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s 0 5              # Run iterations 0-4 with native timer\n", argv[0]);
        fprintf(stderr, "  %s -c 0 5           # Run iterations 0-4 with chrome timer\n", argv[0]);
        fprintf(stderr, "  %s -n 5 5           # Run iterations 5-9 with native timer\n", argv[0]);
        fprintf(stderr, "  %s -c 0 5 /tmp/out  # Run with chrome timer, custom output dir\n\n", argv[0]);
        fprintf(stderr, "Running with defaults: timer_mode=%s, start_iteration=%d, batch_size=%d\n\n", 
                timer_mode == 1 ? "chrome (-c)" : "native (-n)", start_iteration, batch_size);
    }

    // Initialize browser environment globals (MUST be called FIRST before any timer usage)
    setup_browser_environment();
    sleep(0.5);
    runStressNG_batches(tst_sec, batch_size, start_iteration, output_dir, HUGEPAGE_PATH_A, MAPPING_FILE_A, timer_mode);














































    
// ============================================ START: Regular 2 Mastik Prepares (l3, l3B) ============================================
    //  printf("=== Testing simple prepareL3 call ===\n");
    // l3pp_t l3=NULL;
    // prepareL3(&l3, 0);


    // sleep(3);

    // l3pp_t l3B = NULL;
    // prepareL3(&l3B, 0);


// ******************************************** END: Regular 2 Mastik Prepares (l3, l3B) ********************************************








// ============================================ START: Backed 2 Mastik Prepares (l3, l3B) ============================================


//     l3pp_t l3 = prepareBackedL3(HUGEPAGE_PATH_A);
//     if(!l3) {
//         fprintf(stderr, "Failed to prepare backed L3 structure.\n");
//         return 1;
//     }

//     sleep(3);

//     l3pp_t l3B = prepareBackedL3(HUGEPAGE_PATH_B);
//     if(!l3B) {
//         fprintf(stderr, "Failed to prepare backed L3 structure.\n");
//         return 1;
//     }


// // ******************************************** END: Backed 2 Mastik Prepares (l3, l3B) ********************************************

  
//     int sets = 16384; 
//     int ways = 12;
//     size_t buf_size = 24 * 1024 * 1024; // e.g. 24MB, needs to match what was saved this is what mastik allocates --> we allocated it to hugepages

//     void *bufferA = map_hugepage_file(HUGEPAGE_PATH_A, buf_size);
//     if (bufferA == MAP_FAILED) return 1;

//     void *bufferB = map_hugepage_file(HUGEPAGE_PATH_B, buf_size);
    // if (bufferB == MAP_FAILED) return 1;


//     // 2. Load PAs
//     int count;
//     uint64_t *phys_mapA = load_physical_mapping(MAPPING_FILE_A, &count);
//     uint64_t *phys_mapB = load_physical_mapping(MAPPING_FILE_B, &count);


//     for (int i = 0; i < 12; i++) {
//             printf("  Way %d: 0x%lx\n", i, phys_mapA[i]);
//     }

//     for (int i = 0; i < 12; i++) {
//             printf("  Way %d: 0x%lx\n", i, phys_mapB[i]);
//     }

//     void **e_setsA = fill_eviction_sets(bufferA, buf_size, phys_mapA, sets, ways);
//     if (e_setsA == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction setsS.\n");
//         return 1;
//     }



//     void **e_setsB = fill_eviction_sets(bufferB, buf_size, phys_mapB, sets, ways);
//     if (e_setsB == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction setsB.\n");
//         return 1;
//     }

//     int *transTable = get_transTable(l3, l3B, e_setsA, e_setsB, 16, 1024, 12,0);
    
//     sync_eSetsB_to_eSetsA(e_setsB, 1024, 16, transTable);
//     printf("\nfnished Syncing\n\n");


// // ============================================ START: 1 slice Probe tests ============================================


//     get_transTable(l3, l3B, e_setsA, e_setsB, 16, 1024, 12,1);

//     save_physical_mapping(l3B, e_setsB, MAPPING_FILE_B);

// ******************************************** END: 1 slice Probe tests ********************************************


// // ============================================ START: **Get Eviction Sets** test (by dumping to txt)  ============================================
// //                                                      Load & Save at the end of the section


//     int numOfsets = l3_getSets(l3);
//     printf("Number of sets in L3: %d\n", numOfsets);

//     // --- Retrieve Eviction Sets ---
//     void **e_sets = l3_get_eviction_sets(l3);
//     if (e_sets == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
//         l3_release(l3);
//         return 1;
//     }


//     sleep(1);

//     printf("Successfully retrieved eviction sets array. Dumping to file...\n");
//     dump_eSets_to_txt(e_sets, numOfsets, "dump.txt", 0);


//     // l3_dump_l3memory_pas(l3);
//     // l3_dump_groups(l3, "l3_groups_dump.txt");
//     // l3_print_l3buffer_pas(l3);


//     int numOfsetsB = l3_getSets(l3B);
//     printf("Number of sets in L3: %d\n", numOfsetsB);

//     // --- Retrieve Eviction Sets ---
//     void **e_setsB = l3_get_eviction_sets(l3B);
//     if (e_setsB == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
//         l3_release(l3B);
//         return 1;
//     }

//     printf("Successfully retrieved eviction sets array. Dumping to file...\n");
//     dump_eSets_to_txt(e_setsB, numOfsetsB, "dumpB.txt", 0);

// // @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ START: Save & Load physical mapping test @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

//     sleep(1);
//     // --- STEP 1 TEST: Save and Load Physical Mapping ---
//     printf("\n=== Testing Save and Load of Physical Mapping ===\n");
//     if (test_save_and_load_physical_mapping(&l3, e_sets, MAPPING_FILE_A) != 0) {
//         fprintf(stderr, "Test failed.\n");
//     }
    


//     printf("\n=== Testing Save and Load of Physical Mapping ===\n");
//     if (test_save_and_load_physical_mapping(&l3B, e_setsB, MAPPING_FILE_B) != 0) {
//         fprintf(stderr, "Test failed.\n");
//     }

//     free(e_sets);
//     free(e_setsB);
//     return 0;

// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ END: Save & Load physical mapping test @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@


    // free(e_sets);
    // free(e_setsB);




// // ******************************************** END: **Get Eviction Sets** test (by dumping to txt)  ********************************************






// ============================================ START: Reconstruction to from BIN file to eSets test ============================================

    // sleep(3);


    // if (test_mapping_BIN_reconstruction_to_eSets(MAPPING_FILE_A, HUGEPAGE_PATH_A, "dump_reconstructed.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test A failed.\n");
    // }

    // if (test_mapping_BIN_reconstruction_to_eSets(MAPPING_FILE_B, HUGEPAGE_PATH_B, "dump_reconstructedB.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test B failed.\n");
    // }

// ******************************************** End: Reconstruction to from BIN file to eSets test ********************************************
   

    // // Check contiguity before using the hugepages
    // int result = check_hugepage_contiguity(HUGEPAGE_PATH_A, 24 * 1024 * 1024);
    // if (result == 1) {
    //     printf("Hugepages are contiguous - optimal for cache attacks\n");
    // } else if (result == 0) {
    //     printf("Warning: Hugepages are NOT contiguous\n");
    // } else {
    //     printf("Error checking contiguity (need root?)\n");
    // }
        
    
    // uint16_t *resT = (uint16_t*) calloc(1, sizeof(uint16_t));
    // l3_unmonitorall(l3);
    // l3_monitor_manual(l3, 0, e_setsA[0]);
    // l3_probecount(l3, resT);
    // printf("111Probing Set 0, Result: %u\n", resT[0]);
    // l3_unmonitorall(l3);
    // l3_monitor_manual(l3, 0, e_setsA[0]);
    // l3_probecount(l3, resT);
    // printf("222Probing Set 0, Result: %u\n", resT[0]);

    // Clusters_t *Clusters = eviction_sets_to_Clusters(&e_setsA, sets);
    // if (Clusters) {
    //     printf("Cluster 0 has %d sets\n", Clusters->counts[0]);
    //     printf("First set in cluster 0: %d\n", Clusters->ClustersArr[0][0]);
        
    //     dump_Clusters_to_file(Clusters, "Clusters_dump.txt");
        
    //     free_Clusters(Clusters);
    // }
    // calculate_avg_monitor_and_bprobe_time(l3, e_setsA);
    // Create lazy Clusters from eviction sets
    // Clusters_t *Clusters = eviction_sets_to_Clusters(&e_setsA, sets);
    // if (Clusters) {
    //     printf("Starting lazy cluster probe dump...\n");
    //     dump_Clusters_probes_to_jsonl(l3, l3B, Clusters, e_setsA, e_setsB, 
    //                                     sets, "lazycluster_probes.jsonl");
    //     free_Clusters(clusters);
    // } else {
    //     fprintf(stderr, "Failed to create lazy clusters\n");
    // }


    // ============================================ START: MastikElite examples ============================================


    // l3pp_t l3 = NULL;
    // l3pp_t l3B = NULL;
    // void **e_sets = NULL;
    // void **e_setsB = NULL;

    // create2_newBackedMappings_and_syncSetIndexes(&l3, &l3B, HUGEPAGE_PATH_A, HUGEPAGE_PATH_B, MAPPING_FILE_A, MAPPING_FILE_B);
    // newBackedMapping_and_saveEsets_toBinFile(&l3, HUGEPAGE_PATH_A, MAPPING_FILE_A);
    // load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, HUGEPAGE_PATH_A, MAPPING_FILE_A);
    // load_mapping_and_eSetsFrom_BIN_file(&l3B, &e_setsB, HUGEPAGE_PATH_B, MAPPING_FILE_B);

    // int *transTable = get_transTable(l3, l3B, e_sets, e_setsB, 16, 1024, 12,0);
    // printf("[DEBUG MAIN] esets = %p ----- l3=%p\n", e_sets, l3);
    // // Create lazy Clusters from eviction sets
    // printf("[DEBUG] GET SETS: %d\n", l3_getSets(l3));
    // Clusters_t *Clusters = eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), MAX_NUM_CLUSTERS);
    // dump_Clusters_to_file(Clusters, MAX_NUM_CLUSTERS,"Clusters_dump.txt");



    
    // printf("\n=== Testing get_spatioTemporal_memoryGram ===\n");
    
    // int NoC = MAX_NUM_CLUSTERS;
    // int num_sets = l3_getSets(l3);
    
    // // Calculate matrix dimensions
    // uint64_t TST_cycles = CLOCK_SPEED * TST_SEC;  
    // uint64_t SST_cycles = CLOCK_SPEED * SST_SEC;
    // uint64_t total_samples = TST_cycles / (NoC * SST_cycles);
    
    // printf("Total samples per cluster: %lu\n", total_samples);
    // printf("Matrix size: %lu x %d\n", total_samples, NoC);
    
    // // Pre-allocate and initialize matrix to 0
    // uint32_t *matrix = (uint32_t *)calloc(total_samples * NoC, sizeof(uint32_t));
    // if (!matrix) {
    //     fprintf(stderr, "FATAL: Matrix allocation failed.\n");
    //     free_Clusters(Clusters);
    //     l3_release(l3);
    //     return 1;
    // }
    
    // printf("Matrix allocated and initialized to 0\n");
    
    // // Call the function
    // get_spatioTemporal_memoryGram(&e_sets, num_sets, NoC, TST_cycles, SST_cycles, matrix, "memoryGram_output.csv");
    
    // printf("✓ Successfully completed get_spatioTemporal_memoryGram test\n");
    // printf("Output written to memoryGram_output.csv\n");
    
    // // Optional: Print first row of matrix to verify data
    // printf("\nFirst row of matrix (first %d values):\n", NoC);
    // for (int g = 0; g < NoC && g < 10; g++) {
    //     printf("Cluster %d: %u\n", g, matrix[0 * NoC + g]);
    // }

    // // ******************************************** End:  MastikElite examples ********************************************


    // l3_release(l3);
    // free(matrix);
    // free_Clusters(Clusters);
    // l3_release(l3B);
    return 0;
}