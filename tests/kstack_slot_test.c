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
    uint64_t generation[SLOTS] = {0};
    uint32_t slot = UINT32_MAX;
    uint64_t gen_a = 0, gen_b = 0;
    uint64_t valid = BASE + STRIDE - SIZE;

    assert(kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(slot == 0);
    assert(!kstack_slot_from_base(valid + 1, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(valid + 4096, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(BASE + STRIDE, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(BASE - 1, BASE, STRIDE, SLOTS, SIZE, 1, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, 0, 1, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 0, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, SLOTS + 1u, &slot));
    assert(!kstack_slot_from_base(valid, BASE, STRIDE, SLOTS, SIZE, 1, 0));
    assert(!kstack_slot_from_base(0, 0, UINT64_MAX, 2, 1, 1, &slot));

    assert(!kstack_slot_can_reuse(0, 0, SLOTS));
    assert(kstack_slot_can_reuse(0, 1, SLOTS));
    assert(kstack_slot_can_reuse(SLOTS - 1u, SLOTS, SLOTS));
    assert(!kstack_slot_can_reuse(SLOTS, SLOTS, SLOTS));
    assert(!kstack_slot_can_reuse(1, 1, SLOTS));

    assert(kstack_slot_activate(active, generation, SLOTS, 0, &gen_a));
    assert(gen_a == 1);
    assert(!kstack_slot_activate(active, generation, SLOTS, 0, &gen_b));
    assert(!kstack_slot_release(active, generation, SLOTS, 1, gen_a));
    assert(kstack_slot_release(active, generation, SLOTS, 0, gen_a));
    assert(!kstack_slot_release(active, generation, SLOTS, 0, gen_a));
    assert(kstack_slot_activate(active, generation, SLOTS, 0, &gen_b));
    assert(gen_b == gen_a + 1);
    /* A delayed duplicate free from the old owner must not free the reused slot. */
    assert(!kstack_slot_release(active, generation, SLOTS, 0, gen_a));
    assert(kstack_slot_release(active, generation, SLOTS, 0, gen_b));
    assert(!kstack_slot_activate(active, generation, SLOTS, SLOTS, &gen_a));
    assert(!kstack_slot_release(active, generation, SLOTS, SLOTS, gen_b));
    assert(kstack_slot_activate(active, generation, SLOTS, SLOTS - 1u, &gen_a));
    assert(kstack_slot_release(active, generation, SLOTS, SLOTS - 1u, gen_a));

    /* The free-list transaction rejects duplicate, stale-generation, corrupt and over-capacity entries. */
    uint16_t free_slots[SLOTS];
    uint32_t free_count = 0, reused = UINT32_MAX;
    uint64_t gen_first = 0, gen_reused = 0;
    assert(kstack_slot_activate(active, generation, SLOTS, 7, &gen_first));
    assert(kstack_slot_free_push(free_slots, &free_count, active, generation,
                                 SLOTS, SLOTS, 7, gen_first));
    assert(free_count == 1);
    assert(!kstack_slot_free_push(free_slots, &free_count, active, generation,
                                  SLOTS, SLOTS, 7, gen_first));
    assert(kstack_slot_free_pop(free_slots, &free_count, active, generation,
                                SLOTS, SLOTS, &reused, &gen_reused));
    assert(reused == 7 && free_count == 0 && gen_reused != gen_first);
    assert(!kstack_slot_free_push(free_slots, &free_count, active, generation,
                                  SLOTS, SLOTS, 7, gen_first));
    assert(kstack_slot_free_push(free_slots, &free_count, active, generation,
                                 SLOTS, SLOTS, 7, gen_reused));

    /* A duplicate corrupt entry must not reactivate or consume the bad entry. */
    free_slots[free_count++] = 7;
    uint64_t gen_third = 0;
    assert(kstack_slot_free_pop(free_slots, &free_count, active, generation,
                                SLOTS, SLOTS, &reused, &gen_third));
    assert(reused == 7 && free_count == 1);
    assert(!kstack_slot_free_pop(free_slots, &free_count, active, generation,
                                 SLOTS, SLOTS, &reused, &gen_a));
    assert(free_count == 1);
    assert(kstack_slot_release(active, generation, SLOTS, 7, gen_third));

    free_slots[0] = (uint16_t)SLOTS;
    free_count = 1;
    assert(!kstack_slot_free_pop(free_slots, &free_count, active, generation,
                                 SLOTS, SLOTS, &reused, &gen_a));
    assert(free_count == 1);
    free_count = 0;

    uint32_t full_count = SLOTS;
    assert(kstack_slot_activate(active, generation, SLOTS, 8, &gen_a));
    assert(!kstack_slot_free_push(free_slots, &full_count, active, generation,
                                  SLOTS, SLOTS, 8, gen_a));
    assert(kstack_slot_release(active, generation, SLOTS, 8, gen_a));

    /* ARM64 uses 65,536 slots; verify the final uint16_t slot index and generation. */
    uint8_t arm_active[(ARM_SLOTS + 7u) / 8u] = {0};
    uint64_t arm_generation[ARM_SLOTS] = {0};
    uint64_t arm_last = BASE + (uint64_t)(ARM_SLOTS - 1u) * STRIDE + STRIDE - SIZE;
    uint64_t arm_gen = 0;
    assert(kstack_slot_from_base(arm_last, BASE, STRIDE, ARM_SLOTS, SIZE,
                                 ARM_SLOTS, &slot));
    assert(slot == ARM_SLOTS - 1u);
    assert(kstack_slot_activate(arm_active, arm_generation, ARM_SLOTS, slot, &arm_gen));
    assert(kstack_slot_release(arm_active, arm_generation, ARM_SLOTS, slot, arm_gen));

    uint16_t arm_free[ARM_SLOTS];
    uint32_t arm_free_count = 0, arm_reused = 0;
    uint64_t arm_gen2 = 0;
    assert(kstack_slot_activate(arm_active, arm_generation, ARM_SLOTS,
                                ARM_SLOTS - 1u, &arm_gen));
    assert(kstack_slot_free_push(arm_free, &arm_free_count, arm_active,
                                 arm_generation, ARM_SLOTS, ARM_SLOTS,
                                 ARM_SLOTS - 1u, arm_gen));
    assert(kstack_slot_free_pop(arm_free, &arm_free_count, arm_active,
                                arm_generation, ARM_SLOTS, ARM_SLOTS,
                                &arm_reused, &arm_gen2));
    assert(arm_reused == ARM_SLOTS - 1u && arm_free_count == 0);
    assert(arm_gen2 != arm_gen);
    assert(!kstack_slot_free_push(arm_free, &arm_free_count, arm_active,
                                  arm_generation, ARM_SLOTS + 1u, ARM_SLOTS,
                                  ARM_SLOTS - 1u, arm_gen2));

    puts("kstack slot lifecycle: exact base, bounds, ownership, ABA generation, double-free and reuse passed");
    return 0;
}
