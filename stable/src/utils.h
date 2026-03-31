#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include "mastik/l3.h"

#define CLOCK_SPEED 3.6e9 // 3.6 GHz

// ===== Browser Environment Globals =====
extern uint64_t g_tsc_freq_hz;
extern uint32_t g_context_seed;
extern uint32_t g_secret_seed;

void setup_browser_environment(void);

// ===== Utility Functions =====
static inline void maccessMy(void *p) {
    __asm__ volatile("movb (%0), %%al" : : "r"(p) : "eax", "memory");
}


void prepareL3(l3pp_t *l3, int enablePTE);

l3pp_t prepareBackedL3(const char *backing_file);


// l3pp_t prepareDeterministicL3(const char *mapping_file, char *hugePage_file, int num_sets, int ways);

uintptr_t virt_to_phys(void *vaddr);

// Step 1: Save Physical Addresses of all sets to disk
// Format: Flat binary array of uint64_t. 
// [Set0_Way0, Set0_Way1... SetN_WayM...]
int save_physical_mapping(l3pp_t l3, void **eviction_sets, const char *filename);

// Step 1 Test: Load Physical Addresses from disk into an array
// Returns a pointer to the allocated array (size = sets * ways * 8 bytes)
uint64_t* load_physical_mapping(const char *filename, int *out_num_entries);

// 1. Helper to just Map the file (Deterministic "Prepare")
// Returns a pointer to the mapped memory (or MAP_FAILED)
void *map_hugepage_file(const char *path, size_t size);

// 2. Converts physical addresses to virtual addresses WITHIN the given buffer
// (Does not need l3pp_t)
void **phys_to_virt_buffer(void *buffer, size_t buf_size, uint64_t *phys_addr_buffer, int num_elements);

// 3. Reconstructs the linked lists into the provided buffer
// (Does not need l3pp_t)
void **fill_eviction_sets(void *buffer, size_t buf_size, uint64_t *phys_addr_buffer, int total_sets, int ways);

int check_hugepage_contiguity(const char *path, size_t buf_size);


uint64_t get_tsc_freq_hz();
//returns time in jittered 100us resolution, in us units
uint64_t chrome_mock_timer(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed);
uint64_t wait_edge(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed);
uint64_t count_edge(uint64_t tsc_freq_hz, uint32_t context_seed, uint32_t secret_seed);
#endif