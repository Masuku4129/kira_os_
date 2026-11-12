#include <stdint.h>
#include <stdio.h>
#include "vmm_range.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    uint64_t first = 0, size = 0;
    CHECK(vmm_mmio_page_range(0x1003, 1, 4096, &first, &size));
    CHECK(first == 0x1000 && size == 4096);
    CHECK(vmm_mmio_page_range(0x1FFF, 2, 4096, &first, &size));
    CHECK(first == 0x1000 && size == 8192);
    CHECK(vmm_mmio_page_range(0x2000, 4096, 4096, &first, &size));
    CHECK(first == 0x2000 && size == 4096);
    CHECK(vmm_mmio_page_range(UINT64_MAX - 4096, 1, 4096, &first, &size));
    CHECK(first == UINT64_MAX - 8191 && size == 4096);
    CHECK(!vmm_mmio_page_range(UINT64_MAX - 2047, 1024, 4096, &first, &size));
    CHECK(!vmm_mmio_page_range(UINT64_MAX - 4095, 4095, 4096, &first, &size));
    CHECK(!vmm_mmio_page_range(1, UINT64_MAX, 4096, &first, &size));
    CHECK(!vmm_mmio_page_range(0, 0, 4096, &first, &size));
    CHECK(!vmm_mmio_page_range(0, 1, 3000, &first, &size));
    CHECK(!vmm_mmio_page_range(0, 1, 4096, 0, &size));
    puts("vmm_range: MMIO page rounding, overflow and invalid input PASS");
    return 0;
}
