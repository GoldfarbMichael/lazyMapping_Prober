#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <l3.h>


void prepareL3(l3pp_t *l3, l3info_t info);


uintptr_t virt_to_phys(void *vaddr);

// Step 1: Save Physical Addresses of all sets to disk
// Format: Flat binary array of uint64_t. 
// [Set0_Way0, Set0_Way1... SetN_WayM...]
int save_physical_mapping(l3pp_t l3,void **eviction_sets, const char *filename);

// Step 1 Test: Load Physical Addresses from disk into an array
// Returns a pointer to the allocated array (size = sets * ways * 8 bytes)
uint64_t* load_physical_mapping(const char *filename, int *out_num_entries);


#endif