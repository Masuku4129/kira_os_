#include "drivers/iommu/arm_smmu.h"
#include "drivers/iommu/arm_smmu_domain.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE 4096u
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)
#define REG_BASE 0x100000u
#define DMA_BASE 0x201000ull
#define PA_MASK  0x0000fffffffff000ull

static _Alignas(PAGE) uint8_t registers_page[2u * PAGE];
static _Alignas(PAGE) uint8_t dma_pool[96u * PAGE];
static size_t pool_used;
static unsigned alloc_calls, free_calls, freed_pages, clean_calls;

static uint32_t reg_read(uint32_t off)
{
    return *(volatile uint32_t *)(registers_page + off);
}

static void reg_write(uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(registers_page + off) = value;
}

static volatile void *test_map_mmio(void *context, uint64_t physical, size_t length)
{
    (void)context;
    if (physical != REG_BASE || length != sizeof(registers_page))
        return 0;
    return registers_page;
}

static void *test_alloc_dma_pages(void *context, size_t pages, uint64_t *physical)
{
    size_t offset;
    (void)context;
    ++alloc_calls;
    offset = pool_used;
    if (pages > (sizeof(dma_pool) - offset) / PAGE)
        return 0;
    pool_used += pages * PAGE;
    *physical = DMA_BASE + offset;
    return dma_pool + offset;
}

static void test_free_dma_pages(void *context, void *virtual_address,
                                uint64_t physical, size_t pages)
{
    (void)context;
    (void)virtual_address;
    (void)physical;
    ++free_calls;
    freed_pages += (unsigned)pages;
}

static void test_clean_cache(void *context, const void *address, size_t length)
{
    (void)context;
    (void)address;
    (void)length;
    ++clean_calls;
}

