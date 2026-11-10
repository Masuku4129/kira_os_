#include "drivers/iommu/arm_smmu_domain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define REG_BYTES (2u * PAGE)
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static _Alignas(PAGE) uint8_t regs[REG_BYTES];
static _Alignas(PAGE) uint8_t stream_table[256u * 64u];
static _Alignas(PAGE) uint8_t cmdq[256u * 16u];
static _Alignas(PAGE) uint8_t dma_pool[8u * PAGE];
static size_t dma_used;
static unsigned misalign_virtual_alloc_at;
static unsigned alloc_calls, free_calls, clean_calls, barrier_calls;
static unsigned metadata_allocs, metadata_frees;

struct test_context { uint8_t *registers; };

static volatile void *map_mmio(void *opaque, uint64_t physical, size_t length)
{
    (void)opaque;
    if (physical != 0x100000u || length > sizeof(regs)) return 0;
    return regs;
}

static void *alloc_dma_pages(void *opaque, size_t pages, uint64_t *physical)
{
    size_t bytes = pages * PAGE;
    (void)opaque;
    ++alloc_calls;
    if (dma_used + bytes > sizeof(dma_pool)) return 0;
    *physical = 0x300000u + dma_used;
    void *v = dma_pool + dma_used;
    dma_used += bytes;
    if (alloc_calls == misalign_virtual_alloc_at)
        return (uint8_t *)v + 1u;
    return v;
}

static void *alloc_metadata(void *opaque, size_t bytes)
{
    (void)opaque;
    void *p = calloc(1, bytes);
    if (p) ++metadata_allocs;
    return p;
}

static void free_metadata(void *opaque, void *memory)
{
    (void)opaque;
    if (memory) { ++metadata_frees; free(memory); }
}

static void free_dma_pages(void *opaque, void *v, uint64_t physical, size_t pages)
{
    (void)opaque; (void)v; (void)physical; (void)pages;
    ++free_calls;
}

static void clean_cache(void *opaque, const void *address, size_t length)
{
    (void)opaque; (void)address; (void)length;
    ++clean_calls;
}

static void write_barrier(void *opaque)
{
    (void)opaque;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ++barrier_calls;
}

/* Hardware model: every CMDQ command is consumed before the next poll. */
static void read_barrier(void *opaque)
{
    struct test_context *ctx = opaque;
    uint32_t producer = *(volatile uint32_t *)(ctx->registers + 0x0098u);
    *(volatile uint32_t *)(ctx->registers + 0x009cu) = producer;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(stream_table, 0, sizeof(stream_table));
    memset(cmdq, 0, sizeof(cmdq));
    memset(dma_pool, 0xa5, sizeof(dma_pool));
    dma_used = 0;
    alloc_calls = free_calls = clean_calls = barrier_calls = 0;
    metadata_allocs = metadata_frees = 0;
    misalign_virtual_alloc_at = 0;
}

static void init_smmu(struct kira_smmu *s)
{
    memset(s, 0, sizeof(*s));
    s->registers = regs;
    s->idr0 = (2u << 2) | (1u << 1); /* AArch64 translation tables + S1P */
    s->idr5 = 5u | (1u << 4);        /* 48-bit OAS + 4 KiB granule */
    s->enabled = 1;
    s->stream_id_limit = 256;
    s->stream_table = stream_table;
    s->stream_table_physical = 0x200000u;
    s->cmdq = cmdq;
    s->cmdq_physical = 0x210000u;
}

static struct kira_smmu_platform platform(struct test_context *ctx)
{
    struct kira_smmu_platform p;
    memset(&p, 0, sizeof(p));
    p.context = ctx;
    p.map_mmio = map_mmio;
    p.alloc_dma_pages = alloc_dma_pages;
    p.free_dma_pages = free_dma_pages;
    p.alloc_metadata = alloc_metadata;
    p.free_metadata = free_metadata;
    p.clean_cache = clean_cache;
    p.write_barrier = write_barrier;
    p.read_barrier = read_barrier;
    return p;
}

