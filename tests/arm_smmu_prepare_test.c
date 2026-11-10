#include "drivers/iommu/arm_smmu.h"
#include <stdio.h>
#include <string.h>

#define PAGE 4096u
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static _Alignas(PAGE) uint8_t registers_page[2u * PAGE];
static _Alignas(PAGE) uint8_t dma_pool[12u * PAGE];
static unsigned alloc_calls, free_calls, clean_calls, map_calls;
static unsigned fail_alloc_at, misalign_alloc_at;
static size_t freed_pages, pool_used;
static uint64_t next_physical;

static volatile void *test_map_mmio(void *context, uint64_t physical, size_t length)
{
    (void)context;
    ++map_calls;
    if (physical != 0x100000u || length != 2u * PAGE)
        return 0;
    return registers_page;
}

static void *test_alloc_dma_pages(void *context, size_t pages, uint64_t *physical)
{
    size_t offset;
    (void)context;
    ++alloc_calls;
    if (alloc_calls == fail_alloc_at)
        return 0;
    offset = pool_used;
    if (offset + pages * PAGE > sizeof(dma_pool))
        return 0;
    pool_used += pages * PAGE;
    *physical = next_physical + offset;
    if (alloc_calls == misalign_alloc_at)
        ++*physical;
    return dma_pool + offset;
}

static void test_free_dma_pages(void *context, void *virtual_address,
                      uint64_t physical, size_t pages)
{
    (void)context;
    (void)physical;
    (void)virtual_address;
    ++free_calls;
    freed_pages += pages;
}

static void test_clean_cache(void *context, const void *address, size_t length)
{
    (void)context;
    (void)address;
    (void)length;
    ++clean_calls;
}

static void reset_test_platform(void)
{
    memset(registers_page, 0, sizeof(registers_page));
    memset(dma_pool, 0xa5, sizeof(dma_pool));
    alloc_calls = free_calls = clean_calls = map_calls = 0;
    fail_alloc_at = misalign_alloc_at = 0;
    freed_pages = pool_used = 0;
    next_physical = 0x201000u;
    *(uint32_t *)(registers_page + 0x0000u) = 1u;
    *(uint32_t *)(registers_page + 0x0004u) = (8u << 21) | (8u << 16) | 8u;
}

static struct kira_smmu_platform platform(void)
{
    struct kira_smmu_platform p;
    memset(&p, 0, sizeof(p));
    p.map_mmio = test_map_mmio;
    p.alloc_dma_pages = test_alloc_dma_pages;
    p.free_dma_pages = test_free_dma_pages;
    p.clean_cache = test_clean_cache;
    return p;
}

int main(void)
{
    struct kira_smmu smmu;
    struct kira_smmu_platform p;

    /* Mandatory cleanup callback is validated before MMIO mapping. */
    reset_test_platform();
    p = platform();
    p.free_dma_pages = 0;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_INVALID);
    CHECK(map_calls == 0 && alloc_calls == 0);

    /* Physical register base must be page aligned. */
    reset_test_platform();
    p = platform();
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100001u, &p) == KIRA_SMMU_INVALID);
    CHECK(map_calls == 0);

    /* Unsupported identification is rejected before allocating DMA memory. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0000u) = 0;
    p = platform();
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(map_calls == 1 && alloc_calls == 0);

    /* A two-level-only stream-table implementation cannot accept our linear table. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0000u) = 1u | (2u << 27);
    p = platform();
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* ST_LVL=1 permits a linear table even when two-level tables are supported. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0000u) = 1u | (1u << 27);
    p = platform();
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_OK);
    CHECK(smmu.stream_table != 0);

    /* Successful prepare allocates one 16 KiB stream table and two queues. */
    reset_test_platform();
    p = platform();
    memset(&smmu, 0, sizeof(smmu));
    *(uint32_t *)(registers_page + 0x0020u) = 0x0du;
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_OK);
    CHECK(*(uint32_t *)(registers_page + 0x0020u) == 0u);
    CHECK(alloc_calls == 3 && free_calls == 0);
    CHECK(smmu.stream_table == dma_pool + 3u * PAGE);
    CHECK(smmu.stream_table_physical == next_physical + 3u * PAGE);
    CHECK(smmu.cmdq == dma_pool + 7u * PAGE);
    CHECK(smmu.evtq == dma_pool + 9u * PAGE);
    CHECK(smmu.evtq_physical == next_physical + 9u * PAGE);
    CHECK(smmu.evtq_allocation_pages == 3u);
    CHECK(smmu.queue_entries == 256u && smmu.enabled == 0);
    CHECK(smmu.stream_id_limit == 256u);
    CHECK(*(uint32_t *)(registers_page + 0x0088u) == 8u);
    CHECK((smmu.stream_table_physical & 0x3fffu) == 0);
    CHECK(*(uint64_t *)(registers_page + 0x0080u) ==
          smmu.stream_table_physical);
    CHECK(clean_calls == 3);

    /* The hardware queue base registers encode LOG2SIZE=8, not a valid bit. */
    CHECK(kira_smmu_v3_enable(&smmu, &p) == KIRA_SMMU_TIMEOUT);
    CHECK(*(uint64_t *)(registers_page + 0x0090u) ==
          ((smmu.cmdq_physical & 0x000fffffffffffe0ull) | 8ull));
    CHECK(*(uint64_t *)(registers_page + 0x00a0u) ==
          ((smmu.evtq_physical & 0x000fffffffffffe0ull) | 8ull));
    CHECK(*(uint32_t *)(registers_page + 0x10a8u) == 0u);
    CHECK(*(uint32_t *)(registers_page + 0x10acu) == 0u);

    /* Hardware with a narrower SID field must not expose unreachable STEs. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0004u) = (8u << 21) | (8u << 16) | 4u;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_OK);
    CHECK(smmu.stream_id_limit == 16u);
    CHECK(*(uint32_t *)(registers_page + 0x0088u) == 4u);

    /* Reject hardware that cannot implement this fixed 256-entry CMDQ. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0004u) = (7u << 21) | (8u << 16) | 8u;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* Preset hardware tables/queues are read-only and must not be overwritten. */
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0004u) |= (1u << 30);
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);
    reset_test_platform();
    *(uint32_t *)(registers_page + 0x0004u) |= (1u << 29);
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* Failure allocating CMDQ must release the already allocated stream table. */
    reset_test_platform();
    p = platform();
    fail_alloc_at = 2;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_MEMORY_FAILED);
    CHECK(alloc_calls == 2 && free_calls == 1 && freed_pages == 7u);
    CHECK(smmu.stream_table == 0 && smmu.stream_table_physical == 0);

    /* Failure allocating EVTQ must release CMDQ and the stream table. */
    reset_test_platform();
    p = platform();
    fail_alloc_at = 3;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_MEMORY_FAILED);
    CHECK(alloc_calls == 3 && free_calls == 2 && freed_pages == 8u);
    CHECK(smmu.cmdq == 0 && smmu.stream_table == 0);

    /* An unaligned DMA address is rejected and the allocation is returned. */
    reset_test_platform();
    p = platform();
    misalign_alloc_at = 1;
    memset(&smmu, 0, sizeof(smmu));
    CHECK(kira_smmu_v3_prepare(&smmu, 0x100000u, &p) == KIRA_SMMU_MEMORY_FAILED);
    CHECK(alloc_calls == 1 && free_calls == 1 && freed_pages == 7u);
    CHECK(smmu.stream_table == 0);

    puts("ARM SMMUv3 prepare: validation, stream table setup and allocation unwind passed");
    return 0;
}
