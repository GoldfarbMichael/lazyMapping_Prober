#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <l3.h>
#include "utils.h"

#define EXPECTED_SETS 16384

void prepareL3(l3pp_t *l3, l3info_t info){
    do{
        if (*l3 != NULL)
            l3_release(*l3);
        *l3 = l3_prepare(info);
    }
    while (l3_getSets(*l3) != EXPECTED_SETS);
    
    
    if (*l3 == NULL) {
        fprintf(stderr, "FATAL: l3_prepare failed. Are hugepages enabled? (sudo sysctl -w vm.nr_hugepages=1024)\n");
        return;
    }
}

// Helper to get Physical Address from Virtual Address
uintptr_t virt_to_phys(void *vaddr) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;
    // Calculate which "page number" the virtual address belongs to
    //    (Divide by 4096 because standard pages are 4KB)
    uintptr_t virt_pfn = (uintptr_t)vaddr / 4096;

    // Calculate where in the 'pagemap' file this information is stored
    //    (Multiply by 8 because each entry is 8 bytes long
    off_t offset = virt_pfn * sizeof(uint64_t);
    uint64_t page;
    
    // Read the 64-bit entry from the kernel
    if (pread(fd, &page, sizeof(uint64_t), offset) != sizeof(uint64_t)) {
        close(fd);
        return 0;
    }
    close(fd);

    // Check if present
    if ((page & (1ULL << 63)) == 0) return 0;

    // PFN is bits 0-54
    uintptr_t phys_pfn = page & 0x7FFFFFFFFFFFFFULL;

    // Add the offset-within-the-page back (the last 12 bits)
    return (phys_pfn * 4096) + ((uintptr_t)vaddr % 4096);
}


// --- Step 1: Save Physical Addresses ---
int save_physical_mapping(l3pp_t l3, void **eviction_sets, const char *filename) {
    if (!l3) return -1;
    
    FILE *fp = fopen(filename, "wb");
    if (!fp) { perror("Failed to open output file"); return -1; }

    int numOfSets = l3_getSets(l3);
    int ways = l3_getAssociativity(l3);
    if (!eviction_sets) { 
        fprintf(stderr, "[DEBUG] l3_get_eviction_sets returned NULL!\n");
        fclose(fp);
         return -1;
    }

    uint64_t *buffer = malloc(numOfSets * ways * sizeof(uint64_t));
    int idx = 0;
    int valid_sets_count = 0;
    for (int s = 0; s < numOfSets; s++) {
        void *curr = eviction_sets[s];
        int count = 0;


        // Debug first set only
        if (s == 0) {
            printf("[DEBUG] Inspecting Set 0:\n");
            if (curr == NULL) printf("  -> Set 0 is NULL (Empty)\n");
            else printf("  -> Set 0 Head VA: %p\n", curr);
        }

        // Traverse the list and collect PAs
        if (curr != NULL) {
            valid_sets_count++;
            void *start = curr;
            do {
                if (count < ways) {
                    uintptr_t pa = virt_to_phys(curr);

                    // Debug failures for Set 0
                    if (s == 0 && pa == 0) {
                        printf("  -> virt_to_phys failed for %p\n", curr);
                    }

                    buffer[idx++] = (uint64_t)pa;
                    count++;
                }
                curr = *(void **)curr;
            } while (curr != start && count < ways);
        }

        // Pad with 0 if set is incomplete
        while (count < ways) {
            buffer[idx++] = 0;
            count++;
        }
    }

    // Write to disk
    fwrite(buffer, sizeof(uint64_t), numOfSets * ways, fp);
    
    printf("[Step 1] Saved %d entries (Sets: %d, Ways: %d) to %s\n", 
            idx, numOfSets, ways, filename);

    printf("[Step 1] Saved. Non-empty sets found: %d / %d\n", valid_sets_count, numOfSets);

    free(buffer);
    fclose(fp);
    return 0;
}

// --- Step 1 Test: Load ---
uint64_t* load_physical_mapping(const char *filename, int *out_num_entries) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) { perror("Failed to open mapping file"); return NULL; }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    int num_entries = size / sizeof(uint64_t);
    uint64_t *data = malloc(size);
    
    if (fread(data, 1, size, fp) != (size_t)size) {
        fprintf(stderr, "Read error\n");
        free(data);
        fclose(fp);
        return NULL;
    }

    *out_num_entries = num_entries;
    printf("[Step 1 Test] Loaded %d entries from %s\n", num_entries, filename);
    fclose(fp);
    return data;
}