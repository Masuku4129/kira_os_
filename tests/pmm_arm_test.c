#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pmm.h"
#include "pmm_arm.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

int main(void) {
    const uint64_t lo = 0x100000, pages = 128, hi = lo + pages * PMM_PAGE_SIZE;
    uint64_t map[4] = {0}; /* two 64-bit maps: used + reserved */
    pmm_arm_region_t regions[2] = {
        { lo, pages * PMM_PAGE_SIZE },
        { lo, pages * PMM_PAGE_SIZE } /* duplicate/overlapping FDT entry must not double-count */
    };

    CHECK(pmm_arm_map_bytes(lo, hi) == sizeof map);
    CHECK(pmm_arm_map_bytes(hi, lo) == 0);
    pmm_arm_init(regions, 2, lo, hi, map, lo, lo + 2 * PMM_PAGE_SIZE);

    CHECK(pmm_get_total_memory_kb() == pages * 4);
    CHECK(pmm_get_free_memory_kb() == (pages - 2) * 4);
    CHECK(pmm_free_page(lo) == -1); /* kernel-reserved page */
    CHECK(pmm_free_page(lo + PMM_PAGE_SIZE) == -1); /* second reserved page */

    uint64_t one = pmm_alloc_page();
    CHECK(one == lo + 2 * PMM_PAGE_SIZE);
    CHECK(pmm_free_page(one) == 0);
    CHECK(pmm_free_page(one) == -1); /* double free */

    uint64_t block = pmm_alloc_contiguous(4);
    CHECK(block == lo + 2 * PMM_PAGE_SIZE);
    uint32_t before = pmm_get_free_memory_kb();
    CHECK(pmm_free_contiguous(block, 4) == 0);
    CHECK(pmm_get_free_memory_kb() == before + 16);

    block = pmm_alloc_contiguous(4);
    CHECK(block == lo + 2 * PMM_PAGE_SIZE);
    CHECK(pmm_free_page(block + PMM_PAGE_SIZE) == 0); /* make an invalid middle page */
    before = pmm_get_free_memory_kb();
    CHECK(pmm_free_contiguous(block, 4) == -1);
    CHECK(pmm_get_free_memory_kb() == before); /* no partial release */
    CHECK(pmm_free_page(block) == 0);
    CHECK(pmm_free_page(block + 2 * PMM_PAGE_SIZE) == 0);
    CHECK(pmm_free_page(block + 3 * PMM_PAGE_SIZE) == 0);

    uint64_t reserved = lo + 20 * PMM_PAGE_SIZE;
    before = pmm_get_free_memory_kb();
    pmm_arm_reserve(reserved, reserved + 1);
    CHECK(pmm_get_free_memory_kb() == before - 4);
    CHECK(pmm_free_page(reserved) == -1); /* late reservation cannot be freed */

    CHECK(pmm_free_contiguous(0, 0) == -1);
    CHECK(pmm_free_contiguous(block + 1, 1) == -1);
    CHECK(pmm_free_contiguous(UINT64_MAX - 4095, 2) == -1);
    CHECK(pmm_free_page(hi) == -1);

    if (fails) return 1;
    puts("ARM64 PMM TESTS PASSED");
    return 0;
}
