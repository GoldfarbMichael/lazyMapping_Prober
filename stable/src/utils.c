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
#include "utils.h"
#include "murmur3.h"

#define EXPECTED_SETS 16384
#define HUGE_PAGE_SIZE (2 * 1024 * 1024)
// Max pages to scan. For a 24MB buffer, we need 12. 64 is safe padding.
#define MAX_TRACKED_PAGES 64 




// -------------------------------- Mock browser env Start --------------------------------
// 1. Global State
uint64_t g_tsc_freq_hz = 0;
uint32_t g_context_seed = 0x12345678; // Simulates a specific victim website origin
uint32_t g_secret_seed  = 0;

// 2. Initialization Function (Run ONCE at startup)
void setup_browser_environment() {
    // A. Calibrate TSC
    g_tsc_freq_hz = get_tsc_freq_hz();
    if (g_tsc_freq_hz == 0) {
        fprintf(stderr, "Fatal: TSC Calibration failed.\n");
        // Exit or handle error
    }

    // B. Seed Initialization based on Compile-Time Flag
#ifdef LAB_DETERMINISTIC_MODE
    printf("[LAB MODE] Using static secret seed for distribution testing.\n");
    g_secret_seed = 0xCAFEBABE;
#else
    printf("[DATASET MODE] Generating dynamic secret seed for ML collection.\n");
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, &g_secret_seed, sizeof(g_secret_seed)) != sizeof(g_secret_seed)) {
        fprintf(stderr, "Fatal: Failed to read /dev/urandom.\n");
        // Exit or handle error
    }
    if (fd >= 0) close(fd);
#endif
}

// -------------------------------- Mock browser env End --------------------------------






typedef struct {
    uint64_t phys_base;
    uint64_t virt_base;
} page_map_t;



void prepareL3(l3pp_t *l3, int enablePTE) {
    l3info_t l3i = (l3info_t)malloc(sizeof(struct l3info));
    if (!l3i) {
        fprintf(stderr, "Failed to allocate l3info\n");
        return;
    }
    
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    printf("Filling l3info structure...\n");
    start_cycles = rdtscp64();
    do{
        if (*l3 != NULL)
            l3_release(*l3);
        *l3 = l3_prepare(l3i, NULL, enablePTE);
    }
    while (l3_getSets(*l3) != EXPECTED_SETS);
    
    end_cycles = rdtscp64();
    
    double time_cycles = (double)((end_cycles - start_cycles) / CLOCK_SPEED) * 1e3; // in ms
    printf("L3 preparation took VIA CYCLES: %.3f ms\n", time_cycles);

    if (*l3) {
        printf("L3 Cache Sets: %d\n", l3_getSets(*l3));
        printf("L3 Cache Slices: %d\n", l3_getSlices(*l3));
        printf("L3 Cache num of lines: %d\n", l3_getAssociativity(*l3));
    }

    free(l3i);
}



l3pp_t prepareBackedL3(const char *backing_file){
    
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    printf("Filling l3info structure...\n");
    start_cycles = rdtscp64();
    l3pp_t l3 = NULL;
    do{
        if (l3 != NULL)
            l3_release(l3);
        l3 = l3_prepare_backed(backing_file);
    }
    while (l3_getSets(l3) != EXPECTED_SETS);
    
    end_cycles = rdtscp64();
    
    double time_cycles = (double)((end_cycles - start_cycles) / CLOCK_SPEED) * 1e3; // in ms
    printf("L3 preparation took VIA CYCLES: %.3f ms\n", time_cycles);

    if (l3) {
        printf("L3 Cache Sets: %d\n", l3_getSets(l3));
        printf("L3 Cache Slices: %d\n", l3_getSlices(l3));
        printf("L3 Cache num of lines: %d\n", l3_getAssociativity(l3));
    }

    return l3;
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
    
    printf("[Utils] Reconstructed %d eviction sets in buffer PA=0x%lx; VA=%p.\n", reconstructed_count, virt_to_phys(buffer), buffer);
    return eviction_sets;
}


