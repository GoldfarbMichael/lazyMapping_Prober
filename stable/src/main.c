#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <mastik/l3.h>
#include <mastik/util.h>
#include <mastik/impl.h>
#include <sys/mman.h>


#include "utils.h"
#include "tests.h"


#define HUGEPAGE_PATH_A "/dev/hugepages/map_A"
#define HUGEPAGE_PATH_B "/dev/hugepages/map_B"

#define MAPPING_FILE_A "mapping_A.bin"
#define MAPPING_FILE_B "mapping_B.bin"

#define MAX_NUM_GROUPS 64

typedef struct {
    int **lazyGroupsArr;           // 2D array: lazyGroupsArr[group][index] = set index
    int counts[MAX_NUM_GROUPS];    // Number of e_sets in each group
    int lazyGroupSize;             // Max capacity per group
} lazyGroups_t;


void monitorSlice(l3pp_t l3, int slice) {
    int sets_per_slice = l3_getSets(l3) / l3_getSlices(l3);

    int start_set = slice * sets_per_slice;
    int end_set = start_set + sets_per_slice;

    l3_unmonitorall(l3);
    for (int s = start_set; s < end_set; s++) {
        l3_monitor(l3, s);
    }
}


void monitorSlice_manual(l3pp_t l3, int slice, void** e_sets) {
   

    int sets_per_slice = l3_getSets(l3) / l3_getSlices(l3);
    int start_set = slice * sets_per_slice;
    

    int end_set = start_set + sets_per_slice;

    
    l3_unmonitorall(l3);
    for (int s = start_set; s < end_set; s++) {
        l3_monitor_manual(l3, s, e_sets[s]);
    }
}



/*
 * Converts an array of Mastik eviction sets into a group_t array.
 * Groups are determined by bits 7-11 of the address (merging adjacent lines).
 * * e_sets: Array of pointers to linked lists (from get_eviction_sets_via_offsets)
 * num_sets: Size of the e_sets array (e.g., from l3_getSets)
 */
lazyGroups_t* eviction_sets_to_groups(void **e_sets, int num_sets) {
    if (!e_sets) return NULL;

    // Allocate the main struct
    lazyGroups_t *lazyGroups = (lazyGroups_t *)malloc(sizeof(lazyGroups_t));
    if (!lazyGroups) {
        perror("malloc lazyGroups");
        return NULL;
    }

    int lazyGroupSize = num_sets / MAX_NUM_GROUPS;
    lazyGroups->lazyGroupSize = lazyGroupSize;

    // Initialize counts to zero
    memset(lazyGroups->counts, 0, sizeof(lazyGroups->counts));

    // Allocate the 2D array for group indices
    lazyGroups->lazyGroupsArr = (int **)malloc(MAX_NUM_GROUPS * sizeof(int *));
    if (!lazyGroups->lazyGroupsArr) {
        perror("malloc lazyGroupsArr");
        free(lazyGroups);
        return NULL;
    }

    // Allocate each group's array
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        lazyGroups->lazyGroupsArr[g] = (int *)malloc(lazyGroupSize * sizeof(int));
        if (!lazyGroups->lazyGroupsArr[g]) {
            perror("malloc lazyGroup");
            // Free previously allocated groups
            for (int j = 0; j < g; j++) {
                free(lazyGroups->lazyGroupsArr[j]);
            }
            free(lazyGroups->lazyGroupsArr);
            free(lazyGroups);
            return NULL;
        }
        memset(lazyGroups->lazyGroupsArr[g], -1, lazyGroupSize * sizeof(int)); // -1 = empty
    }


    int shiftRight;
    int andTarget;
    if (MAX_NUM_GROUPS == 64) {
        shiftRight = 6;
        andTarget = 0x3F;
    } else if (MAX_NUM_GROUPS == 32) {
        shiftRight = 7;
        andTarget = 0x1F;
    } else if (MAX_NUM_GROUPS == 16) {
        shiftRight = 8;
        andTarget = 0x0F;
    } else {
        fprintf(stderr, "Unsupported MAX_NUM_GROUPS value\n");
        free(lazyGroups);
        return NULL;
    }


    // Iterate through all eviction sets
    for (int i = 0; i < num_sets; i++) {
        if (e_sets[i] == NULL) continue;

        void *curr = e_sets[i];
        int headIdx = ((uintptr_t)curr >> shiftRight) & andTarget;

        // Validate: traverse the circular linked list
        int valid = 1;
        do {
            uintptr_t addr_val = (uintptr_t)curr;
            int group_idx = (addr_val >> shiftRight) & andTarget;
            if (group_idx != headIdx || group_idx >= MAX_NUM_GROUPS) {
                printf("GroupID doesn't match head Group id OR group id >= MAX_NUM_GROUPS --> e_set[%d]\n", i);
                valid = 0;
                break;
            }
            curr = LNEXT(curr);
        } while (curr != e_sets[i]);

        // If valid, assign the e_set index to the group
        if (valid) {
            int group = headIdx;
            int count = lazyGroups->counts[group];
            if (count < lazyGroupSize) {
                lazyGroups->lazyGroupsArr[group][count] = i;
                lazyGroups->counts[group]++;
            } else {
                fprintf(stderr, "Warning: Group %d overflow at set %d\n", group, i);
            }
        }
    }

    return lazyGroups;
}



// Helper function to free lazyGroups_t
void free_lazyGroups(lazyGroups_t *lazyGroups) {
    if (!lazyGroups) return;
    
    if (lazyGroups->lazyGroupsArr) {
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            free(lazyGroups->lazyGroupsArr[g]);
        }
        free(lazyGroups->lazyGroupsArr);
    }
    free(lazyGroups);
}



void dump_lazyGroups_to_file(lazyGroups_t *lazyGroups, const char *filename) {
    if (!lazyGroups || !filename) {
        fprintf(stderr, "Invalid arguments to dump_lazyGroups_to_file\n");
        return;
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("Failed to open file for lazyGroups dump");
        return;
    }

    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        fprintf(fp, "====lazygroup%d - has %d sets====\n", g, lazyGroups->counts[g]);
        
        for (int i = 0; i < lazyGroups->counts[g]; i++) {
            fprintf(fp, "set%d\n", lazyGroups->lazyGroupsArr[g][i]);
        }
    }

    fclose(fp);
    printf("Dumped lazyGroups to %s\n", filename);
}


int main(int argc, char **argv) {
    printf("=== Mastik L3 Cache Mapping and Probing Test ===\n");





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

    // int *transTable = get_transTable(l3, l3B, e_setsA, e_setsB, 16, 1024, 12,0);
    
    // sync_eSetsB_to_eSetsA(e_setsB, 1024, 16, transTable);
    // printf("\nfnished Syncing\n\n");


// ============================================ START: 1 slice Probe tests ============================================


    // get_transTable(l3, l3B, e_setsA, e_setsB, 16, 1024, 12,1);

    // save_physical_mapping(l3B, e_setsB, MAPPING_FILE_B);

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

    lazyGroups_t *groups = eviction_sets_to_groups(e_setsA, sets);
    if (groups) {
        printf("Group 0 has %d sets\n", groups->counts[0]);
        printf("First set in group 0: %d\n", groups->lazyGroupsArr[0][0]);
        
        dump_lazyGroups_to_file(groups, "lazygroups_dump.txt");
        
        free_lazyGroups(groups);
    }

    l3_release(l3);
    l3_release(l3B);
    return 0;
}