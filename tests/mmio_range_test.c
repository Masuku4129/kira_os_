#include <stdio.h>
#include <stdint.h>
#include "vmm_mmio_range.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fails++; } } while (0)

int main(void) {
    uint64_t first = 0, end = 0;
    CHECK(!vmm_mmio_page_range(0, 0, &first, &end));
    CHECK(!vmm_mmio_page_range(0, 1, 0, &end));
    CHECK(!vmm_mmio_page_range(0, 1, &first, 0));

    CHECK(vmm_mmio_page_range(0x1003, 1, &first, &end));
    CHECK(first == 0x1000 && end == 0x2000);
    CHECK(vmm_mmio_page_range(0x1fff, 2, &first, &end));
    CHECK(first == 0x1000 && end == 0x3000);
    CHECK(vmm_mmio_page_range(0x2000, 0x2000, &first, &end));
    CHECK(first == 0x2000 && end == 0x4000);

    /* Both the raw addition and the subsequent page align-up must reject overflow. */
    CHECK(!vmm_mmio_page_range(UINT64_MAX - 7, 8, &first, &end));
    CHECK(!vmm_mmio_page_range(UINT64_MAX - 4095, 1, &first, &end));
    CHECK(!vmm_mmio_page_range(UINT64_MAX - 4096, 1, &first, &end));
    CHECK(!vmm_mmio_page_range(UINT64_MAX & ~0xfffULL, 4096, &first, &end));

    if (fails) return 1;
    puts("MMIO RANGE TESTS PASSED");
    return 0;
}
