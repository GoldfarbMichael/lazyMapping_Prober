/*
 * Copyright 2016 CSIRO
 *
 * This file is part of Mastik.
 *
 * Mastik is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mastik is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Mastik.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/vm_statistics.h>
#endif

#include <mastik/mm.h>
#include <mastik/l3.h>
#include <mastik/low.h>
#include <mastik/impl.h>
#include <mastik/lx.h>

#include "vlist.h"
#include "mm-impl.h"
#include "timestats.h"
#include "tsx.h"

#define CHECKTIMES 16

/*
 * Intel documentation still mentiones one slice per core, but
 * experience shows that at least some Skylake models have two
 * smaller slices per core. 
 * When probing the cache, we can use the smaller size - this will
 * increase the probe time but for huge pages, where we use
 * the slice size, the probe is fast and the increase is not too
 * significant.
 * When using the PTE maps, we need to know the correct size and
 * the correct number of slices.  This means that, currently and
 * without user input, PTE is not guaranteed to work.
 * So, on a practical note, L3_GROUPSIZE_FOR_HUGEPAGES is the
 * smallest slice size we have seen; L3_SETS_PER_SLICE is the
 * default for the more common size.  If we learn how to probe
 * the slice size we can get rid of this mess.
 */

struct l3pp {
  void **monitoredhead;
  int nmonitored;
  
  int *monitoredset;
  
  uint32_t *monitoredbitmap;
    
  cachelevel_e level;
  size_t totalsets;
  
  int ngroups;
  int groupsize;
  vlist_t *groups;
  
  struct l3info l3info;
  
  mm_t mm; 
  uint8_t internalmm;
  
  // To reduce probe time we group sets in cases that we know that a group of consecutive cache lines will
  // always map to equivalent sets. In the absence of user input (yet to be implemented) the decision is:
  // Non linear mappings - 1 set per group (to be implemeneted)
  // Huge pages - L3_SETS_PER_SLICE sets per group (to be impolemented)
  // Otherwise - L3_SETS_PER_PAGE sets per group.
};

int loadL3cpuidInfo(l3info_t l3info) {
  for (int i = 0; ; i++) {
    l3info->cpuidInfo.regs.eax = CPUID_CACHEINFO;
    l3info->cpuidInfo.regs.ecx = i;
    cpuid(&l3info->cpuidInfo);
    if (l3info->cpuidInfo.cacheInfo.type == 0)
      return 0;
    if (l3info->cpuidInfo.cacheInfo.level == 3)
      return 1;
  }
}

void fillL3Info(l3info_t l3info) {
  loadL3cpuidInfo(l3info);
  if (l3info->associativity == 0)
    l3info->associativity = l3info->cpuidInfo.cacheInfo.associativity + 1;
  if (l3info->slices == 0) {
    if (l3info->setsperslice == 0)
      l3info->setsperslice = L3_SETS_PER_SLICE;
    l3info->slices = (l3info->cpuidInfo.cacheInfo.sets + 1)/ l3info->setsperslice;
  }
  if (l3info->setsperslice == 0)
    l3info->setsperslice = (l3info->cpuidInfo.cacheInfo.sets + 1)/l3info->slices;
  if (l3info->bufsize == 0) {
    l3info->bufsize = l3info->associativity * l3info->slices * l3info->setsperslice * L3_CACHELINE * 2;
    if (l3info->bufsize < 10 * 1024 * 1024)
      l3info->bufsize = 10 * 1024 * 1024;
  }
}

#if 0
void printL3Info() {
  struct l3info l3Info;
  loadL3cpuidInfo(&l3Info);
  printf("type            : %u\n", l3Info.cpuidInfo.cacheInfo.type);
  printf("level           : %u\n", l3Info.cpuidInfo.cacheInfo.level);
  printf("selfInitializing: %u\n", l3Info.cpuidInfo.cacheInfo.selfInitializing);
  printf("fullyAssociative: %u\n", l3Info.cpuidInfo.cacheInfo.fullyAssociative);
  printf("logIds          : %u\n", l3Info.cpuidInfo.cacheInfo.logIds + 1);
  printf("phyIds          : %u\n", l3Info.cpuidInfo.cacheInfo.phyIds + 1);
  printf("lineSize        : %u\n", l3Info.cpuidInfo.cacheInfo.lineSize + 1);
  printf("partitions      : %u\n", l3Info.cpuidInfo.cacheInfo.partitions + 1);
  printf("associativity   : %u\n", l3Info.cpuidInfo.cacheInfo.associativity + 1);
  printf("sets            : %u\n", l3Info.cpuidInfo.cacheInfo.sets + 1);
  printf("wbinvd          : %u\n", l3Info.cpuidInfo.cacheInfo.wbinvd);
  printf("inclusive       : %u\n", l3Info.cpuidInfo.cacheInfo.inclusive);
  printf("complexIndex    : %u\n", l3Info.cpuidInfo.cacheInfo.complexIndex);
  exit(0);
}
#endif

