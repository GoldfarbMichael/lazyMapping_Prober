#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <l3.h>
#include <util.h>

int main(int argc, char **argv) {

    // --- STEP 1: Configure the Struct ---
    struct l3info info;
    
    // CRITICAL: Zero out the struct to avoid garbage flags
    memset(&info, 0, sizeof(info));

    // Define the deterministic file path.
    // This file acts as the physical memory anchor.
    // If you run this code 100 times, it uses the exact same physical RAM 
    // (as long as you don't delete the file).
    info.backing_file = "/dev/hugepages/mastik_determ_set_A";

    printf("=== Initializing L3 Deterministically ===\n");
    printf("Backing File: %s\n", info.backing_file);


    // 1. Prepare L3
    // We pass NULL to use standard hugepages (or a specific path if you set one up)
   l3pp_t l3 = NULL;
    do{
        if (l3 != NULL)
            l3_release(l3);
        l3 = l3_prepare(&info);
    }
    while (l3_getSets(l3) != 16384);
    
    
    if (l3 == NULL) {
        fprintf(stderr, "FATAL: l3_prepare failed. Are hugepages enabled? (sudo sysctl -w vm.nr_hugepages=1024)\n");
        return 1;
    }

    // 2. Retrieve Architecture Details
    int sets = l3_getSets(l3);
    int slices = l3_getSlices(l3);
    int ways = l3_getAssociativity(l3);


    // Proof of life: Check if the file exists on the OS
    printf("System check:\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls -lh %s", info.backing_file);
    system(cmd);


    // 3. Print Results
    printf("=== Mastik Linkage Test ===\n");
    printf("Linkage:      SUCCESS\n");
    printf("L3 Sets:      %d\n", sets);
    printf("L3 Slices:    %d\n", slices);
    printf("L3 Ways:      %d\n", ways);
    printf("===========================\n");


// --- STEP 4: Retrieve Eviction Sets ---
    void **e_sets = l3_get_eviction_sets(l3);
    
    if (e_sets == NULL) {
        fprintf(stderr, "ERROR: Failed to retrieve eviction sets.\n");
        free(l3);
        return 1;
    }

    printf("Successfully retrieved eviction sets array. Dumping to file...\n");

    // --- STEP 5: Dump Cyclic Lists to File ---
    FILE *fp = fopen("dump1.txt", "w");
    if (!fp) {
        perror("Error opening file");
        return 1;
    }
  

    // Iterate over every cache set
    for (int s = 0; s < sets; s++) {
        void *head = e_sets[s];
        int i = 0;
        // Skip empty sets (if any)
        if (head == NULL) continue;

        void *curr = head;

        // Header for the set
        fprintf(fp, "=== 111Set %d ===\n", s);

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
    printf("Dump complete. Check 'dump.txt'.\n");

    // Cleanup
    free(e_sets);
    l3_release(l3);
    return 0;
}