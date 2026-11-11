#ifndef KIRA_AARCH64_RANGE_H
#define KIRA_AARCH64_RANGE_H
#include <stdint.h>

/* Half-open physical ranges. Invalid, empty or overflowing ranges never match. */
static inline int arm_range_end(uint64_t base, uint64_t length, uint64_t* end) {
    if (!end || !length || base > UINT64_MAX - length) return 0;
    *end = base + length;
    return 1;
}
static inline int arm_range_contains(uint64_t base, uint64_t size,
                                     uint64_t address, uint64_t length) {
    uint64_t limit, end;
    return arm_range_end(base, size, &limit) &&
           arm_range_end(address, length, &end) &&
           address >= base && end <= limit;
}
static inline int arm_range_overlaps(uint64_t base, uint64_t size,
                                     uint64_t address, uint64_t length) {
    uint64_t limit, end;
    return arm_range_end(base, size, &limit) &&
           arm_range_end(address, length, &end) &&
           address < limit && base < end;
}
#endif