int main(void)
{
    struct kira_smmu smmu;
    struct kira_smmu_domain domain;
    struct test_context ctx = { regs };
    struct kira_smmu_platform p;
    uint64_t *ste, *cd;
    int rc;

    reset();
    init_smmu(&smmu);
    memset(&domain, 0, sizeof(domain));
    p = platform(&ctx);

    rc = kira_smmu_domain_create(&domain, &smmu, 7, &p);
    CHECK(rc == KIRA_SMMU_OK);
    CHECK(domain.active && domain.stream_id == 7);
    CHECK(alloc_calls == 2 && free_calls == 0);
    CHECK(clean_calls >= 3 && barrier_calls >= 2);

    ste = (uint64_t *)stream_table + 7u * 8u;
    cd = (uint64_t *)domain.context_table;
    CHECK((ste[0] & 1ull) != 0);                  /* STE.V */
    CHECK(((ste[0] >> 1) & 7ull) == 5ull);       /* STE.CFG = S1_TRANS */
    CHECK((ste[0] & 0x000fffffffffffc0ull) ==
          domain.context_table_physical);         /* STE.S1ContextPtr in DW0 */
    CHECK((ste[1] & 3ull) == 2ull);              /* STE.S1DSS = SSID0 */
    CHECK(((ste[1] >> 2) & 3ull) == 1ull);       /* inner WB read-allocate */
    CHECK(((ste[1] >> 4) & 3ull) == 1ull);       /* outer WB read-allocate */
    CHECK(((ste[1] >> 6) & 3ull) == 3ull);       /* inner-shareable */

    CHECK((cd[0] & (1ull << 31)) != 0);           /* CD.V */
    CHECK((cd[0] & 0x3full) == 16ull);            /* T0SZ = 16 (48-bit VA) */
    CHECK(((cd[0] >> 30) & 1ull) == 1ull);       /* EPD1 */
    CHECK(((cd[0] >> 32) & 7ull) == 5ull);       /* IPS = 48-bit */
    CHECK(((cd[0] >> 41) & 1ull) == 1ull);       /* AA64 */
    CHECK(((cd[0] >> 48) & 0xffffull) == 8ull); /* unique nonzero ASID */
    CHECK(cd[1] == domain.stage1_root_physical); /* CD.TTB0 */
    CHECK(cd[2] == 0);
    CHECK(cd[3] == 0xffull); /* MAIR AttrIndx 0 is normal write-back memory. */

    /* AArch64 stage-1 page descriptors must encode AF, SH and AP correctly. */
    CHECK(kira_smmu_domain_map_4k(&domain, 0x12345000u, 0x500000u,
                                  KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE,
                                  &p) == KIRA_SMMU_OK);
    {
        uint64_t *l0 = (uint64_t *)domain.stage1_root;
        uint64_t *l1 = (uint64_t *)(dma_pool + (0x302000u - 0x300000u));
        uint64_t *l2 = (uint64_t *)(dma_pool + (0x303000u - 0x300000u));
        uint64_t *l3 = (uint64_t *)(dma_pool + (0x304000u - 0x300000u));
        uint64_t leaf = l3[(0x12345000ull >> 12) & 0x1ffu];
        CHECK((l0[(0x12345000ull >> 39) & 0x1ffu] & 3ull) == 3ull);
        CHECK((l1[(0x12345000ull >> 30) & 0x1ffu] & 3ull) == 3ull);
        CHECK((l2[(0x12345000ull >> 21) & 0x1ffu] & 3ull) == 3ull);
        CHECK((leaf & 0x0000fffffffff000ull) == 0x500000u);
        CHECK((leaf & 3ull) == 3ull);
        CHECK(((leaf >> 8) & 3ull) == 3ull); /* inner-shareable */
        CHECK((leaf & (1ull << 10)) != 0);  /* access flag */
        CHECK((leaf & (1ull << 11)) != 0);  /* nG: translation is ASID-tagged */
        CHECK(((leaf >> 6) & 3ull) == 0ull); /* EL1 read/write */
    }
    CHECK(kira_smmu_domain_map_4k(&domain, 0x12346000u, 0x501000u,
                                  KIRA_SMMU_MAP_READ, &p) == KIRA_SMMU_OK);
    {
        uint64_t *l3 = (uint64_t *)(dma_pool + (0x304000u - 0x300000u));
        uint64_t leaf = l3[(0x12346000ull >> 12) & 0x1ffu];
        CHECK(((leaf >> 6) & 3ull) == 2ull); /* EL1 read-only */
    }
    CHECK(kira_smmu_domain_map_4k(&domain, 0x12347000u, 0x502000u,
                                  KIRA_SMMU_MAP_WRITE, &p) == KIRA_SMMU_INVALID);
    CHECK(kira_smmu_domain_map_4k(&domain, 0x12348000u, (1ull << 48),
                                  KIRA_SMMU_MAP_READ, &p) == KIRA_SMMU_INVALID);
    CHECK(kira_smmu_domain_unmap_4k(&domain, 0x12345000u, &p) == KIRA_SMMU_OK);
    {
        uint64_t *l3 = (uint64_t *)(dma_pool + (0x304000u - 0x300000u));
        CHECK(l3[(0x12345000ull >> 12) & 0x1ffu] == 0);
    }

    rc = kira_smmu_domain_destroy(&domain, &p);
    CHECK(rc == KIRA_SMMU_OK);
    CHECK(((uint64_t *)cmdq)[1] == 1ull);  /* linear STE leaf invalidation */
    CHECK(((uint64_t *)cmdq)[5] == 1ull);  /* STE publish leaf invalidation */
    CHECK(((uint64_t *)cmdq)[25] == 1ull); /* linear CD leaf invalidation */
    CHECK(((uint64_t *)cmdq)[8] == (0x11ull | (8ull << 48)));  /* map TLBI */
    CHECK(((uint64_t *)cmdq)[12] == (0x11ull | (8ull << 48))); /* read-only TLBI */
    CHECK(((uint64_t *)cmdq)[16] == (0x11ull | (8ull << 48))); /* unmap TLBI */
    CHECK(((uint64_t *)cmdq)[24] == (0x05ull | (7ull << 32))); /* CFGI_CD: opcode + StreamID, SSID=0 */
    CHECK(((uint64_t *)cmdq)[25] == 1ull); /* Leaf is bit 64, the low bit of command word 1 */
    CHECK(((uint64_t *)cmdq)[28] == (0x11ull | (8ull << 48))); /* destroy TLBI */
    CHECK(!domain.active && free_calls == 5);
    CHECK(metadata_allocs == metadata_frees);
    CHECK(ste[0] == 0);

    /* Reject stage-2-only hardware before allocating any DMA memory. */
    reset();
    init_smmu(&smmu);
    smmu.idr0 &= ~(1u << 1);
    memset(&domain, 0, sizeof(domain));
    p = platform(&ctx);
    CHECK(kira_smmu_domain_create(&domain, &smmu, 7, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* Reject hardware without 4 KiB translation granules. */
    reset();
    init_smmu(&smmu);
    smmu.idr5 &= ~(1u << 4);
    memset(&domain, 0, sizeof(domain));
    p = platform(&ctx);
    CHECK(kira_smmu_domain_create(&domain, &smmu, 7, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* Without ASID16, stream 255 cannot be assigned a unique nonzero ASID. */
    reset();
    init_smmu(&smmu);
    memset(&domain, 0, sizeof(domain));
    p = platform(&ctx);
    CHECK(kira_smmu_domain_create(&domain, &smmu, 255, &p) == KIRA_SMMU_UNSUPPORTED);
    CHECK(alloc_calls == 0);

    /* Unaligned CPU virtual memory is rejected and every partial allocation is unwound. */
    reset();
    init_smmu(&smmu);
    memset(&domain, 0, sizeof(domain));
    p = platform(&ctx);
    misalign_virtual_alloc_at = 1;
    CHECK(kira_smmu_domain_create(&domain, &smmu, 9, &p) == KIRA_SMMU_MEMORY_FAILED);
    CHECK(!domain.active && !domain.smmu && !domain.table_pages);
    CHECK(free_calls == 2 && metadata_allocs == metadata_frees);

    puts("ARM SMMUv3 domain: stage-1 STE/CD programming and teardown passed");
    return 0;
}
