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
#define MAPPING_FILE_A "mapping_A.bin"

int main(int argc, char **argv) {

    // --- STEP 1: Configure the Struct ---
    struct l3info info;
    // CRITICAL: Zero out the struct to avoid garbage flags
    memset(&info, 0, sizeof(info));
    // Define the deterministic file path.
    // This file acts as the physical memory anchor.
    // If you run this code 100 times, it uses the exact same physical RAM 
    // (as long as you don't delete the file).
    info.backing_file = HUGEPAGE_PATH_A;

    printf("=== Initializing L3 Deterministically ===\n");
    printf("Backing File: %s\n", info.backing_file);


    l3pp_t l3 = NULL;
    prepareL3(&l3, &info);
    // Check if preparation succeeded
    int sets = l3_getSets(l3);
    int slices = l3_getSlices(l3);
    int ways = l3_getAssociativity(l3);
    // Proof of life: Check if the file exists on the OS
    printf("System check:\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls -lh %s", info.backing_file);
    if (system(cmd) != 0) {
        fprintf(stderr, "Warning: system command failed\n");
    }

    // Print Results
    printf("=== Mastik Test ===\n");
    printf("L3 Sets:      %d\n", sets);
    printf("L3 Slices:    %d\n", slices);
    printf("L3 Ways:      %d\n", ways);
    printf("===========================\n");


// --- Retrieve Eviction Sets ---
    void **e_sets = l3_get_eviction_sets(l3);
    if (e_sets == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
        free(l3);
        return 1;
    }

    printf("Successfully retrieved eviction sets array. Dumping to file...\n");
    dump_eSets_to_txt(e_sets, sets, "dump.txt");


    // --- STEP 1 TEST: Save and Load Physical Mapping ---
    printf("\n=== Testing Save and Load of Physical Mapping ===\n");
    if (test_save_and_load_physical_mapping(&l3, e_sets, MAPPING_FILE_A) != 0) {
        fprintf(stderr, "Test failed.\n");
    }



    if (test_mapping_BIN_reconstruction_to_eSets(MAPPING_FILE_A, HUGEPAGE_PATH_A, "dump_reconstructed.txt") != 0) {
        fprintf(stderr, "Reconstruction Test failed.\n");
    }

    free(e_sets);
    l3_release(l3);
    return 0;
}