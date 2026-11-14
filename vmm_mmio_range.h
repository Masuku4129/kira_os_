#ifndef KIRA_VMM_MMIO_RANGE_H
#define KIRA_VMM_MMIO_RANGE_H

#include <stdint.h>

#define VMM_MMIO_PAGE_SIZE 4096ULL
#define VMM_MMIO_PAGE_MASK (VMM_MMIO_PAGE_SIZE - 1ULL)

/* Return a checked page-aligned half-open physical range [first, end).
   Reject arithmetic overflow instead of allowing align-up to wrap to zero. */
static inline int vmm_mmio_page_range(uint64_t pa, uint64_t len,
                                      uint64_t* first, uint64_t* end) {
    if (!len || !first || !end) return 0;
    if (len > UINT64_MAX - pa) return 0;
    uint64_t raw_end = pa + len;
    if (raw_end > UINT64_MAX - VMM_MMIO_PAGE_MASK) return 0;
    uint64_t aligned_end = (raw_end + VMM_MMIO_PAGE_MASK) & ~VMM_MMIO_PAGE_MASK;
    uint64_t aligned_first = pa & ~VMM_MMIO_PAGE_MASK;
    if (aligned_end <= aligned_first) return 0;
    *first = aligned_first;
    *end = aligned_end;
    return 1;
}

#endif
