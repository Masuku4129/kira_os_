#ifndef KSTACK_SLOT_H
#define KSTACK_SLOT_H

#include <stdint.h>

/* Shared, host-testable ownership rules for x86-64 and ARM64 kernel-stack slots. */
static inline int kstack_slot_from_base(uint64_t base, uint64_t window,
                                        uint64_t stride, uint32_t slots,
                                        uint32_t stack_size, uint32_t used_hi,
                                        uint32_t *slot_out) {
    if (!slot_out || !stride || !stack_size || stack_size > stride ||
        used_hi > slots || (slots && stride > UINT64_MAX / slots) ||
        base < window || base - window >= (uint64_t)slots * stride)
        return 0;

    uint64_t off = base - window;
    uint64_t slot = off / stride;
    if (slot >= used_hi || off % stride != stride - stack_size)
        return 0;

    *slot_out = (uint32_t)slot;
    return 1;
}

static inline int kstack_slot_can_reuse(uint32_t slot, uint32_t used_hi,
                                        uint32_t slots) {
    return slot < used_hi && slot < slots;
}

static inline int kstack_slot_activate(uint8_t *active, uint64_t *generation,
                                       uint32_t slots, uint32_t slot,
                                       uint64_t *generation_out) {
    if (!active || !generation || !generation_out || slot >= slots) return 0;
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    uint8_t *byte = &active[slot >> 3];
    if (*byte & mask) return 0;
    *byte |= mask;
    uint64_t next = generation[slot] + 1u;
    if (!next) next = 1u;
    generation[slot] = next;
    *generation_out = next;
    return 1;
}

static inline int kstack_slot_release(uint8_t *active, uint64_t *generation,
                                      uint32_t slots, uint32_t slot,
                                      uint64_t expected_generation) {
    if (!active || !generation || !expected_generation || slot >= slots) return 0;
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    uint8_t *byte = &active[slot >> 3];
    if (!(*byte & mask) || generation[slot] != expected_generation) return 0;
    *byte &= (uint8_t)~mask;
    return 1;
}

static inline int kstack_slot_free_push(uint16_t *free_slots,
                                        uint32_t *free_count,
                                        uint8_t *active, uint64_t *generation,
                                        uint32_t slots, uint32_t used_hi,
                                        uint32_t slot, uint64_t expected_generation) {
    if (!free_slots || !free_count || !active || !generation ||
        slots > (uint32_t)UINT16_MAX + 1u || *free_count >= slots ||
        !kstack_slot_can_reuse(slot, used_hi, slots))
        return 0;

    if (!kstack_slot_release(active, generation, slots, slot, expected_generation))
        return 0;
    free_slots[(*free_count)++] = (uint16_t)slot;
    return 1;
}

static inline int kstack_slot_free_pop(uint16_t *free_slots,
                                       uint32_t *free_count,
                                       uint8_t *active, uint64_t *generation,
                                       uint32_t slots, uint32_t used_hi,
                                       uint32_t *slot_out, uint64_t *generation_out) {
    if (!free_slots || !free_count || !active || !generation || !slot_out ||
        !generation_out || slots > (uint32_t)UINT16_MAX + 1u || !*free_count ||
        *free_count > slots)
        return 0;

    uint32_t slot = free_slots[*free_count - 1u];
    if (!kstack_slot_can_reuse(slot, used_hi, slots) ||
        !kstack_slot_activate(active, generation, slots, slot, generation_out))
        return 0;

    --*free_count;
    *slot_out = slot;
    return 1;
}

#endif
