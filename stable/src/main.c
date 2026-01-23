#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <mastik/l3.h>
#include <mastik/util.h>
#include <sys/mman.h>


#include "utils.h"
#include "tests.h"


#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"

#define MAPPING_FILE_A "mapping_A.bin"
#define MAPPING_FILE_B "mapping_B.bin"

void monitorSlice(l3pp_t l3, int slice) {
    // int sets_per_slice = l3_getSets(l3) / l3_getSlices(l3);
    int sets_per_slice = 1024;

    int start_set = slice * sets_per_slice;
    int end_set = start_set + sets_per_slice;

    l3_unmonitorall(l3);
    for (int s = start_set; s < end_set; s++) {
        l3_monitor(l3, s);
    }
}


void monitorSlice_manual(l3pp_t l3, int slice, void** e_sets) {
    // int sets_per_slice = 1024;
    // int start_set = (slice * sets_per_slice)+1024;

    int sets_per_slice = l3_getSets(l3) / l3_getSlices(l3);
    int start_set = slice * sets_per_slice;
    

    // int end_set = start_set + sets_per_slice;
    int end_set = start_set + 1024;
    
    l3_unmonitorall(l3);
    for (int s = start_set; s < end_set; s++) {
        l3_monitor_manual(l3, s, e_sets[s]);
    }
}


