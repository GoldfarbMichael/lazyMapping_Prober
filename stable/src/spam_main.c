#include <stdio.h>
#include <stdint.h>
#include "murmur3.h"
#include <fcntl.h>
#include <unistd.h>

/*


To run this write in the terminal

compile:
gcc -g -O3 -Wall -Wextra -Wno-implicit-fallthrough -std=gnu99 -fno-strict-aliasing src/spam_main.c src/murmur3.c -o spam_main

execute:
./spam_main

*/


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
    // 1. Get raw cycles
    uint64_t current_cycles = __builtin_ia32_rdtscp(&aux); 

    // 2. Calculate cycles per 100 microseconds (Time = Cycles / Freq)
    uint64_t cycles_per_100us = tsc_freq_hz / 10000;
    if (cycles_per_100us == 0) return current_cycles; // Safety catch

    // 3. Clamp the time to the nearest lower 100us boundary
    uint64_t clamped_cycles = current_cycles - (current_cycles % cycles_per_100us);

    // 4. Construct the Hash Tuple (clamped_time + context_seed)
    struct ChromeHashTuple tuple;
    tuple.clamped_time = clamped_cycles;
    tuple.context_seed = context_seed;

    // 5. Hash the Tuple using the secret_seed
    uint32_t hash_out;
    // We hash the entire struct, and use secret_seed as the Murmur3 seed
    MurmurHash3_x86_32(&tuple, sizeof(tuple), secret_seed, &hash_out);

    // // 6. Calculate a uniform midpoint between the current clamp and the next clamp
    // uint64_t jittered_cycles = clamped_cycles + (hash_out % cycles_per_100us);
    // uint64_t midpoint = clamped_cycles + (cycles_per_100us/2);
    // // 7. Temporal Discrimination (Triangle Distribution logic)
    // uint64_t result_cycles;
    // if (jittered_cycles < midpoint) {
    //     result_cycles = clamped_cycles;
    // } else {
    //     result_cycles = clamped_cycles + cycles_per_100us;
    // }

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

#define RESULT_SIZE 100000

int main() {
    printf("===== CHROME MOCK TIMER TEST SUITE =====\n\n");
    setup_browser_environment();
    //print tsc freq
    printf("Calibrated TSC Frequency: %lu Hz\n", g_tsc_freq_hz);
    //init array size RESULT_SIZE
    uint64_t results[RESULT_SIZE];
    uint64_t counts[RESULT_SIZE];

    for(int i=0; i<RESULT_SIZE; i++){
        uint64_t start = wait_edge(g_tsc_freq_hz, g_context_seed, g_secret_seed);
        counts[i]=count_edge(g_tsc_freq_hz, g_context_seed, g_secret_seed);
    }

    for(int i=0; i<RESULT_SIZE; i++){
        uint32_t aux;
        uint64_t start = __builtin_ia32_rdtscp(&aux); 
        wait_edge(g_tsc_freq_hz, g_context_seed, g_secret_seed);
        uint64_t end = __builtin_ia32_rdtscp(&aux);
        results[i] = ((end - start) * 1000000000) / g_tsc_freq_hz;
    }

    //write array into csv file
    FILE *fp = fopen("chrome_timer_results.csv", "w");
    if(fp == NULL){
        fprintf(stderr, "Error opening file for writing.\n");
        return 1;
    }
    fprintf(fp, "EdgeIndex,times ,Count\n");
    for(int i=0; i<RESULT_SIZE; i++){
        fprintf(fp, "%d,%lu,%lu\n", i, results[i],counts[i]);
    }
    fclose(fp);
    printf("Test completed. Results written to chrome_timer_results.csv\n");



    return 0;
}