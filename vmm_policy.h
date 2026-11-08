#ifndef KIRA_VMM_POLICY_H
#define KIRA_VMM_POLICY_H

#include <stdint.h>

/* Pure policy checks shared by the kernel and host regression tests.
   These helpers do not emulate page-table walking or physical allocation. */
static inline int kira_vmm_x86_entry_allows_user(uint64_t entry, int write) {
    const uint64_t present = 1ull << 0;
    const uint64_t writable = 1ull << 1;
    const uint64_t user = 1ull << 2;
    if ((entry & (present | user)) != (present | user)) return 0;
    if (write && !(entry & writable)) return 0;
    return 1;
}

/* Validate and page-round a physical MMIO span. addr_mask contains the
   address bits (page-aligned); the extra page_size represents the final
   byte offset. Outputs are untouched on failure. */
static inline int kira_vmm_mmio_span(uint64_t pa, uint64_t len,
                                     uint64_t addr_mask, uint64_t page_size,
                                     uint64_t* first_out, uint64_t* last_out) {
    if (!len || !page_size || (page_size & (page_size - 1)) ||
        !first_out || !last_out) return 0;
    if (addr_mask > UINT64_MAX - page_size) return 0;
    const uint64_t limit = addr_mask + page_size;
    if (pa >= limit || len > limit - pa) return 0;
    const uint64_t end = pa + len;
    const uint64_t mask = ~(page_size - 1);
    const uint64_t first = pa & mask;
    const uint64_t last = ((end - 1) & mask) + page_size;
    if (last < end || last <= first) return 0;
    *first_out = first;
    *last_out = last;
    return 1;
}

/* Validate the exact base address returned for a kernel-stack slot.
   'allocated' must describe the slot's current allocation state. */
static inline int kira_vmm_kstack_slot_valid(uint64_t base, uint64_t stride,
                                              uint32_t slots, uint32_t size,
                                              uint64_t va, uint32_t used_hi,
                                              int allocated, uint32_t* slot_out) {
    if (!stride || !size || size > stride || !slot_out ||
        !slots) return 0;
    const uint64_t span = (uint64_t)slots * stride;
    if (base > UINT64_MAX - span || va < base || va >= base + span) return 0;
    const uint64_t rel = va - base;
    const uint32_t slot = (uint32_t)(rel / stride);
    if (slot >= used_hi || slot >= slots || !allocated) return 0;
    const uint64_t expected = base + ((uint64_t)slot + 1) * stride - size;
    if (va != expected) return 0;
    *slot_out = slot;
    return 1;
}

#endif
