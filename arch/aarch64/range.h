#ifndef KIRA_AARCH64_RANGE_H
#define KIRA_AARCH64_RANGE_H
#include <stdint.h>

typedef struct { uint64_t base, size; } arm_range_t;

/* Half-open physical ranges. Invalid, empty or overflowing ranges never match. */
static inline int arm_range_end(uint64_t base, uint64_t length, uint64_t* end) {
    if (!end || !length || base > UINT64_MAX - length) return 0;
    *end = base + length;
    return 1;
}
/* Clip a validated half-open RAM range to the CPU's implemented physical-address limit. */
static inline int arm_range_clip(uint64_t base, uint64_t size, uint64_t limit, uint64_t* clipped_size) {
    if (!clipped_size || !size || !limit || base >= limit || base > UINT64_MAX - size) return 0;
    uint64_t end = base + size;
    if (end > limit) end = limit;
    if (end <= base) return 0;
    *clipped_size = end - base;
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
static inline int arm_ranges_contains(const arm_range_t* ranges, unsigned count,
                                      uint64_t address, uint64_t length) {
    if (!ranges) return 0;
    for (unsigned i = 0; i < count; i++)
        if (arm_range_contains(ranges[i].base, ranges[i].size, address, length)) return 1;
    return 0;
}
static inline int arm_ranges_overlaps(const arm_range_t* ranges, unsigned count,
                                      uint64_t address, uint64_t length) {
    if (!ranges) return 0;
    for (unsigned i = 0; i < count; i++)
        if (arm_range_overlaps(ranges[i].base, ranges[i].size, address, length)) return 1;
    return 0;
}
#endif