static void test_barrier(void *context)
{
    (void)context;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void test_delay(void *context)
{
    uint32_t cr0;
    (void)context;
    cr0 = reg_read(0x0020u);
    reg_write(0x0024u, cr0 & 0x0du); /* SMMUEN, EVENTQEN, CMDQEN acknowledgements */
    reg_write(0x009cu, reg_read(0x0098u) & 0x1ffu); /* model command consumption */
}

static struct kira_smmu_platform make_platform(void)
{
    struct kira_smmu_platform p;
    memset(&p, 0, sizeof(p));
    p.map_mmio = test_map_mmio;
    p.alloc_dma_pages = test_alloc_dma_pages;
    p.free_dma_pages = test_free_dma_pages;
    p.clean_cache = test_clean_cache;
    p.write_barrier = test_barrier;
    p.read_barrier = test_barrier;
    p.delay = test_delay;
    return p;
}

static void reset_model(void)
{
    memset(registers_page, 0, sizeof(registers_page));
    memset(dma_pool, 0xa5, sizeof(dma_pool));
    pool_used = 0;
    alloc_calls = free_calls = freed_pages = clean_calls = 0;
    reg_write(0x0000u, (1u << 1) | (2u << 2)); /* S1P + AArch64 translation tables */
    reg_write(0x0004u, (8u << 21) | (8u << 16) | 8u); /* 256-entry CMDQ/EVTQ and SID table */
    reg_write(0x0014u, (1u << 4) | 5u); /* 4 KiB granule, 48-bit output address */
}

static void *table_for(struct kira_smmu_domain *d, uint64_t entry)
{
    uint64_t pa = entry & PA_MASK;
    uint16_t i;
    for (i = 0; i < d->table_page_count; ++i)
        if (d->table_pages[i].physical == pa)
            return d->table_pages[i].virtual_address;
    return 0;
}

int main(void)
{
    struct kira_smmu smmu;
    struct kira_smmu_domain domain = {0};
    struct kira_smmu_domain duplicate = {0};
    struct kira_smmu_platform p;
    uint64_t *ste, *cd, *root, *l1, *l2, *l3;
    uint64_t ste0, page;
    uint64_t *commands;
    unsigned freed_before;

    reset_model();
    memset(&smmu, 0, sizeof(smmu));
    p = make_platform();

    CHECK(kira_smmu_v3_prepare(&smmu, REG_BASE, &p) == KIRA_SMMU_OK);
    CHECK(kira_smmu_v3_enable(&smmu, &p) == KIRA_SMMU_OK);
    CHECK(smmu.enabled == 1);

    CHECK(kira_smmu_domain_create(&domain, &smmu, 7u, &p) == KIRA_SMMU_OK);
    CHECK(domain.active == 1 && domain.stream_id == 7u && domain.asid == 7u);
    CHECK(smmu.stream_ids[0] & (1u << 7));

    ste = (uint64_t *)smmu.stream_table + 7u * 8u;
    ste0 = ste[0];
    CHECK((ste0 & 1u) == 1u);
    CHECK(((ste0 >> 1) & 7u) == 5u); /* CFG=S1_TRANS */
    CHECK((ste0 & 0x000fffffffffffc0ull) == domain.context_table_physical);
    CHECK((ste[1] & 3u) == 2u);      /* S1DSS=SSID0 */
    CHECK(((ste[1] >> 2) & 3u) == 1u && ((ste[1] >> 4) & 3u) == 1u);
    CHECK(((ste[1] >> 6) & 3u) == 3u); /* inner-shareable CD fetch */

    cd = (uint64_t *)domain.context_table;
    CHECK((cd[0] & (1ull << 31)) != 0);
    CHECK((cd[0] & 0x3full) == 16u); /* 48-bit input VA */
    CHECK(((cd[0] >> 8) & 3u) == 1u && ((cd[0] >> 10) & 3u) == 1u);
    CHECK(((cd[0] >> 12) & 3u) == 3u);
    CHECK((cd[0] & (1ull << 30)) != 0); /* TT1 disabled */
    CHECK((cd[0] & (1ull << 41)) != 0); /* AArch64 */
    CHECK(((cd[0] >> 32) & 7u) == 5u); /* 48-bit output */
    CHECK(((cd[0] >> 48) & 0xffffu) == 7u);
    CHECK((cd[1] & PA_MASK) == domain.stage1_root_physical);
    CHECK(cd[3] == 0xffu);
    commands = (uint64_t *)smmu.cmdq;
    CHECK(commands[0] == (0x05ull | (7ull << 32)) && commands[1] == 1u);
    CHECK(commands[2] == (0x03ull | (7ull << 32)) && commands[3] == 0x46u);
    CHECK(commands[4] == (0x11ull | (7ull << 48)) && commands[5] == 0x46u); /* MAIR AttrIdx 0 = normal WB */

    CHECK(kira_smmu_domain_create(&duplicate, &smmu, 7u, &p) == KIRA_SMMU_INVALID);
    CHECK(duplicate.active == 0);

    CHECK(kira_smmu_domain_map_4k(&domain, 0x4000u, 0x300000u,
                                  KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE, &p) == KIRA_SMMU_OK);
    CHECK(domain.table_page_count == 3u);
    root = (uint64_t *)domain.stage1_root;
    CHECK((root[0] & 3u) == 3u);
    l1 = (uint64_t *)table_for(&domain, root[0]);
    CHECK(l1 != 0 && (l1[0] & 3u) == 3u);
    l2 = (uint64_t *)table_for(&domain, l1[0]);
    CHECK(l2 != 0 && (l2[0] & 3u) == 3u);
    l3 = (uint64_t *)table_for(&domain, l2[0]);
    CHECK(l3 != 0);
    page = l3[4];
    CHECK((page & PA_MASK) == 0x300000u);
    CHECK((page & 3u) == 3u); /* valid page descriptor */
    CHECK(((page >> 6) & 3u) == 2u); /* EL0/EL1 read/write */
    CHECK(((page >> 8) & 3u) == 3u); /* inner-shareable */
    CHECK((page & (1ull << 10)) != 0); /* access flag */
    CHECK((page & (1ull << 11)) != 0); /* non-global */
    CHECK((page & (1ull << 53)) != 0 && (page & (1ull << 54)) != 0); /* execute-never */

    CHECK(kira_smmu_domain_map_4k(&domain, 0x5000u, 0x301000u,
                                  KIRA_SMMU_MAP_READ, &p) == KIRA_SMMU_OK);
    CHECK(((l3[5] >> 6) & 3u) == 3u); /* EL0/EL1 read-only */
    CHECK(kira_smmu_domain_map_4k(&domain, 0x6000u, 0x302000u,
                                  KIRA_SMMU_MAP_WRITE, &p) == KIRA_SMMU_INVALID);
    CHECK(kira_smmu_domain_map_4k(&domain, 0x4000u, 0x303000u,
                                  KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE, &p) ==
          KIRA_SMMU_HARDWARE_ERROR);

    CHECK(kira_smmu_domain_unmap_4k(&domain, 0x4000u, &p) == KIRA_SMMU_OK);
    CHECK(l3[4] == 0);

    freed_before = free_calls;
    CHECK(kira_smmu_domain_destroy(&domain, &p) == KIRA_SMMU_OK);
    CHECK(domain.active == 0 && domain.smmu == 0);
    CHECK((smmu.stream_ids[0] & (1u << 7)) == 0);
    CHECK(ste[0] == 0 && ste[1] == 0);
    CHECK(free_calls - freed_before == 5u); /* three intermediate tables + root + CD table */
    CHECK(freed_pages == 5u);

    puts("ARM SMMUv3 domain: STE/CD encoding, stage-1 maps, ASID invalidation and teardown passed");
    return 0;
}
