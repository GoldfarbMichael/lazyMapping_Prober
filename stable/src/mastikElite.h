#ifndef MASTIK_ELITE_H
#define MASTIK_ELITE_H

#include "mastik/l3.h"

#define MAX_NUM_CLUSTERS 128
#define TST_SEC 32
#define SST_SEC 500e-6
#define DEFAULT_SAMPLING_PATH "memoryGram_output.csv"

typedef struct {
    void **clusterHeads;        // Array of pointers to circular linked list heads (one per cluster)
    int counts[MAX_NUM_CLUSTERS];   // Number of eviction sets in each cluster
    int Clustersize;        // Expected number of sets per cluster
} Clusters_t;

int newBackedMapping_and_saveEsets_toBinFile(l3pp_t *l3, const char *backing_file, const char *BIN_file);

int create2_newBackedMappings_and_syncSetIndexes(l3pp_t *l3,l3pp_t *l3B, const char *backing_fileA, const char *backing_fileB, const char *BIN_fileA, const char *BIN_fileB);

int load_mapping_and_eSetsFrom_BIN_file(l3pp_t *l3, void ***e_sets, const char *backing_file, const char *BIN_file);

Clusters_t* eviction_sets_to_Clusters(void ***e_sets, int num_sets, int NoC);

void get_spatioTemporal_memoryGram(Clusters_t *Clusters, int NoC, uint64_t TST_cycles, uint64_t SST_cycles, uint32_t *matrix, const char* filename);



void free_Clusters(Clusters_t *Clusters);
#endif // MASTIK_ELITE_H