int check_hugepage_contiguity(const char *path, size_t buf_size) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("[Contiguity Check] Cannot open hugepage file");
        return -1;
    }

    void *buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (buf == MAP_FAILED) {
        perror("[Contiguity Check] mmap failed");
        return -1;
    }

    int num_pages = buf_size / HUGE_PAGE_SIZE;
    
    // Touch each page to ensure it's allocated
    for (int i = 0; i < num_pages; i++) {
        ((volatile char *)buf)[i * HUGE_PAGE_SIZE] = 0;
    }

    printf("\n=== Hugepage Contiguity Check: %s ===\n", path);
    printf("%-4s  %-18s  %-18s  %s\n", "Page", "Virtual Addr", "Physical Addr", "Contiguous?");
    printf("----  ------------------  ------------------  -----------\n");

    uint64_t prev_phys = 0;
    int all_contiguous = 1;

    for (int i = 0; i < num_pages; i++) {
        void *vaddr = buf + (i * HUGE_PAGE_SIZE);
        uint64_t phys = virt_to_phys(vaddr);

        const char *status = "";
        if (i > 0) {
            if (phys == prev_phys + HUGE_PAGE_SIZE) {
                status = "YES";
            } else {
                status = "NO";
                all_contiguous = 0;
            }
        }

        printf("%-4d  0x%016lx  0x%016lx  %s\n", i, (uintptr_t)vaddr, phys, status);
        prev_phys = phys;
    }

    printf("\n==> Result: %s\n\n", all_contiguous ? "ALL CONTIGUOUS ✓" : "NOT CONTIGUOUS ✗");

    munmap(buf, buf_size);
    return all_contiguous;
}



uint64_t get_tsc_freq_hz() {
    uint32_t eax, ebx, ecx, edx;
    
    // Execute CPUID with EAX = 0x15, ECX = 0
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x15), "c"(0));

    // If EAX or EBX is 0, Leaf 0x15 is not supported by this microarchitecture
    if (eax == 0 || ebx == 0) {
        fprintf(stderr, "Error: CPUID Leaf 0x15 not supported on this silicon.\n");
        return 0; 
    }

    // On some microarchitectures (e.g., Skylake), ECX returns 0.
    // In those cases, the core crystal clock is typically 24 MHz.
    if (ecx == 0) {
        ecx = 24000000; 
    }

    // TSC_Frequency = (Crystal_Clock * Numerator) / Denominator
    return ((uint64_t)ecx * ebx) / eax;
}



// Define the exact Memory Tuple expected by the browser engine
// __attribute__((packed)) prevents the compiler from adding padding bytes, 
// ensuring strict deterministic hashing of the memory block.
struct __attribute__((packed)) ChromeHashTuple {
    uint64_t clamped_time;
    uint32_t context_seed;
};

// Updated native timer function
uint64_t chrome_mock_timer(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed) {
    uint32_t aux;
    // Get raw cycles
    uint64_t current_cycles = __builtin_ia32_rdtscp(&aux); 

    // Calculate cycles per 100 microseconds (Time = Cycles / Freq)
    uint64_t cycles_per_100us = tsc_freq_hz / 10000;
    if (cycles_per_100us == 0) return current_cycles; // Safety catch

    // Clamp the time to the nearest lower 100us boundary
    uint64_t clamped_cycles = current_cycles - (current_cycles % cycles_per_100us);

    // Construct the Hash Tuple (clamped_time + context_seed)
    struct ChromeHashTuple tuple;
    tuple.clamped_time = clamped_cycles;
    tuple.context_seed = context_seed;

    // Hash the Tuple using the secret_seed
    uint32_t hash_out;
    // We hash the entire struct, and use secret_seed as the Murmur3 seed
    MurmurHash3_x86_32(&tuple, sizeof(tuple), secret_seed, &hash_out);

    // Calculate a uniform midpoint between the current clamp and the next clamp

    uint64_t midpoint = clamped_cycles + (hash_out % cycles_per_100us);
    uint64_t result_cycles;
    if (current_cycles < midpoint) {
        result_cycles = clamped_cycles;
    } else {
        result_cycles = clamped_cycles + cycles_per_100us;
    }


    // 8. Convert cycles to microseconds and return as int
    return (uint64_t)((result_cycles * 1000000) / tsc_freq_hz);
}

uint64_t wait_edge(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed){
    uint64_t current = chrome_mock_timer(tsc_freq_hz, context_seed, secret_seed);
    volatile uint64_t next=chrome_mock_timer(tsc_freq_hz, context_seed, secret_seed);
    while(current == (next = chrome_mock_timer(tsc_freq_hz, context_seed, secret_seed))){}
    return next;
}

uint64_t count_edge(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed){
    uint64_t current = chrome_mock_timer(tsc_freq_hz, context_seed, secret_seed);
    volatile uint64_t count=0;
    while(current == chrome_mock_timer(tsc_freq_hz, context_seed, secret_seed)){
        count++;
    }
    return count;
}
