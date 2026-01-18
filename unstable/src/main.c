#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <l3.h>
#include <util.h>
#include <sys/mman.h>


#include "utils.h"
#include "tests.h"


#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"

#define MAPPING_FILE_A "mapping_A.bin"
#define MAPPING_FILE_B "mapping_B.bin"

void monitorSlice(l3pp_t l3, int slice) {
    int sets_per_slice = l3_getSets(l3) / l3_getSlices(l3);
    int start_set = slice * sets_per_slice;
    int end_set = start_set + sets_per_slice;

    l3_unmonitorall(l3);
    for (int s = start_set; s < end_set; s++) {
        l3_monitor(l3, s);
    }
}










int main(int argc, char **argv) {

    // // --- STEP 1: Configure the Struct ---
    // struct l3info info;
    // // CRITICAL: Zero out the struct to avoid garbage flags
    // memset(&info, 0, sizeof(info));
    // // Define the deterministic file path.
    // // This file acts as the physical memory anchor.
    // // If you run this code 100 times, it uses the exact same physical RAM 
    // // (as long as you don't delete the file).
    // info.backing_file = HUGEPAGE_PATH_A;

    // printf("=== Initializing L3 Deterministically ===\n");
    // printf("Backing File: %s\n", info.backing_file);


    // l3pp_t l3 = NULL;
    // prepareL3(&l3, &info);
    // // Check if preparation succeeded
    // int sets = l3_getSets(l3);
    // int slices = l3_getSlices(l3);
    // int ways = l3_getAssociativity(l3);
    // // Proof of life: Check if the file exists on the OS
    // printf("System check:\n");
    // char cmd[256];
    // snprintf(cmd, sizeof(cmd), "ls -lh %s", info.backing_file);
    // if (system(cmd) != 0) {
    //     fprintf(stderr, "Warning: system command failed\n");
    // }

    //    // Print Results
    // printf("=== Mastik Test ===\n");
    // printf("L3 Sets:      %d\n", sets);
    // printf("L3 Slices:    %d\n", slices);
    // printf("L3 Ways:      %d\n", ways);
    // printf("===========================\n");

    // struct l3info infoB;
    // // CRITICAL: Zero out the struct to avoid garbage flags
    // memset(&infoB, 0, sizeof(infoB));
    // // Define the deterministic file path.
    // // This file acts as the physical memory anchor.
    // // If you run this code 100 times, it uses the exact same physical RAM 
    // // (as long as you don't delete the file).
    // infoB.backing_file = HUGEPAGE_PATH_B;

    // printf("=== Initializing L3 Deterministically ===\n");
    // printf("Backing File: %s\n", infoB.backing_file);


    // l3pp_t l3B = NULL;
    // prepareL3(&l3B, &infoB);
    // // Check if preparation succeeded
    // int setsB = l3_getSets(l3B);
    // int slicesB = l3_getSlices(l3B);
    // int waysB = l3_getAssociativity(l3B);

    // // Print Results
    // printf("=== Mastik Test ===\n");
    // printf("L3 Sets:      %d\n", setsB);
    // printf("L3 Slices:    %d\n", slicesB);
    // printf("L3 Ways:      %d\n", waysB);
    // printf("===========================\n");


// ------------------------------- End of l3 Initialization -------------------------------
    l3pp_t l3 = prepareDeterministicL3(MAPPING_FILE_A, HUGEPAGE_PATH_A, 16384, 12);
    if(!l3) {
        fprintf(stderr, "Failed to prepare attacker L3 structure.\n");
        return 1;
    }
    l3pp_t l3B = prepareDeterministicL3(MAPPING_FILE_B, HUGEPAGE_PATH_B, 16384, 12);
    if(!l3B) {
        fprintf(stderr, "Failed to prepare victim L3 structure.\n");
        l3_release(l3);
        return 1;
    }
    sleep(1);

    // struct l3info infoA;
    // memset(&infoA, 0, sizeof(infoA));
    // infoA.backing_file = HUGEPAGE_PATH_A;
    // l3pp_t l3_attacker = NULL;
    // prepareL3(&l3_attacker, &infoA);

    

    // sleep(3);
    // struct l3info infoB;
    // memset(&infoB, 0, sizeof(infoB));
    // infoB.backing_file = HUGEPAGE_PATH_B;
    // l3pp_t l3_victim = NULL;
    // prepareL3(&l3_victim, &infoB);

    int setsPerSlice = l3_getSets(l3) / l3_getSlices(l3);
    monitorSlice(l3, 0); // Attacker monitors slice 0
    uint16_t *resAttacker = (uint16_t*) calloc(setsPerSlice, sizeof(uint16_t));
    // GET_PLACE macro can return indices 0-31, so we need at least 32 elements
    uint16_t *resVictim = (uint16_t*) calloc(32, sizeof(uint16_t));
    uint16_t *finalResVictim = (uint16_t*) calloc(l3_getSets(l3B), sizeof(uint16_t));
    // zero out finalResVictim
    memset(finalResVictim, 0, l3_getSets(l3B) * sizeof(uint16_t));
    
    for (int i = 0; i < l3_getSets(l3B); i++) {
        // Victim accesses its monitored set
        //zero out resVictim
        memset(resVictim, 0, 32 * sizeof(uint16_t));
        l3_unmonitorall(l3B);
        l3_monitor(l3B, i); 
        l3_bprobecount(l3B, resVictim);

        l3_probecount(l3, resAttacker);

        l3_probecount(l3B,resVictim);
        finalResVictim[i] = resVictim[0];

    }

    sleep(3);
    printf("Victim Probecount Results:\n");
    // log final results into a file
    FILE *fp = fopen("victim_probecount_results.txt", "w");
    if (!fp) {
        perror("Failed to open output file");
        return 1;
    }
    for (int i = 0; i < l3_getSets(l3B); i++) {
        // printf("Set %d: %d\n", i, finalResVictim[i]);
        fprintf(fp, "Set %d: %u\n", i, finalResVictim[i]);
    }
    fclose(fp);



    l3_release(l3);
    l3_release(l3B);
    free(resAttacker);
    free(resVictim);
    free(finalResVictim);

// ------------------------------- End of l3_prepare_deterministic test -------------------------------




//     // Optional: Investigate the l3pp_t structure
//     investigate_l3pp_t(l3, "investigation.txt");

// // --- Retrieve Eviction Sets ---
//     void **e_sets = l3_get_eviction_sets(l3);
//     if (e_sets == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
//         free(l3);
//         return 1;
//     }

//     // --- Retrieve Eviction Sets ---
//     void **e_setsB = l3_get_eviction_sets(l3B);
//     if (e_setsB == NULL) {
//         fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
//         free(l3B);
//         return 1;
//     }


//     // printf("Successfully retrieved eviction sets array. Dumping to file...\n");
//     // dump_eSets_to_txt(e_sets, sets, "dump.txt");


//     // --- STEP 1 TEST: Save and Load Physical Mapping ---
//     printf("\n=== Testing Save and Load of Physical Mapping ===\n");
//     if (test_save_and_load_physical_mapping(&l3, e_sets, MAPPING_FILE_A) != 0) {
//         fprintf(stderr, "Test failed.\n");
//     }

//     printf("\n=== Testing Save and Load of Physical Mapping ===\n");
//     if (test_save_and_load_physical_mapping(&l3B, e_setsB, MAPPING_FILE_B) != 0) {
//         fprintf(stderr, "Test failed.\n");
//     }

    // if (test_mapping_BIN_reconstruction_to_eSets(MAPPING_FILE_A, HUGEPAGE_PATH_A, "dump_reconstructed.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test failed.\n");
    // }

    //   if (test_mapping_l3_prepare_deterministic(MAPPING_FILE_B, HUGEPAGE_PATH_B, "dump_reconstructed.txt") != 0) {
    //     fprintf(stderr, "Reconstruction Test failed.\n");
    // }



    // free(e_sets);
    // l3_release(l3);
    return 0;
}