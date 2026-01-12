#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <l3.h>
#include "utils.h"

#define EXPECTED_SETS 16384
#define HUGE_PAGE_SIZE (2 * 1024 * 1024)
// Max pages to scan. For a 24MB buffer, we need 12. 64 is safe padding.
#define MAX_TRACKED_PAGES 64 

typedef struct {
    uint64_t phys_base;
    uint64_t virt_base;
} page_map_t;



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




// --- Map Hugepage File Directly ---
void *map_hugepage_file(const char *path, size_t size) {
    printf("[Utils] Mapping file: %s (Size: %zu)\n", path, size);
    
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("[Utils] Failed to open file");
        return MAP_FAILED;
    }

    // Ensure size (optional, but good practice if creating new)
    if (ftruncate(fd, size) != 0) {
        perror("[Utils] Failed to resize file");
        close(fd);
        return MAP_FAILED;
    }

    // MAP_SHARED is critical for persistence!
    void *buffer = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // fd is no longer needed

    if (buffer == MAP_FAILED) {
        perror("[Utils] mmap failed");
    }

    return buffer;
}

// --- INTERNAL HELPER: Build PA->VA Lookup Table ---
static int build_page_lookup(void *buf_start, size_t buf_size, page_map_t *map, int max_pages) {
    if (!buf_start || buf_size == 0) return 0;

    uint64_t start_addr = (uint64_t)buf_start;
    int pages_found = 0;

    // Scan in 2MB strides
    for (uint64_t offset = 0; offset < buf_size; offset += HUGE_PAGE_SIZE) {
        if (pages_found >= max_pages) break;

        void *va = (void *)(start_addr + offset);

        // --- FIX: Force Page Fault ---
        // We read a byte from the page. This forces the OS to update the Page Table.
        // Without this, virt_to_phys sees "Page Not Present".
        volatile uint8_t *touch = (volatile uint8_t *)va;
        uint8_t dummy = *touch;
        (void)dummy; // Silence unused warning
        // -----------------------------


        uintptr_t pa = virt_to_phys(va);
        
        if (pa != 0) {
            // Store the base of the 2MB page
            map[pages_found].phys_base = pa & ~(HUGE_PAGE_SIZE - 1);
            map[pages_found].virt_base = (uint64_t)va;
            pages_found++;
        }else {
            // Debug if still failing
            printf("[Utils] Warning: chunk at offset %lu has no PA.\n", offset);
        }
    }
    printf("[Utils] Mapped %d physical hugepages.\n", pages_found);
    return pages_found;
}

// --- INTERNAL HELPER: Translate Single PA ---
static void *translate_pa(uint64_t pa, page_map_t *map, int num_pages) {
    if (pa == 0) return NULL;
    
    uint64_t target_base = pa & ~(HUGE_PAGE_SIZE - 1);
    uint64_t offset = pa & (HUGE_PAGE_SIZE - 1);

    for (int i = 0; i < num_pages; i++) {
        if (map[i].phys_base == target_base) {
            return (void *)(map[i].virt_base + offset);
        }
    }
    return NULL;
}

// --- FUNCTION 1: Physical Buffer -> Virtual Buffer ---
// NOW DECOUPLED: Takes buffer pointer and size, not l3
void **phys_to_virt_buffer(void *buffer, size_t buf_size, uint64_t *phys_addr_buffer, int num_elements) {
    // 1. Build Lookup Table from the provided buffer
    page_map_t lookup[MAX_TRACKED_PAGES];
    int num_pages = build_page_lookup(buffer, buf_size, lookup, MAX_TRACKED_PAGES);
    
    if (num_pages == 0) {
        fprintf(stderr, "[Utils] Error: Could not map any pages in the provided buffer.\n");
        return NULL;
    }

    // 2. Allocate VA Buffer
    void **virt_buffer = malloc(num_elements * sizeof(void*));
    if (!virt_buffer) return NULL;

    // 3. Translate
    int null_count = 0;
    for (int i = 0; i < num_elements; i++) {
        virt_buffer[i] = translate_pa(phys_addr_buffer[i], lookup, num_pages);
        if (phys_addr_buffer[i] != 0 && virt_buffer[i] == NULL) {
            null_count++;
        }
    }

    if (null_count > 0) {
        printf("[Utils] Warning: %d valid PAs could not be translated to VAs.\n", null_count);
    }

    return virt_buffer;
}

// --- FUNCTION 2: Fill Eviction Sets (Reconstruct) ---
// NOW DECOUPLED: Takes buffer pointer and size, not l3
void **fill_eviction_sets(void *buffer, size_t buf_size, uint64_t *phys_addr_buffer, int total_sets, int ways) {
    int num_elements = total_sets * ways;
    
    // 1. Get Virtual Addresses using Function 1
    void **virt_addrs = phys_to_virt_buffer(buffer, buf_size, phys_addr_buffer, num_elements);
    if (!virt_addrs) return NULL;

    // 2. Allocate Output Array (Dense Array of Heads)
    void **eviction_sets = calloc(total_sets, sizeof(void *));
    if (!eviction_sets) { free(virt_addrs); return NULL; }

    int reconstructed_count = 0;
    
    for (int s = 0; s < total_sets; s++) {
        int base_idx = s * ways;
        
        if (virt_addrs[base_idx] == NULL) {
            continue; 
        }

        // 3. Link the ways cyclically
        for (int w = 0; w < ways; w++) {
            void *current_line = virt_addrs[base_idx + w];
            
            // Wrap around for the next pointer
            int next_w = (w + 1) % ways;
            void *next_line = virt_addrs[base_idx + next_w];

            if (current_line && next_line) {
                // WRITE TO MEMORY: Create the pointer chase
                *(void **)current_line = next_line;
            }
        }

        eviction_sets[s] = virt_addrs[base_idx];
        reconstructed_count++;
    }

    free(virt_addrs); 
    
    printf("[Utils] Reconstructed %d eviction sets in buffer %p.\n", reconstructed_count, buffer);
    return eviction_sets;
}