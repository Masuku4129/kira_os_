#ifndef KIRA_KSTACK_SLOTS_H
#define KIRA_KSTACK_SLOTS_H

#include <stdint.h>

#ifndef KSTACK_SLOT_COUNT
#define KSTACK_SLOT_COUNT 8192u
#endif
#ifndef KSTACK_SLOT_STRIDE
#define KSTACK_SLOT_STRIDE (64u * 1024u)
#endif

typedef struct {
    uint16_t free_slots[KSTACK_SLOT_COUNT];
    uint32_t free_count;
    uint32_t used_hi;
    uint8_t active[(KSTACK_SLOT_COUNT + 7u) / 8u];
} kstack_slots_t;

static inline int kstack_slots_take(kstack_slots_t *s, uint32_t *slot, int *is_new) {
    if (!s || !slot || !is_new) return -1;
    if (s->free_count) {
        *slot = s->free_slots[--s->free_count];
        *is_new = 0;
    } else {
        if (s->used_hi >= KSTACK_SLOT_COUNT) return -1;
        *slot = s->used_hi++;
        *is_new = 1;
    }
    s->active[*slot >> 3] |= (uint8_t)(1u << (*slot & 7u));
    return 0;
}

static inline void kstack_slots_quarantine(kstack_slots_t *s, uint32_t slot) {
    if (!s || slot >= s->used_hi) return;
    s->active[slot >> 3] &= (uint8_t)~(1u << (slot & 7u));
}

static inline int kstack_slots_release(kstack_slots_t *s, uint32_t slot) {
    if (!s || slot >= s->used_hi || s->free_count >= KSTACK_SLOT_COUNT) return -1;
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    if (!(s->active[slot >> 3] & mask)) return -1;
    s->active[slot >> 3] &= (uint8_t)~mask;
    s->free_slots[s->free_count++] = (uint16_t)slot;
    return 0;
}

static inline int kstack_slots_decode_base(uint64_t base, uint64_t region,
                                            uint32_t bytes, uint32_t *slot) {
    if (!slot || !bytes || bytes > KSTACK_SLOT_STRIDE || base < region) return -1;
    uint64_t off = base - region;
    uint64_t index = off / KSTACK_SLOT_STRIDE;
    if (index >= KSTACK_SLOT_COUNT) return -1;
    if (off % KSTACK_SLOT_STRIDE != KSTACK_SLOT_STRIDE - bytes) return -1;
    *slot = (uint32_t)index;
    return 0;
}
#endif