int main(int argc, char **argv) {
    printf("=== Mastik L3 Cache Mapping and Probing Test ===\n");


    // l3pp_t l3 = prepareDeterministicL3(MAPPING_FILE_A, HUGEPAGE_PATH_A, 16384, 12);
    // if(!l3) {
    //     fprintf(stderr, "Failed to prepare attacker L3 structure.\n");
    //     return 1;
    // }
    // l3pp_t l3B = prepareDeterministicL3(MAPPING_FILE_B, HUGEPAGE_PATH_B, 16384, 12);
    // if(!l3B) {
    //     fprintf(stderr, "Failed to prepare victim L3 structure.\n");
    //     l3_release(l3);
    //     return 1;
    // }
    // sleep(1);

    // // struct l3info infoA;
    // // memset(&infoA, 0, sizeof(infoA));
    // // infoA.backing_file = HUGEPAGE_PATH_A;
    // // l3pp_t l3_attacker = NULL;
    // // prepareL3(&l3_attacker, &infoA);

    

    // // sleep(3);
    // // struct l3info infoB;
    // // memset(&infoB, 0, sizeof(infoB));
    // // infoB.backing_file = HUGEPAGE_PATH_B;
    // // l3pp_t l3_victim = NULL;
    // // prepareL3(&l3_victim, &infoB);

    // int setsPerSlice = l3_getSets(l3) / l3_getSlices(l3);
    // monitorSlice(l3, 0); // Attacker monitors slice 0
    // uint16_t *resAttacker = (uint16_t*) calloc(setsPerSlice, sizeof(uint16_t));
    // // GET_PLACE macro can return indices 0-31, so we need at least 32 elements
    // uint16_t *resVictim = (uint16_t*) calloc(32, sizeof(uint16_t));
    // uint16_t *finalResVictim = (uint16_t*) calloc(l3_getSets(l3B), sizeof(uint16_t));
    // // zero out finalResVictim
    // memset(finalResVictim, 0, l3_getSets(l3B) * sizeof(uint16_t));
    
    // for (int i = 0; i < l3_getSets(l3B); i++) {
    //     // Victim accesses its monitored set
    //     //zero out resVictim
    //     memset(resVictim, 0, 32 * sizeof(uint16_t));
    //     l3_unmonitorall(l3B);
    //     l3_monitor(l3B, i); 
    //     l3_bprobecount(l3B, resVictim);

    //     l3_probecount(l3, resAttacker);

    //     l3_probecount(l3B,resVictim);
    //     finalResVictim[i] = resVictim[0];

    // }

    // sleep(3);
    // printf("Victim Probecount Results:\n");
    // // log final results into a file
    // FILE *fp = fopen("victim_probecount_results.txt", "w");
    // if (!fp) {
    //     perror("Failed to open output file");
    //     return 1;
    // }
    // for (int i = 0; i < l3_getSets(l3B); i++) {
    //     // printf("Set %d: %d\n", i, finalResVictim[i]);
    //     fprintf(fp, "Set %d: %u\n", i, finalResVictim[i]);
    // }
    // fclose(fp);



    // l3_release(l3);
    // l3_release(l3B);
    // free(resAttacker);
    // free(resVictim);
    // free(finalResVictim);

// ------------------------------- End of l3_prepare_deterministic test -------------------------------






    // if (test_mapping_BIN_reconstruction_to_eSets(MAPPING_FILE_A, HUGEPAGE_PATH_A, "dump_reconstructed.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test failed.\n");
    // }

    //   if (test_mapping_l3_prepare_deterministic(MAPPING_FILE_B, HUGEPAGE_PATH_B, "dump_reconstructed.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test failed.\n");
    // }



    // free(e_sets);
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


    l3pp_t l3 = prepareBackedL3(HUGEPAGE_PATH_A);
    if(!l3) {
        fprintf(stderr, "Failed to prepare backed L3 structure.\n");
        return 1;
    }

    sleep(3);

    l3pp_t l3B = prepareBackedL3(HUGEPAGE_PATH_B);
    if(!l3B) {
        fprintf(stderr, "Failed to prepare backed L3 structure.\n");
        return 1;
    }


// ******************************************** END: Backed 2 Mastik Prepares (l3, l3B) ********************************************

  
    int sets = 16384; 
    int ways = 12;
    size_t buf_size = 24 * 1024 * 1024; // e.g. 24MB, needs to match what was saved this is what mastik allocates --> we allocated it to hugepages

    void *bufferA = map_hugepage_file(HUGEPAGE_PATH_A, buf_size);
    if (bufferA == MAP_FAILED) return 1;

    void *bufferB = map_hugepage_file(HUGEPAGE_PATH_B, buf_size);
    if (bufferB == MAP_FAILED) return 1;


    // 2. Load PAs
    int count;
    uint64_t *phys_mapA = load_physical_mapping(MAPPING_FILE_A, &count);
    uint64_t *phys_mapB = load_physical_mapping(MAPPING_FILE_B, &count);


    for (int i = 0; i < 12; i++) {
            printf("  Way %d: 0x%lx\n", i, phys_mapA[i]);
    }

    for (int i = 0; i < 12; i++) {
            printf("  Way %d: 0x%lx\n", i, phys_mapB[i]);
    }

    void **e_setsA = fill_eviction_sets(bufferA, buf_size, phys_mapA, sets, ways);
    if (e_setsA == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction setsS.\n");
        return 1;
    }



    void **e_setsB = fill_eviction_sets(bufferB, buf_size, phys_mapB, sets, ways);
    if (e_setsB == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction setsB.\n");
        return 1;
    }





// ============================================ START: 1 slice Probe tests ============================================


    int setsPerSlice = l3_getSets(l3) / l3_getSlices(l3);

    for(int slice = 0; slice < l3_getSlices(l3); slice++){
        printf("Monitoring whole slice\n");
        // monitorSlice(l3, slice); // Attacker monitors slice 0
        monitorSlice_manual(l3, slice, e_setsA); // Victim monitors same slice

        uint16_t *resAttacker = (uint16_t*) calloc(setsPerSlice, sizeof(uint16_t));
        // GET_PLACE macro can return indices 0-31, so we need at least 32 elements
        uint16_t *resVictim = (uint16_t*) calloc(1, sizeof(uint16_t));
        uint16_t *finalResVictim = (uint16_t*) calloc(l3_getSets(l3B), sizeof(uint16_t));
        // zero out finalResVictim
        memset(finalResVictim, 0, l3_getSets(l3B) * sizeof(uint16_t));
        printf("Starting %d-slice probe tests...\n", slice);
        for (int i = 0; i < l3_getSets(l3B); i++) {
            // Victim accesses its monitored set
            //zero out resVictim
            memset(resVictim, 0, sizeof(uint16_t));
            l3_unmonitorall(l3B);
            // l3_monitor(l3B, i); 
            l3_monitor_manual(l3B, i, e_setsB[i]);
            // l3_bprobecount(l3B, resVictim);
            l3_bprobecount(l3B, resVictim);

            l3_probecount(l3, resAttacker);

            l3_probecount(l3B,resVictim);
            finalResVictim[i] = resVictim[0];

        }

        sleep(1);
        printf("Victim Probecount Results:\n");
        // log final results into a file
        char filename[256];
        snprintf(filename, sizeof(filename), "victim_probecount_results%d.txt", slice);
        FILE *fp = fopen(filename, "w");
        if (!fp) {
            perror("Failed to open output file");
            return 1;
        }
        for (int i = 0; i < l3_getSets(l3B); i++) {
            // printf("Set %d: %d\n", i, finalResVictim[i]);
            fprintf(fp, "Set %d: %u\n", i, finalResVictim[i]);
        }
        fclose(fp);
    }

// ******************************************** END: 1 slice Probe tests ********************************************


// // ============================================ START: **Get Eviction Sets** test (by dumping to txt)  ============================================
// //                                                      Load & Save at the end of the section


    // int numOfsets = l3_getSets(l3);
    // printf("Number of sets in L3: %d\n", numOfsets);

    // // --- Retrieve Eviction Sets ---
    // void **e_sets = l3_get_eviction_sets(l3);
    // if (e_sets == NULL) {
    //     fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
    //     l3_release(l3);
    //     return 1;
    // }


    // sleep(1);

    // printf("Successfully retrieved eviction sets array. Dumping to file...\n");
    // dump_eSets_to_txt(e_sets, numOfsets, "dump.txt", 0);


    // // l3_dump_l3memory_pas(l3);
    // // l3_dump_groups(l3, "l3_groups_dump.txt");
    // // l3_print_l3buffer_pas(l3);


    // int numOfsetsB = l3_getSets(l3B);
    // printf("Number of sets in L3: %d\n", numOfsetsB);

    // // --- Retrieve Eviction Sets ---
    // void **e_setsB = l3_get_eviction_sets(l3B);
    // if (e_setsB == NULL) {
    //     fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
    //     l3_release(l3B);
    //     return 1;
    // }

    // printf("Successfully retrieved eviction sets array. Dumping to file...\n");
    // dump_eSets_to_txt(e_setsB, numOfsetsB, "dumpB.txt", 0);

// // @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ START: Save & Load physical mapping test @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

    // sleep(1);
    // // --- STEP 1 TEST: Save and Load Physical Mapping ---
    // printf("\n=== Testing Save and Load of Physical Mapping ===\n");
    // if (test_save_and_load_physical_mapping(&l3, e_sets, MAPPING_FILE_A) != 0) {
    //     fprintf(stderr, "Test failed.\n");
    // }
    


    // printf("\n=== Testing Save and Load of Physical Mapping ===\n");
    // if (test_save_and_load_physical_mapping(&l3B, e_setsB, MAPPING_FILE_B) != 0) {
    //     fprintf(stderr, "Test failed.\n");
    // }

    // free(e_sets);
    // free(e_setsB);
    // return 0;

// // @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ END: Save & Load physical mapping test @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@


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




    l3_release(l3);
    l3_release(l3B);
    return 0;
}