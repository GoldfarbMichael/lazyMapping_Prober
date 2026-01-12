#ifndef TESTS_H
#define TESTS_H
#include <stdint.h>
#include <l3.h>


void dump_eSets_to_txt(void **e_sets, int numOfSets, const char *filename);

int test_save_and_load_physical_mapping(l3pp_t *l3, void **e_sets, const char *filename);

int test_mapping_BIN_reconstruction_to_eSets(const char *mapping_file, const char *hugePage_file,const char *dump_file);

#endif