void prime(void *pp, int reps) {
  walk((void *)pp, reps);
}

#define str(x) #x
#define xstr(x) str(x)

l3pp_t l3_prepare(l3info_t l3info, mm_t mm, int enablePTE) {
  // Setup
  l3pp_t l3 = (l3pp_t)malloc(sizeof(struct l3pp));
  bzero(l3, sizeof(struct l3pp));
  if (l3info != NULL)
    bcopy(l3info, &l3->l3info, sizeof(struct l3info));
  fillL3Info(&l3->l3info);
  l3->level = L3;

  // Check if linearmap and quadratic map are called together
  if ((l3->l3info.flags & (L3FLAG_LINEARMAP | L3FLAG_QUADRATICMAP)) == (L3FLAG_LINEARMAP | L3FLAG_QUADRATICMAP)) {
    free(l3);
    fprintf(stderr, "Error: Cannot call linear and quadratic map together\n");
    return NULL;
  }
  
  l3->mm = mm;
  if (l3->mm == NULL) {
    printf("[DEBUG] Mastik: l3.c; l3_prepare; l3->mm = mm_prepare(NULL, NULL, (lxinfo_t)l3info); --> Creating internal mm for l3\n");
    l3->mm = mm_prepare(NULL, NULL, (lxinfo_t)l3info);
    l3->internalmm = 1;
  }
   if (enablePTE) {
    enable_PTE_flag(l3->mm);
  }
  
  if (!mm_initialisel3(l3->mm)) 
    return NULL;
  
  l3->ngroups = l3->mm->l3ngroups;
  l3->groupsize = l3->mm->l3groupsize;
  
  // Allocate monitored set info
  l3->monitoredbitmap = (uint32_t *)calloc((l3->ngroups*l3->groupsize/32) + 1, sizeof(uint32_t));
  l3->monitoredset = (int *)malloc(l3->ngroups * l3->groupsize * sizeof(int));
  l3->monitoredhead = (void **)malloc(l3->ngroups * l3->groupsize * sizeof(void *));
  l3->nmonitored = 0;
  l3->totalsets = l3->ngroups * l3->groupsize;

  return l3;
}

void l3_release(l3pp_t l3) {
  lx_release((lxpp_t)l3);
}


/**
 * @brief Target function for graphing.
 */
int l3_monitor(l3pp_t l3, int line) {
  return lx_monitor((lxpp_t) l3, line);
}

int l3_unmonitor(l3pp_t l3, int line) {
  return lx_unmonitor((lxpp_t) l3, line);
}

void l3_unmonitorall(l3pp_t l3) {
  lx_unmonitorall((lxpp_t) l3);
}

int l3_getmonitoredset(l3pp_t l3, int *lines, int nlines) {
  return lx_getmonitoredset((lxpp_t) l3, lines, nlines);
}

void l3_randomise(l3pp_t l3) {
  lx_randomise((lxpp_t) l3);
}

void l3_probe(l3pp_t l3, uint16_t *results) {
  lx_probe((lxpp_t) l3, results);
}

void l3_bprobe(l3pp_t l3, uint16_t *results) {
  lx_bprobe((lxpp_t) l3, results);
}

void l3_probecount(l3pp_t l3, uint16_t *results) {
  lx_probecount((lxpp_t) l3, results);
}

void l3_bprobecount(l3pp_t l3, uint16_t *results) {
  lx_bprobecount((lxpp_t) l3, results);
}

// Returns the number of probed sets in the LLC
int l3_getSets(l3pp_t l3) {
  return l3->ngroups * l3->groupsize;
}

// Returns the number slices
int l3_getSlices(l3pp_t l3) {
  return l3->l3info.slices;
}

// Returns the LLC associativity
int l3_getAssociativity(l3pp_t l3) {
  return l3->l3info.associativity;
}

