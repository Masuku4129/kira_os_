#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pmm.h"
#include "arch/aarch64/pmm_arm.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("pmm_arm FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)

int main(void) {
    static uint64_t map[64];
    const pmm_arm_region_t ram[] = { { 0x100000, 0x100000 }, { 0x180000, 0x80000 } };
    CHECK(pmm_arm_map_bytes(0x100000, 0x200000) == 64);
    CHECK(pmm_arm_map_bytes(0x200000, 0x100000) == 0);
    pmm_arm_init(ram, 2, 0x100000, 0x200000, map, 0x100000, 0x110000);
    CHECK(pmm_get_total_memory_kb() == 1024);
    CHECK(pmm_get_free_memory_kb() == 960);
    CHECK(pmm_free_page(0x100000) == -1);                 /* kernel reservation is permanent */

    uint64_t a = pmm_alloc_page();
    CHECK(a == 0x110000);
    pmm_arm_reserve(a, a + 4096);                         /* reserve an already allocated page */
    CHECK(pmm_free_page(a) == -1);

    pmm_arm_reserve(0x120001, 0x120002);                  /* unaligned sub-page reservation rounds outward */
    CHECK(pmm_free_page(0x120000) == -1);
    CHECK(pmm_get_free_memory_kb() == 952);

    uint64_t blk = pmm_alloc_contiguous(4);
    CHECK(blk == 0x111000);
    CHECK(pmm_free_contiguous(blk, 4) == 0);
    CHECK(pmm_free_contiguous(blk, 4) == -1);             /* double free */
    uint64_t aligned = pmm_alloc_contiguous_aligned_below(4, 4, 0x200000);
    CHECK(aligned && aligned % (4 * 4096) == 0 && aligned + 4 * 4096 <= 0x200000);
    CHECK(pmm_free_contiguous(aligned, 4) == 0);
    uint64_t half_aligned = pmm_alloc_contiguous_aligned_below(2, 8, 0x200000);
    CHECK(half_aligned && half_aligned % (8 * 4096) == 0 && half_aligned + 2 * 4096 <= 0x200000);
    CHECK((half_aligned & 0x1FFFFULL) + 2 * 4096 <= 0x20000ULL);
    CHECK(pmm_free_contiguous(half_aligned, 2) == 0);
    CHECK(pmm_alloc_contiguous_aligned_below(4, 3, 0x200000) == 0); // alignment must be a power of two

    /* A reservation inside a live run must make free_contiguous atomic: no prefix is freed. */
    blk = pmm_alloc_contiguous(4);
    CHECK(blk == 0x111000);
    pmm_arm_reserve(blk + 4096, blk + 8192);
    CHECK(pmm_free_contiguous(blk, 4) == -1);
    CHECK(pmm_free_page(blk) == 0);
    CHECK(pmm_free_page(blk + 4096) == -1);
    CHECK(pmm_free_page(blk + 8192) == 0);
    CHECK(pmm_free_page(blk + 12288) == 0);

    puts(fails ? "pmm_arm: FAIL" : "pmm_arm: reserved pages, range rounding and atomic contiguous free PASS");
    return fails != 0;
}
