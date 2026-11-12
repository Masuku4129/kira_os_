#ifndef KSTACK_SLOT_H
#define KSTACK_SLOT_H
#include <stdint.h>
static inline int kstack_slot_from_base(uint64_t base, uint64_t window, uint64_t stride, uint32_t slots, uint32_t stack_size, uint32_t used_hi, uint32_t *slot_out) {
    if (!slot_out || !stride || !stack_size || stack_size > stride ||
        used_hi > slots || (slots && stride > UINT64_MAX / slots) ||
        base < window || base - window >= (uint64_t)slots * stride) return 0;
    uint64_t off = base - window, slot = off / stride;
    if (slot >= used_hi || off % stride != stride - stack_size) return 0;
    *slot_out = (uint32_t)slot;
    return 1;
}
static inline int kstack_slot_activate(uint8_t *active, uint32_t slots, uint32_t slot) {
    if (!active || slot >= slots) return 0;
    uint8_t mask = (uint8_t)(1u << (slot & 7u)), *byte = &active[slot >> 3];
    if (*byte & mask) return 0;
    *byte |= mask;
    return 1;
}
static inline int kstack_slot_release(uint8_t *active, uint32_t slots, uint32_t slot) {
    if (!active || slot >= slots) return 0;
    uint8_t mask = (uint8_t)(1u << (slot & 7u)), *byte = &active[slot >> 3];
    if (!(*byte & mask)) return 0;
    *byte &= (uint8_t)~mask;
    return 1;
}
#endif
