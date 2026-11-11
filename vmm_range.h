#ifndef KIRA_VMM_RANGE_H
#define KIRA_VMM_RANGE_H

#include <stdint.h>

/* Convert a byte interval to its enclosing page interval without wraparound. */
static inline int vmm_mmio_page_range(uint64_t pa, uint64_t len, uint64_t page_size,
                                      uint64_t* first, uint64_t* size) {
    if (!first || !size || !len || !page_size || (page_size & (page_size - 1)) ||
        pa > UINT64_MAX - len) return 0;
    uint64_t end = pa + len;
    uint64_t mask = page_size - 1;
    if (end > UINT64_MAX - mask) return 0;
    uint64_t lo = pa & ~mask;
    uint64_t hi = (end + mask) & ~mask;
    if (hi <= lo) return 0;
    *first = lo;
    *size = hi - lo;
    return 1;
}

#endif
