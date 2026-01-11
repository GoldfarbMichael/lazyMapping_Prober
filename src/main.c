#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <l3.h>
#include <util.h>

int main(int argc, char **argv) {

    // 1. Prepare L3
    // We pass NULL to use standard hugepages (or a specific path if you set one up)
   l3pp_t l3;
    do{
        l3 = l3_prepare(NULL);
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

    // 3. Print Results
    printf("=== Mastik Linkage Test ===\n");
    printf("Linkage:      SUCCESS\n");
    printf("L3 Sets:      %d\n", sets);
    printf("L3 Slices:    %d\n", slices);
    printf("L3 Ways:      %d\n", ways);
    printf("===========================\n");

    // Cleanup
    free(l3);
    return 0;
}