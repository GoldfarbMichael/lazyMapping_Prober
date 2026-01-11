#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

// Helper to get Physical Address from Virtual Address
uintptr_t virt_to_phys(void *vaddr) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;

    uintptr_t virt_pfn = (uintptr_t)vaddr / 4096;
    off_t offset = virt_pfn * sizeof(uint64_t);
    uint64_t page;
    
    if (pread(fd, &page, sizeof(uint64_t), offset) != sizeof(uint64_t)) {
        close(fd);
        return 0;
    }
    close(fd);

    // Check if present
    if ((page & (1ULL << 63)) == 0) return 0;

    // PFN is bits 0-54
    uintptr_t phys_pfn = page & 0x7FFFFFFFFFFFFFULL;
    return (phys_pfn * 4096) + ((uintptr_t)vaddr % 4096);
}