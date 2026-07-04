#ifndef MASTIK_ELITE_H
#define MASTIK_ELITE_H
#define _GNU_SOURCE
#include <sched.h>
#include "mastik/l3.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include "lazy_map.h"   // LazyMap (JS-style lazy-map victim, timer_mode==2)
#define MAX_NUM_CLUSTERS 64
#define TST_SEC 2
#define SST_SEC 8000e-6
#define DEFAULT_SAMPLING_PATH "memoryGram_output.csv"

#define MAX_ARGS 10 //arguments for stressor config


typedef struct {
    void **clusterHeads;        // Array of pointers to circular linked list heads (one per cluster)
    int counts[MAX_NUM_CLUSTERS];   // Number of eviction sets in each cluster
    int Clustersize;        // Expected number of sets per cluster
} Clusters_t;

typedef struct {
    char *stressor_name;
    char *exec_args[MAX_ARGS];
} StressorConfig;


extern StressorConfig stress_battery[];
#define NUM_STRESSORS (sizeof(stress_battery) / sizeof(StressorConfig))
#define SAMPLES_PER_STRESSOR 5 //default samples for each batch for each stressor

// Runtime accessor for the battery size. NUM_STRESSORS uses sizeof(stress_battery), which
// only works in the translation unit that DEFINES the array (mastikElite.c); other units
// see an incomplete extern type. Call this from elsewhere (e.g. fingerprint_orchestrator).
size_t stress_battery_count(void);

// Function declarations
void pin_to_core(int core_id);
void cleanup_handler(int sig);



int newBackedMapping_and_saveEsets_toBinFile(l3pp_t *l3, const char *backing_file, const char *BIN_file);

int create2_newBackedMappings_and_syncSetIndexes(l3pp_t *l3,l3pp_t *l3B, const char *backing_fileA, const char *backing_fileB, const char *BIN_fileA, const char *BIN_fileB);

int load_mapping_and_eSetsFrom_BIN_file(l3pp_t *l3, void ***e_sets, const char *backing_file, const char *BIN_file);

Clusters_t* eviction_sets_to_Clusters(void ***e_sets, int num_sets, int NoC);

void get_spatioTemporal_memoryGram(Clusters_t *Clusters, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename);

void get_spatioTemporal_memoryGram_ChromeMock_jsmap(LazyMap *m, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename);

int runStressNG_batches(double tst_sec, int batch_size, int start_iteration, char *output_dir,const char *backing_file, const char *BIN_file, int timer_mode);



void free_Clusters(Clusters_t *Clusters);
#endif // MASTIK_ELITE_H