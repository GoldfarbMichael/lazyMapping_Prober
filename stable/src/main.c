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

#include "utils.h"
#include "tests.h"
#include "mastikElite.h"

#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"

#define MAPPING_FILE_A "mapping_A.bin"
#define MAPPING_FILE_B "mapping_B.bin"


#define PROBE_ITERATIONS 30
#define MAX_ARGS 10

typedef struct {
    char *stressor_name;
    char *exec_args[MAX_ARGS];
} StressorConfig;

// Define your massive test battery here. 
// Note: The array MUST be NULL-terminated for execvp.
StressorConfig stress_battery[] = {
    // The Scientifically Validated L3 Thrasher
    { .stressor_name = "cache", .exec_args = {"stress-ng", "--cache", "1", "--cache-flush", "--cache-level", "3", NULL} },
    
    // Algorithmic & Memory Access Stressors
    { .stressor_name = "bsearch",   .exec_args = {"stress-ng", "--bsearch", "1", "--maximize", NULL} },
    { .stressor_name = "heapsort",  .exec_args = {"stress-ng", "--heapsort", "1", "--maximize", NULL} },
    { .stressor_name = "hsearch",   .exec_args = {"stress-ng", "--hsearch", "1", "--maximize", NULL} },
    // { .stressor_name = "icache",    .exec_args = {"stress-ng", "--icache", "1", NULL} },
    { .stressor_name = "judy",      .exec_args = {"stress-ng", "--judy", "1", "--maximize", NULL} },
    { .stressor_name = "lockbus",   .exec_args = {"stress-ng", "--lockbus", "1", NULL} },
    { .stressor_name = "lsearch",   .exec_args = {"stress-ng", "--lsearch", "1", "--maximize", NULL} },
    { .stressor_name = "malloc",    .exec_args = {"stress-ng", "--malloc", "1", "--malloc-max", "10000", "--malloc-bytes", "4096", NULL} },
    { .stressor_name = "matrix",    .exec_args = {"stress-ng", "--matrix", "1", "--maximize", NULL} },
    // { .stressor_name = "matrix-3d", .exec_args = {"stress-ng", "--matrix-3d", "1", "--matrix-3d-size", "1024", NULL} },
    { .stressor_name = "membarrier",.exec_args = {"stress-ng", "--membarrier", "1", NULL}},
    { .stressor_name = "memcpy",    .exec_args = {"stress-ng", "--memcpy", "1", NULL} },
    { .stressor_name = "mergesort", .exec_args = {"stress-ng", "--mergesort", "1", "--maximize", NULL} },
    { .stressor_name = "qsort",     .exec_args = {"stress-ng", "--qsort", "1", "--maximize", NULL} },
    { .stressor_name = "radixsort", .exec_args = {"stress-ng", "--radixsort", "1", "--maximize", NULL} },
    { .stressor_name = "shellsort", .exec_args = {"stress-ng", "--shellsort", "1", "--maximize", NULL} },
    { .stressor_name = "skiplist",  .exec_args = {"stress-ng", "--skiplist", "1", "--maximize", NULL} },
    { .stressor_name = "str",       .exec_args = {"stress-ng", "--str", "1", NULL} },
    { .stressor_name = "stream",    .exec_args = {"stress-ng", "--stream", "1", "--stream-index", "3", NULL} },
    { .stressor_name = "tree",      .exec_args = {"stress-ng", "--tree", "1", "--maximize", NULL} },
    { .stressor_name = "tsearch",   .exec_args = {"stress-ng", "--tsearch", "1", "--maximize", NULL} },
    { .stressor_name = "vecmath",   .exec_args = {"stress-ng", "--vecmath", "1", NULL} },
    { .stressor_name = "wcs",       .exec_args = {"stress-ng", "--wcs", "1", NULL} },
    { .stressor_name = "zlib",      .exec_args = {"stress-ng", "--zlib", "1", NULL} }
};

#define NUM_STRESSORS (sizeof(stress_battery) / sizeof(StressorConfig))
#define SAMPLES_PER_STRESSOR 5

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