int l3_repeatedprobe(l3pp_t l3, int nrecords, uint16_t *results, int slot) {
  return lx_repeatedprobe((lxpp_t) l3, nrecords, results, slot);
}

int l3_repeatedprobecount(l3pp_t l3, int nrecords, uint16_t *results, int slot) {
  return lx_repeatedprobecount((lxpp_t) l3, nrecords, results, slot);
}

void l3_pa_prime(l3pp_t l3) {
  for (int i = 0; i < l3->nmonitored; i++) {
    int t = probetime(l3->monitoredhead[i]);
  }
}

/* A single round of TSX abort detection, decide existing time period of 
 * RTM region by modifing 'time_limit'.  It returns 0 if there is no TSX 
 * abort; It returns 1 if the abort is caused by internal buffer overflow or
 * memory address conflict; It returns -1 if abort is caused by other reasons,
 * e.g. context switch, some micro instructions 
 */
int l3_pabort(l3pp_t l3, uint32_t time_limit) {
  unsigned ret; 

  if ((ret = xbegin()) == XBEGIN_INIT){
    l3_pa_prime(l3); 
    uint32_t s = rdtscp();
    while((rdtscp() - s) < time_limit)
      ;
    xend();
  } else {
    if ((ret & XABORT_CAPACITY) || (ret & XABORT_CONFLICT) )
      return 1;
    else
      return -1; 
  }
  return 0;
}

/* Run TSX abort detection repeatedly. 'Sample': How many repeats will 
 * be performed; 'results': store results of l3_pabort()
 */
void l3_repeatedpabort(l3pp_t l3, int sample, int16_t *results, uint32_t time_limit) {
  int cont = 0;

  do{ 
    results[cont] = l3_pabort(l3, time_limit);
  } while(cont++ < sample);
}


// ============================================ My additions here ============================================

void l3_testHeaders(int dummy){
    printf("L3 and related headers are working correctly. HERE IS MY DUMMY %d\n", dummy);

}


void **l3_get_eviction_sets(l3pp_t l3) {
    if (!l3) return NULL;

    // 1. Calculate Total Sets (Internal access)
    int total_sets = l3_getSets(l3);
    // 2. Monitor ALL sets to populate internal structures
    // (This fills l3->monitoredhead and l3->monitoredset)
    for (int i = 0; i < total_sets; i++) {

        uint64_t start_cycles = rdtscp64();
        l3_monitor(l3, i);
        uint64_t end_cycles = rdtscp64();
        double time_cycles = (double)((end_cycles - start_cycles) / 3.1e9) * 1e3; // in ms
        // printf("Monitoring set %d took: %.3f ms\n", i, time_cycles);
    }
    // 3. Allocate the Dense Array
    // We use calloc to ensure unmapped sets are NULL
    void **dense_array = (void **)calloc(total_sets, sizeof(void *));
    if (!dense_array) return NULL;

    // 4. Map Internal Sparse Arrays to Dense Array
    // We access the struct members DIRECTLY now.
    // l3->nmonitored:   Count of active sets
    // l3->monitoredset: Array of Set IDs [idx0, idx1, ...]
    // l3->monitoredhead: Array of Pointers [ptr0, ptr1, ...]
    
    for (int i = 0; i < l3->nmonitored; i++) {
        int set_id = l3->monitoredset[i];
        void *head = l3->monitoredhead[i];
        // if(i % 256 == 0) {
        //     printf("i IS: %d set_idx IS: %d\n", i, set_id);
        // }
        if (set_id >= 0 && set_id < total_sets) {
            dense_array[set_id] = head;
        }
    }

    // 5. Cleanup
    // Clear Mastik's internal monitoring list so the user starts fresh
    // l3_print_l3buffer_pas(l3);
    l3_dump_groups(l3, "l3_groups_dump.txt");

    l3_unmonitorall(l3);

    return dense_array;
}


