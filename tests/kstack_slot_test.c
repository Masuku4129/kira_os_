#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "kstack_slot.h"
#define BASE 0xFFFFFF4000000000ULL
#define STRIDE (64ULL * 1024ULL)
#define SLOTS 8192u
#define SIZE (16u * 1024u)
#define ARM_SLOTS 65536u
int main(void) {
    uint8_t active[(SLOTS + 7u) / 8u] = {0};
    uint32_t slot = 999;
    uint64_t valid = BASE + STRIDE - SIZE;
    assert(kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 1, &slot) && slot == 0);
    assert(!kstack_slot_from_base(valid + 1, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(valid + 4096, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(BASE + STRIDE, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(BASE - 1, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 0, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, 0, 1, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 1, 0));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, SLOTS + 1u, &slot));
    assert(!kstack_slot_from_base(0, 0, UINT64_MAX, 2, 1, 1, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 1, &slot) == 0);
    assert(kstack_slot_activate(active, SLOTS, 0));
    assert(!kstack_slot_activate(active, SLOTS, 0));
    assert(!kstack_slot_release(active, SLOTS, 1));
    assert(kstack_slot_release(active, SLOTS, 0));
    assert(!kstack_slot_release(active, SLOTS, 0));
    assert(kstack_slot_activate(active, SLOTS, 0));
    assert(!kstack_slot_activate(active, SLOTS, SLOTS));
    assert(!kstack_slot_release(active, SLOTS, SLOTS));
    assert(kstack_slot_activate(active, SLOTS, SLOTS - 1));
    assert(kstack_slot_release(active, SLOTS, SLOTS - 1));

    /* ARM64 reserves 65,536 slots; exercise its wider slot index and bitmap edge. */
    uint8_t arm_active[(ARM_SLOTS + 7u) / 8u] = {0};
    uint64_t arm_last = BASE + (uint64_t)(ARM_SLOTS - 1u) * STRIDE + STRIDE - SIZE;
    assert(kstack_slot_from_base(arm_last, BASE, STRIDE, ARM_SLOTS, SIZE, ARM_SLOTS, &slot));
    assert(slot == ARM_SLOTS - 1u);
    assert(kstack_slot_activate(arm_active, ARM_SLOTS, slot));
    assert(kstack_slot_release(arm_active, ARM_SLOTS, slot));
    puts("kstack slot lifecycle: exact base, bounds, ownership, double-free and reuse passed");
    return 0;
}
