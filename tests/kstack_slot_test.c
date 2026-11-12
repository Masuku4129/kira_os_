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
    assert(!kstack_slot_can_reuse(0, 0, SLOTS));
    assert(kstack_slot_can_reuse(0, 1, SLOTS));
    assert(kstack_slot_can_reuse(SLOTS - 1, SLOTS, SLOTS));
    assert(!kstack_slot_can_reuse(SLOTS, SLOTS, SLOTS));
    assert(!kstack_slot_can_reuse(1, 1, SLOTS));
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

    /* Exercise the checked free-list transaction, including duplicate frees,
       corrupt entries and full-list refusal without losing stack ownership. */
    uint16_t free_slots[SLOTS];
    uint32_t free_count = 0, reused = UINT32_MAX;
    assert(kstack_slot_activate(active, SLOTS, 7));
    assert(kstack_slot_free_push(free_slots, &free_count, active, SLOTS, SLOTS, 7));
    assert(free_count == 1 && !kstack_slot_free_push(free_slots, &free_count, active, SLOTS, SLOTS, 7));
    assert(kstack_slot_free_pop(free_slots, &free_count, active, SLOTS, SLOTS, &reused));
    assert(reused == 7 && free_count == 0);
    assert(!kstack_slot_free_pop(free_slots, &free_count, active, SLOTS, SLOTS, &reused));
    assert(!kstack_slot_free_push(free_slots, &free_count, active, SLOTS, 7, 7));
    assert(kstack_slot_free_push(free_slots, &free_count, active, SLOTS, SLOTS, 7));
    /* Corrupt duplicate entry must not activate twice or consume the bad entry. */
    free_slots[free_count] = 7; free_count++;
    assert(kstack_slot_free_pop(free_slots, &free_count, active, SLOTS, SLOTS, &reused));
    assert(reused == 7 && free_count == 1);
    assert(!kstack_slot_free_pop(free_slots, &free_count, active, SLOTS, SLOTS, &reused));
    assert(free_count == 1);
    assert(kstack_slot_release(active, SLOTS, 7));

    free_slots[0] = (uint16_t)SLOTS;
    free_count = 1;
    assert(!kstack_slot_free_pop(free_slots, &free_count, active, SLOTS, SLOTS, &reused));
    assert(free_count == 1); /* Out-of-range entry is not consumed. */
    free_count = 0;

    uint32_t full_count = SLOTS;
    assert(kstack_slot_activate(active, SLOTS, 8));
    assert(!kstack_slot_free_push(free_slots, &full_count, active, SLOTS, SLOTS, 8));
    assert(kstack_slot_release(active, SLOTS, 8));


    /* ARM64 reserves 65,536 slots; exercise its wider slot index and bitmap edge. */
    uint8_t arm_active[(ARM_SLOTS + 7u) / 8u] = {0};
    uint64_t arm_last = BASE + (uint64_t)(ARM_SLOTS - 1u) * STRIDE + STRIDE - SIZE;
    assert(kstack_slot_from_base(arm_last, BASE, STRIDE, ARM_SLOTS, SIZE, ARM_SLOTS, &slot));
    assert(slot == ARM_SLOTS - 1u);
    assert(kstack_slot_activate(arm_active, ARM_SLOTS, slot));
    assert(kstack_slot_release(arm_active, ARM_SLOTS, slot));
    uint16_t arm_free[ARM_SLOTS];
    uint32_t arm_free_count = 0, arm_reused = 0;
    assert(kstack_slot_activate(arm_active, ARM_SLOTS, ARM_SLOTS - 1u));
    assert(kstack_slot_free_push(arm_free, &arm_free_count, arm_active,
                                 ARM_SLOTS, ARM_SLOTS, ARM_SLOTS - 1u));
    assert(kstack_slot_free_pop(arm_free, &arm_free_count, arm_active,
                                ARM_SLOTS, ARM_SLOTS, &arm_reused));
    assert(arm_reused == ARM_SLOTS - 1u && arm_free_count == 0);
    assert(!kstack_slot_free_push(arm_free, &arm_free_count, arm_active,
                                  ARM_SLOTS + 1u, ARM_SLOTS, ARM_SLOTS - 1u));
    puts("kstack slot lifecycle: exact base, bounds, ownership, double-free and reuse passed");
    return 0;
}