// Helper to get Physical Address from Virtual Address
uintptr_t virt_to_physM(void *vaddr) {
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


l3pp_t l3_prepare_backed(const char *backing_file_path) {
  // Setup
  l3info_t l3info = (l3info_t)malloc(sizeof(struct l3info));
  l3pp_t l3 = (l3pp_t)malloc(sizeof(struct l3pp));
  bzero(l3, sizeof(struct l3pp));
  if (l3info != NULL)
    bcopy(l3info, &l3->l3info, sizeof(struct l3info));
  fillL3Info(&l3->l3info);
  l3->level = L3;

  // Check if linearmap and quadratic map are called together
  if ((l3->l3info.flags & (L3FLAG_LINEARMAP | L3FLAG_QUADRATICMAP)) == (L3FLAG_LINEARMAP | L3FLAG_QUADRATICMAP)) {
    free(l3);
    fprintf(stderr, "Error: Cannot call linear and quadratic map together\n");
    return NULL;
  }
  printf("\n");
  mm_t mm = NULL;
  l3->mm = mm;
  
  if (l3->mm == NULL) {
    l3->mm = mm_prepare_with_BackUpFile(NULL, NULL, (lxinfo_t)l3info, backing_file_path);
    // l3->mm = mm_prepare(NULL, NULL, (lxinfo_t)&l3->l3info);
    l3->internalmm = 1;
  }
  printf("Mastik: l3->mm->memory addresses before setting backing file: PA=0x%lx VA=%p\n", virt_to_physM(l3->mm->memory), l3->mm->memory);
  // l3->mm->l3info.flags |= L3FLAG_USEPTE;  ** MAKE IT USE PTE FLAG **
  if (!mm_initialisel3(l3->mm)) 
    return NULL;
  
  printf("Mastik: l3->mm->l3buffer addresses before setting backing file: PA=0x%lx VA=%p\n", virt_to_physM(l3->mm->l3buffer), l3->mm->l3buffer);
  
  printf("\n");
  
  l3->ngroups = l3->mm->l3ngroups;
  l3->groupsize = l3->mm->l3groupsize;
  
  // Allocate monitored set info
  l3->monitoredbitmap = (uint32_t *)calloc((l3->ngroups*l3->groupsize/32) + 1, sizeof(uint32_t));
  l3->monitoredset = (int *)malloc(l3->ngroups * l3->groupsize * sizeof(int));
  l3->monitoredhead = (void **)malloc(l3->ngroups * l3->groupsize * sizeof(void *));
  l3->nmonitored = 0;
  l3->totalsets = l3->ngroups * l3->groupsize;
  free(l3info);
  return l3;
}



//dump into txt file inside physical address values l3->mm->l3buffer 
void l3_print_l3buffer_pas(l3pp_t l3) {
  //create txt file
  FILE *file = fopen("l3buffer_physical_addresses.txt", "w");
  if (file == NULL) {
      perror("Error opening file");
      return;
  }
  printf("Mastik: l3->mm->l3buffer physical address: PA=0x%lx\n", virt_to_physM(l3->mm->l3buffer));
  for (size_t offset = 0; offset < l3->l3info.bufsize; offset += 64) {
      void *current_address = (void *)((uintptr_t)l3->mm->l3buffer + offset);
      //value at current address
      uintptr_t value = *(uintptr_t *)current_address;
      uintptr_t pa = virt_to_physM(current_address);
      fprintf(file, "Offset: 0x%lx, VA: %p, PA: 0x%lx, Value: 0x%lx\n", offset, current_address, pa, value);
  }
  fclose(file);
}



//dump into txt file inside physical address values l3->mm->memory
void l3_dump_l3memory_pas(l3pp_t l3) {
  //create txt file
  FILE *file = fopen("l3memory_physical_addresses.txt", "w");
  if (file == NULL) {
      perror("Error opening file");
      return;
  }
  printf("Mastik: l3->mm->memory physical address: PA=0x%lx\n", virt_to_physM(l3->mm->memory));
  void *buf = vl_get(l3->mm->memory, 0); // Get the first buffer from the memory list
  for (size_t offset = 0; offset < l3->l3info.bufsize; offset += 64) {
   
    void *current_address = (void *)((uintptr_t)buf + offset);
      //value at current address
      uintptr_t value = *(uintptr_t *)current_address;
      uintptr_t pa = virt_to_physM(current_address);
      fprintf(file, "Offset: 0x%lx, VA: %p, PA: 0x%lx, Value: 0x%lx\n", offset, current_address, pa, value);
  }
  fclose(file);
}



// L3 Dump Function Uses l3->mm->l3groups 
void l3_dump_groups(l3pp_t l3, const char *filename) {
    if (!l3) {
        fprintf(stderr, "Error: l3 is NULL\n");
        return;
    }
    
    if (!l3->mm->l3groups) {
        fprintf(stderr, "Error: l3->groups is NULL. Probing or Loading hasn't happened.\n");
        return;
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("Error opening dump file");
        return;
    }

    // 1. Header Information
    // We try to resolve the PA of the groups array itself just for context
    uintptr_t groups_pa = virt_to_physM(l3->mm->l3groups);
    
    fprintf(fp, "groups PA=%p, VA=%p\n", (void*)groups_pa, (void*)l3->mm->l3groups);
    fprintf(fp, "ngroups=%d\n", l3->ngroups);
    fprintf(fp, "groupsize=%d\n", l3->groupsize);
    fprintf(fp, "totalsets=%zu\n", l3->totalsets);

    // 2. Iterate over Groups
    for (int i = 0; i < l3->ngroups; i++) {
        fprintf(fp, "---Group%d---\n", i);
        
        vlist_t group = l3->mm->l3groups[i];
        if (group == NULL) {
            fprintf(fp, "  (Empty/NULL)\n");
            continue;
        }

        // 3. Iterate over Lines/Sets inside the Group
        int len = vl_len(group);
        for (int j = 0; j < len; j++) {
            void *va = vl_get(group, j);
            uintptr_t pa = virt_to_physM(va);

            // Read the value stored at this address (it's a pointer/VA)
            uintptr_t value_va = *(uintptr_t *)va;
            // Convert the stored VA to PA
            uintptr_t value_pa = virt_to_physM((void *)value_va);
            // Format: setX: PA={}; VA={}
            fprintf(fp, "set%d: PA=0x%lx; VA=%p; valueInside=0x%lx\n", j, pa, va, value_pa);
        }
    }

    fclose(fp);
    printf("Dumped L3 groups to %s\n", filename);
}

void enable_PTE_flag(mm_t mm) {
    if (mm) {
        mm->l3info.flags |= L3FLAG_USEPTE;
        printf("Mastik: Enabled L3FLAG_USEPTE flag in mm structure.\n");
    }
}


/*
 * Manually installs a pre-constructed eviction set into the l3 object.
 * This bypasses the probing phase entirely.
 *
 * @param l3: The l3 object (initialized via l3_prepare_backed).
 * @param setIndex: The target cache set index (0 to totalsets-1).
 * CRITICAL: This index must match the physical slice/set mapping 
 * you expect if you are relying on specific slice targeting.
 * @param setHead: Pointer to the head of the cyclic linked list (Virtual Address).
 * Must be a valid VA in the current process context.
 * @return: 1 on success, 0 on failure.
 */
// int l3_monitor_manual(l3pp_t l3, int setIndex, void *setHead) {
 
//     if (setIndex < 0 || setIndex >= l3->totalsets)
//       return 0;
//     if (IS_MONITORED(l3->monitoredbitmap, setIndex))
//       return 0;
    


//     l3->monitoredset[l3->nmonitored] = setIndex;
//     l3->monitoredhead[l3->nmonitored++] = setHead;
//     SET_MONITORED(l3->monitoredbitmap, setIndex);

//     return 1;
// }


int l3_monitor_manual(l3pp_t l3, int setIndex, void *setHead) {
 
    if (setIndex < 0 || setIndex >= l3->totalsets)
      return 0;
    if (IS_MONITORED(l3->monitoredbitmap, setIndex))
      return 0;
    if (setHead == NULL)
      return 0;

    // Count the number of ways and collect all nodes
    int len = 0;
    void *current = setHead;
    void **nodes = NULL;
    
    // First pass: count nodes
    do {
        len++;
        current = LNEXT(current);
    } while (current != setHead);
    
    // Allocate temporary array to hold all node pointers
    nodes = (void **)malloc(len * sizeof(void *));
    if (!nodes)
        return 0;
    
    // Second pass: collect all nodes
    current = setHead;
    for (int i = 0; i < len; i++) {
        nodes[i] = current;
        current = LNEXT(current);
    }
    
    // Build doubly-linked circular list (same as lx_monitor)
    for (int way = 0; way < len; way++) {
        void *mem = nodes[way];
        void *nmem = nodes[(way + 1) % len];
        void *pmem = nodes[(way - 1 + len) % len];
        
        // Forward pointer at offset 0
        LNEXT(mem) = nmem;
        // Backward pointer at offset 8 (points to previous node's NEXTPTR location)
        LNEXT(mem + sizeof(void*)) = (pmem + sizeof(void *));
    }
    
    free(nodes);

    l3->monitoredset[l3->nmonitored] = setIndex;
    l3->monitoredhead[l3->nmonitored++] = setHead;
    SET_MONITORED(l3->monitoredbitmap, setIndex);

    return 1;
}