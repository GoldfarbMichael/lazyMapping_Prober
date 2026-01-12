#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "tests.h"
#include "utils.h"



void dump_eSets_to_txt(void **e_sets, int numOfSets, const char *filename){
        FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("Error opening file");
        return;
    }
  

    // Iterate over every cache set
    for (int s = 0; s < numOfSets; s++) {
        void *head = e_sets[s];
        int i = 0;
        // Skip empty sets (if any)
        if (head == NULL) continue;

        void *curr = head;

        // Header for the set
        fprintf(fp, "=== Set %d ===\n", s);

        // Cyclic List Traversal
        // We assume pointer chasing: the content of 'curr' is the address of 'next'
        do {
            uintptr_t pa = virt_to_phys(curr);
            fprintf(fp, "address %d: PA=0x%lx\n", i, pa);
            
            // Move to next: dereference the pointer to get the next address
            curr = *(void **)curr;
            i++;
            
        } while (curr != head); // Stop when we circle back to start
    }

    fclose(fp);
    printf("Dump complete. Check %s.\n", filename);
}


int test_save_and_load_physical_mapping(l3pp_t *l3, void **e_sets,const char *filename) {
    int ways = l3_getAssociativity(*l3);
    printf("Discovering sets and saving to: %s\n", filename);
    if (save_physical_mapping(*l3, e_sets, filename) != 0) {
        fprintf(stderr, "Save failed.\n");
        return 1;
    }

    // Capture geometry for verification
    int numOfSets = l3_getSets(*l3);

    // Free L3 (Unmap) to ensure we are testing persistence
    l3_release(*l3); 
    printf("L3 Unmapped. Buffer closed.\n\n");


    // --- Test: Verify by Loading ---
    printf("=== Testing: Loading Mapping ===\n");
    
    int loaded_entries = 0;
    uint64_t *loaded_pa = load_physical_mapping(filename, &loaded_entries);

    if (!loaded_pa) {
        fprintf(stderr, "Load failed.\n");
        return 1;
    }

    // Verification Logic
    int expected_entries = numOfSets * ways;
    printf("Entries Loaded: %d (Expected: %d)\n", loaded_entries, expected_entries);

    if (loaded_entries == expected_entries) {
        printf("SUCCESS: File structure is correct.\n");
        
        // Sample Check: Print first few PAs of Set 0
        printf("Set 0 Physical Addresses:\n");
        for (int i = 0; i < ways; i++) {
            printf("  Way %d: 0x%lx\n", i, loaded_pa[i]);
        }
    } else {
        printf("FAILURE: Entry count mismatch.\n");
    }

    free(loaded_pa);
    return 0;
}