uint16_t *check_Cluster(l3pp_t l3, l3pp_t l3B, int* lazyIndexes, int setsPerCluster, void** e_setsA, void** e_setsB, int targetSet){
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    uint16_t* final_res = (uint16_t*) calloc(setsPerCluster, sizeof(uint16_t));
    uint16_t* dummyRes = (uint16_t*) calloc(1, sizeof(uint16_t));
    memset(final_res, 0, setsPerCluster * sizeof(uint16_t));

    // l3_unmonitorall(l3B);
    // l3_monitor_manual(l3B, targetSet, e_setsB[targetSet]);
    for(int lazySet = 0; lazySet < setsPerCluster; lazySet++){
        l3_unmonitorall(l3);
        int lazySet_index = lazyIndexes[lazySet];
        l3_monitor_manual(l3, lazySet_index, e_setsA[lazySet_index]);

        for(int i = 0; i < PROBE_ITERATIONS; i++){
            l3_bprobecount(l3, dummyRes);

            __asm__ volatile("mfence" ::: "memory");
            // l3_probecount(l3B, dummyRes);
            // l3_bprobecount(l3B, dummyRes);
            // l3_probecount(l3B, dummyRes);
            // Sweep through the cyclic linked list in e_setsB[targetSet]
            if (e_setsB[targetSet] != NULL) {
                void *curr = e_setsB[targetSet];
                do {
                    maccessMy(curr);
                    maccessMy(curr);
                    maccessMy(curr);
                    maccessMy(curr);
                    maccessMy(curr);

                    curr = LNEXT(curr);
                } while (curr != e_setsB[targetSet]);
            }

            __asm__ volatile("mfence" ::: "memory");


            l3_probecount(l3, res);

            //store minimal result
            if(i == 0 || res[0] < final_res[lazySet]){
                final_res[lazySet] = res[0];
            }
        }
    }

    free(res);
    free(dummyRes);
    return final_res;
}



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


    l3pp_t l3 = NULL;
    void **e_sets = NULL;

    // Default values
    int NoC = MAX_NUM_CLUSTERS;
    double tst_sec = TST_SEC;
    double sst_sec = SST_SEC;
    char *output_path = DEFAULT_SAMPLING_PATH;

    int start_iteration = 0;
    int batch_size = SAMPLES_PER_STRESSOR;
    char *output_dir = "data";

    if (argc >= 3) {
        // Argument format: ./MastikElite <start_iteration> <batch_size> [output_dir]
        start_iteration = atoi(argv[1]);
        batch_size = atoi(argv[2]);
        
        if (argc >= 4) {
            output_dir = argv[3];
        }
        
        printf("[INFO] Batch Mode Enabled:\n");
        printf("  Start Iteration: %d\n", start_iteration);
        printf("  Batch Size: %d\n", batch_size);
        printf("  Output Directory: %s\n", output_dir);
        
    } else if (argc > 1) {
        fprintf(stderr, "\n❌ USAGE ERROR:\n");
        fprintf(stderr, "Usage: %s <start_iteration> <batch_size> [output_dir]\n\n", argv[0]);
        fprintf(stderr, "Arguments:\n");
        fprintf(stderr, "  start_iteration : Starting index for this batch (0-based)\n");
        fprintf(stderr, "  batch_size      : Number of iterations to run in this batch\n");
        fprintf(stderr, "  output_dir      : (Optional) Output directory (default: 'data')\n\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s 0 5              # Run iterations 0-4 (first batch of 5)\n", argv[0]);
        fprintf(stderr, "  %s 5 5              # Run iterations 5-9 (second batch of 5)\n", argv[0]);
        fprintf(stderr, "  %s 0 5 /tmp/output  # Run iterations 0-4, save to /tmp/output\n\n", argv[0]);
        fprintf(stderr, "Running with defaults: start_iteration=%d, batch_size=%d\n\n", 
                start_iteration, batch_size);
    }

 
    



    // Parse command-line arguments
    // if (argc >= 4) {
    //     NoC = atoi(argv[1]);
    //     tst_sec = atof(argv[2]);
    //     sst_sec = atof(argv[3]);

    //     // Optional 4th argument for output path
    //     if (argc >= 5) {
    //         output_path = argv[4];
    //     }
    //     printf("[INFO] Using command-line arguments: NoC=%d, TST_SEC=%.6d, SST_SEC=%.9f, output_path=%s\n", 
    //            NoC, TST_SEC, SST_SEC, output_path);
    
    // } else if (argc > 1) {
    //     fprintf(stderr, "Usage: %s [NoC TST_SEC SST_SEC [output_path]]\n", argv[0]);
    //     fprintf(stderr, "Example: %s 64 32 0.0005 output.csv\n", argv[0]);
    //     fprintf(stderr, "Using defaults: NoC=%d, TST_SEC=%.6d, SST_SEC=%.9f, output_path=%s\n", 
    //             NoC, TST_SEC, SST_SEC, DEFAULT_SAMPLING_PATH);
    // }

    load_mapping_and_eSetsFrom_BIN_file(&l3, &e_sets, HUGEPAGE_PATH_A, MAPPING_FILE_A);
    printf("\n=== Testing get_spatioTemporal_memoryGram ===\n");
    // Calculate matrix dimensions
    uint64_t TST_cycles = CLOCK_SPEED * tst_sec;  
    // uint64_t SST_cycles = CLOCK_SPEED * sst_sec;

    // 1. PIN THE PROBER (PARENT)
    // Assuming Core 0 is isolated or at least stable
    printf("[INFO] Pinning Mastik prober to Core 0...\n");
    pin_to_core(0);    

    // Outer loop: iterate through different target directories and NoC values
    struct {
        const char *target_dir;
        int noc;
    } configs[] = {
        
        // {"2048C_15TST_DynamicSST", 2048}
        // {"1024C_15TST_DynamicSST", 1024}
        // {"512C_15TST_DynamicSST", 512}
        // {"256C_15TST_DynamicSST", 256}
        // {"64C_15TST_DynamicSST", 64}
        // {"32C_15TST_DynamicSST", 32}
        {"16C_15TST_DynamicSST", 16}

        // {"1C_15TST_DynamicSST", 1}
    };

    int num_configs = sizeof(configs) / sizeof(configs[0]);


    for (int config_idx = 0; config_idx < num_configs; config_idx++) {
        const char *current_target_dir = configs[config_idx].target_dir;
        NoC = configs[config_idx].noc;

        Clusters_t *Clusters = eviction_sets_to_Clusters(&e_sets, l3_getSets(l3), NoC);
        if (!Clusters) {
        fprintf(stderr, "Failed to create clusters\n");
        return 1;
        }
        
        printf("\n[INFO] Processing config: %s with NoC=%d\n", current_target_dir, NoC);

        int setsPerCluster = l3_getSets(l3)/NoC;
        uint64_t SST_cycles = 200*1.5*setsPerCluster*l3_getAssociativity(l3);
        printf("Sets PER CLUSTER %d, NoC %d, SST_Cycles: %lu\n", setsPerCluster, NoC, SST_cycles);

        uint64_t total_samples = TST_cycles / (NoC * SST_cycles);

        printf("Total samples per cluster: %lu\n", total_samples);
        printf("Matrix size: %lu x %d\n", total_samples, NoC);
        
        // Pre-allocate and initialize matrix to 0
        uint32_t *matrix = (uint32_t *)calloc(total_samples * NoC, sizeof(uint32_t));
        if (!matrix) {
            fprintf(stderr, "FATAL: Matrix allocation failed.\n");
            free_Clusters(Clusters);
            l3_release(l3);
            return 1;
        }
        printf("Matrix allocated and initialized to 0\n");
    

        for (int s_idx = 0; s_idx < NUM_STRESSORS; s_idx++) {
            printf("\n==================================================\n");
            printf("[*] Starting Battery: %s\n", stress_battery[s_idx].stressor_name);
            printf("==================================================\n");

            for (int iteration = start_iteration; iteration < start_iteration + batch_size; iteration++) {
                
                // 1. DYNAMIC FILE NAMING
                char dynamic_output_path[256];
                snprintf(dynamic_output_path, sizeof(dynamic_output_path), "data/%s/%s/%d.csv", 
                        current_target_dir, stress_battery[s_idx].stressor_name, iteration);

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
                    exit(1);
                }

                if (pid == 0) {
                    // CHILD PROCESS: Pin to Core 1 and execute stressor
                    pin_to_core(1); 
                    execvp(stress_battery[s_idx].exec_args[0], stress_battery[s_idx].exec_args);
                    perror("FATAL: execvp failed in child"); 
                    exit(1);
                }

                // 3. WAIT FOR STEADY STATE
                // Let the OS handle page faults and let the stressor hit its loop
                usleep(50000); // 500ms

                // 4. MEASURE (The Probe)
                // Note: We pass the dynamically generated filename here
                get_spatioTemporal_memoryGram(Clusters, NoC, TST_cycles, SST_cycles, matrix, dynamic_output_path);

                // 5. TERMINATE NOISE
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0); // Reap zombie

                // 6. THE COOLDOWN PHASE (CRITICAL)
                // You MUST let the CPU return to an idle baseline before the next loop,
                // otherwise the L3 cache will bleed noise into the next iteration.
                usleep(1000000); // 1 FULL SECOND COOLDOWN

                // Zero out the matrix memory for the next iteration to prevent logical bleeding
                memset(matrix, 0, total_samples * NoC * sizeof(uint32_t));
            }
        }
        // FREE resources at the end of this config's iteration
        free(matrix);
        free_Clusters(Clusters);
        printf("[INFO] Data collection complete for config %d. Matrix and Clusters freed.\n", config_idx);
    }
    


    // l3_release(l3);




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