#ifndef _MURMURHASH3_H_
#define _MURMURHASH3_H_

#include <stdint.h>

// We strictly require the 32-bit version to mimic Chrome's V8 engine jitter.
void MurmurHash3_x86_32(const void * key, int len, uint32_t seed, void * out);

#endif // _MURMURHASH3_H_