#include <stdio.h>
#include <stdint.h>
#include "vmm_policy.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\\n", __LINE__, #x); fails++; } } while (0)

int main(void) {
    const uint64_t P = 1ull, W = 2ull, U = 4ull;

    puts("VMM policy: effective x86 user permissions");
    CHECK(!kira_vmm_x86_entry_allows_user(0, 0));
    CHECK(!kira_vmm_x86_entry_allows_user(P, 0));          /* supervisor */
    CHECK(!kira_vmm_x86_entry_allows_user(P | U, 1));      /* read-only */
    CHECK(kira_vmm_x86_entry_allows_user(P | U, 0));
    CHECK(kira_vmm_x86_entry_allows_user(P | U | W, 1));
    CHECK(!kira_vmm_x86_entry_allows_user(P | W, 0));      /* USER missing */
    /* The caller must apply this test to every paging level. */
    CHECK(kira_vmm_x86_entry_allows_user(P | U | W, 1) &&
          !kira_vmm_x86_entry_allows_user(P | U, 1));

    puts("VMM policy: MMIO range overflow and page rounding");
    uint64_t first = 0xAA, last = 0xBB;
    const uint64_t mask = 0x000FFFFFFFFFF000ull;
    CHECK(kira_vmm_mmio_span(0x1234, 1, mask, 4096, &first, &last));
    CHECK(first == 0x1000 && last == 0x2000);
    CHECK(kira_vmm_mmio_span(0x1FFF, 2, mask, 4096, &first, &last));
    CHECK(first == 0x1000 && last == 0x3000);
    CHECK(!kira_vmm_mmio_span(0, 0, mask, 4096, &first, &last));
    CHECK(!kira_vmm_mmio_span(mask + 4096, 1, mask, 4096, &first, &last));
    CHECK(!kira_vmm_mmio_span(mask, 2, mask, 4096, &first, &last));
    CHECK(!kira_vmm_mmio_span(UINT64_MAX - 2, 8, UINT64_MAX - 4095, 4096, &first, &last));
    CHECK(first != 0xAA || last != 0xBB); /* valid call wrote outputs */

    puts("VMM policy: exact kernel-stack slot validation");
    const uint64_t base = 0x10000000, stride = 65536;
    const uint32_t size = 16384;
    uint32_t slot = 99;
    const uint64_t stack0 = base + stride - size;
    CHECK(kira_vmm_kstack_slot_valid(base, stride, 8, size, stack0, 1, 1, &slot));
    CHECK(slot == 0);
    CHECK(kira_vmm_kstack_slot_valid(base, stride, 8, size, stack0 + stride, 2, 1, &slot));\n    CHECK(slot == 1);\n    CHECK(!kira_vmm_kstack_slot_valid(base, stride, 8, size, stack0 + 2 * stride, 2, 1, &slot));
    CHECK(!kira_vmm_kstack_slot_valid(base, stride, 8, size, stack0 + 8, 1, 1, &slot));
    CHECK(!kira_vmm_kstack_slot_valid(base, stride, 8, size, stack0, 1, 0, &slot)); /* double free */
    CHECK(!kira_vmm_kstack_slot_valid(base, stride, 8, size, base + 8 * stride, 8, 1, &slot));
    CHECK(!kira_vmm_kstack_slot_valid(base, stride, 8, 0, stack0, 1, 1, &slot));

    printf("%s\\n", fails ? "FAILED" : "VMM policy regression tests passed");
    return fails != 0;
}
