/*
 * SMMUv3 stream-table and stage-1 translation-domain support.
 *
 * Uses a linear stream table and a four-level 4 KiB translation regime.
 * The stream ID must fit the implemented linear-table span. Command queue
 * invalidations are issued synchronously; timeout is reported to the caller.
 */

#include "arm_smmu_domain.h"

#define SMMU_PAGE_SIZE 4096u
#define SMMU_STRTAB_ENTRIES 256u
#define SMMU_STRTAB_ENTRY_BYTES 64u
#define SMMU_STRTAB_BYTES (SMMU_STRTAB_ENTRIES * SMMU_STRTAB_ENTRY_BYTES)
#define SMMU_STRTAB_PAGES (SMMU_STRTAB_BYTES / SMMU_PAGE_SIZE)
#define SMMU_ENTRIES 512u
#define SMMU_STRTAB_BASE 0x0080u
#define SMMU_STRTAB_BASE_CFG 0x0088u
#define SMMU_CMDQ_PROD 0x0098u
#define SMMU_CMDQ_CONS 0x009cu
#define SMMU_CMDQ_BASE 0x0090u
#define SMMU_CMDQ_ENTRIES 256u
#define SMMU_POLL_LIMIT 1000000u
#define SMMU_TTBR_ADDR 0x0000fffffffff000ull

/* SMMUv3 command opcodes. */
#define CMD_CFGI_STE 0x03u
#define CMD_CFGI_ALL 0x04u
#define CMD_TLBI_NH_ASID 0x11u
#define CMD_SYNC 0x46u

static uint64_t *smmu_words(void *p) { return (uint64_t *)p; }

static void smmu_zero(void *p, size_t bytes)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    size_t i;
    for (i = 0; i < bytes; ++i)
        b[i] = 0;
}

static void *smmu_alloc_pages(const struct kira_smmu_platform *p,
                              size_t pages, uint64_t *pa)
{
    void *v = p->alloc_dma_pages(p->context, pages, pa);
    size_t bytes = pages * SMMU_PAGE_SIZE;
    if (!v || !*pa || (*pa & 0xfffu)) {
        if (v && p->free_dma_pages)
            p->free_dma_pages(p->context, v, *pa, pages);
        *pa = 0;
        return 0;
    }
    smmu_zero(v, bytes);
    if (p->clean_cache)
        p->clean_cache(p->context, v, bytes);
    return v;
}

static void *smmu_alloc(const struct kira_smmu_platform *p, uint64_t *pa)
{
    return smmu_alloc_pages(p, 1, pa);
}

/*
 * CMDQ pointers contain an 8-bit index and a wrap bit at bit 8 for
 * this 256-entry queue. Comparing only the index makes full and empty
 * indistinguishable after the producer wraps.
 */
#define SMMU_CMDQ_INDEX_MASK 0xffu
#define SMMU_CMDQ_WRAP       0x100u
#define SMMU_CMDQ_PTR_MASK   0x1ffu

static uint32_t smmu_cmdq_next(uint32_t pointer)
{
    uint32_t index = pointer & SMMU_CMDQ_INDEX_MASK;
    uint32_t next = (pointer & SMMU_CMDQ_WRAP) |
                    ((index + 1u) & SMMU_CMDQ_INDEX_MASK);

    if (index == SMMU_CMDQ_INDEX_MASK)
        next ^= SMMU_CMDQ_WRAP;
    return next;
}

static int smmu_cmd_submit(struct kira_smmu *s,
                           const struct kira_smmu_platform *p,
                           uint64_t word0, uint64_t word1,
                           uint32_t *submitted_pointer)
{
    volatile uint64_t *queue = (volatile uint64_t *)s->cmdq;
    uint32_t producer = s->cmdq_producer & SMMU_CMDQ_PTR_MASK;
    uint32_t consumer;
    uint32_t next;
    uint32_t i;

    for (i = 0; i < SMMU_POLL_LIMIT; ++i) {
        consumer = *(volatile uint32_t *)(s->registers + SMMU_CMDQ_CONS) &
                   SMMU_CMDQ_PTR_MASK;
        if (!((producer & SMMU_CMDQ_INDEX_MASK) ==
              (consumer & SMMU_CMDQ_INDEX_MASK) &&
              ((producer ^ consumer) & SMMU_CMDQ_WRAP) != 0))
            break;
        if (p->delay)
            p->delay(p->context);
    }
    if (i == SMMU_POLL_LIMIT)
        return KIRA_SMMU_TIMEOUT;

    queue[(producer & SMMU_CMDQ_INDEX_MASK) * 2u] = word0;
    queue[(producer & SMMU_CMDQ_INDEX_MASK) * 2u + 1u] = word1;
    if (p->clean_cache)
        p->clean_cache(p->context,
                       (const void *)&queue[(producer & SMMU_CMDQ_INDEX_MASK) * 2u],
                       16);
    if (p->write_barrier)
        p->write_barrier(p->context);

    next = smmu_cmdq_next(producer);
    s->cmdq_producer = next;
    *(volatile uint32_t *)(s->registers + SMMU_CMDQ_PROD) = next;
    if (submitted_pointer)
        *submitted_pointer = next;
    return KIRA_SMMU_OK;
}

static int smmu_cmd_sync(struct kira_smmu *s,
                         const struct kira_smmu_platform *p)
{
    uint32_t completion_pointer = 0;
    uint32_t i;
    int rc = smmu_cmd_submit(s, p, (uint64_t)CMD_SYNC, 0,
                             &completion_pointer);
    if (rc != KIRA_SMMU_OK)
        return rc;

    /*
     * Completion of CMD_SYNC, not merely consumption of the preceding
     * invalidation command, is the synchronization point required before
     * software enables device DMA or frees/reuses translation structures.
     */
    for (i = 0; i < SMMU_POLL_LIMIT; ++i) {
        uint32_t consumer =
            *(volatile uint32_t *)(s->registers + SMMU_CMDQ_CONS) &
            SMMU_CMDQ_PTR_MASK;
        if (consumer == completion_pointer)
            return KIRA_SMMU_OK;
        if (p->delay)
            p->delay(p->context);
    }
    return KIRA_SMMU_TIMEOUT;
}

static int smmu_invalidate_stream(struct kira_smmu_domain *d,
                                  const struct kira_smmu_platform *p)
{
    uint64_t command = (uint64_t)CMD_CFGI_STE |
                       ((uint64_t)d->stream_id << 32);
    int rc = smmu_cmd_submit(d->smmu, p, command, 0, 0);
    if (rc != KIRA_SMMU_OK)
        return rc;
    return smmu_cmd_sync(d->smmu, p);
}

static void *smmu_table_from_entry(const struct kira_smmu_platform *p,
                                   uint64_t entry)
{
    if (!p->map_mmio || !(entry & 1ull))
        return 0;
    return (void *)p->map_mmio(p->context,
                               entry & SMMU_TTBR_ADDR, SMMU_PAGE_SIZE);
}

static int smmu_map_page(struct kira_smmu_domain *d,
                         uint64_t iova, uint64_t physical,
                         uint32_t permissions,
                         const struct kira_smmu_platform *p)
{
    unsigned int shifts[4] = {39, 30, 21, 12};
    uint64_t *table = smmu_words(d->stage1_root);
    unsigned int level;

    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t e = table[index];

        if (!(e & 1ull)) {
            uint64_t pa = 0;
            void *next = smmu_alloc(p, &pa);
            if (!next)
                return KIRA_SMMU_MEMORY_FAILED;
            table[index] = (pa & SMMU_TTBR_ADDR) | 3ull;
            if (p->clean_cache)
                p->clean_cache(p->context, table, SMMU_PAGE_SIZE);
            table = smmu_words(next);
        } else {
            table = (uint64_t *)smmu_table_from_entry(p, e);
            if (!table)
                return KIRA_SMMU_MEMORY_FAILED;
        }
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        uint64_t attr = 1ull; /* valid */
        if (table[index] & 1ull)
            return KIRA_SMMU_HARDWARE_ERROR;
        if (permissions & KIRA_SMMU_MAP_READ)
            attr |= 1ull << 6;
        if (permissions & KIRA_SMMU_MAP_WRITE)
            attr |= 1ull << 7;
        if (!(permissions & (KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE)))
            return KIRA_SMMU_INVALID;
        table[index] = (physical & SMMU_TTBR_ADDR) | attr;
        if (p->clean_cache)
            p->clean_cache(p->context, table, SMMU_PAGE_SIZE);
    }
    return KIRA_SMMU_OK;
}

int kira_smmu_domain_create(struct kira_smmu_domain *d,
                            struct kira_smmu *s,
                            uint32_t stream_id,
                            const struct kira_smmu_platform *p)
{
    uint64_t st_pa = 0, ct_pa = 0, root_pa = 0;
    void *st, *ct, *root;
    uint64_t *ste;

    if (!d || !s || !p || !s->enabled || !p->alloc_dma_pages ||
        !p->map_mmio || stream_id >= 256u)
        return KIRA_SMMU_INVALID;

    /* 256 linear STEs occupy 256 * 64 = 16 KiB, not one page. */
    st = smmu_alloc_pages(p, SMMU_STRTAB_PAGES, &st_pa);
    ct = smmu_alloc(p, &ct_pa);
    root = smmu_alloc(p, &root_pa);
    if (!st || !ct || !root) {
        if (p->free_dma_pages) {
            if (st) p->free_dma_pages(p->context, st, st_pa, SMMU_STRTAB_PAGES);
            if (ct) p->free_dma_pages(p->context, ct, ct_pa, 1);
            if (root) p->free_dma_pages(p->context, root, root_pa, 1);
        }
        return KIRA_SMMU_MEMORY_FAILED;
    }

    /*
     * Linear stream-table entry: STE word 0 valid/config, word 1 points
     * to context descriptor. The descriptor uses a 4-level stage-1 table.
     */
    ste = smmu_words(st) + stream_id * 8u;
    ste[0] = 1ull | (1ull << 1); /* valid, bypass disabled */
    ste[1] = ct_pa & SMMU_TTBR_ADDR;
    ste[2] = 0;
    ste[3] = 0;

    {
        uint64_t *cd = smmu_words(ct);
        cd[0] = 1ull | (1ull << 1); /* valid, stage-1 translation */
        cd[1] = root_pa & SMMU_TTBR_ADDR;
        cd[2] = 0;
        cd[3] = 0;
    }

    if (p->clean_cache) {
        p->clean_cache(p->context, st, SMMU_PAGE_SIZE);
        p->clean_cache(p->context, ct, SMMU_PAGE_SIZE);
        p->clean_cache(p->context, root, SMMU_PAGE_SIZE);
    }

    d->smmu = s;
    d->stream_table = st;
    d->stream_table_physical = st_pa;
    d->context_table = ct;
    d->context_table_physical = ct_pa;
    d->stage1_root = root;
    d->stage1_root_physical = root_pa;
    d->stream_id = stream_id;
    d->active = 1;

    /*
     * Program linear stream table base and size. The SMMU must be configured
     * for a linear table by platform firmware or prior initialization.
     */
    *(volatile uint64_t *)(s->registers + SMMU_STRTAB_BASE) =
        (st_pa & ~0xfffull) | 1ull;
    *(volatile uint32_t *)(s->registers + SMMU_STRTAB_BASE_CFG) =
        8u; /* 2^8 stream entries */

    if (p->write_barrier)
        p->write_barrier(p->context);

    return smmu_invalidate_stream(d, p);
}

int kira_smmu_domain_map_4k(struct kira_smmu_domain *d,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_smmu_platform *p)
{
    int rc;

    if (!d || !d->active || !p || (iova & 0xfffu) ||
        (physical & 0xfffu))
        return KIRA_SMMU_INVALID;

    rc = smmu_map_page(d, iova, physical, permissions, p);
    if (rc != KIRA_SMMU_OK)
        return rc;

    if (p->write_barrier)
        p->write_barrier(p->context);

    /* Invalidate the stream's translation cache before DMA is resumed. */
    return smmu_invalidate_stream(d, p);
}

int kira_smmu_domain_unmap_4k(struct kira_smmu_domain *d,
                              uint64_t iova,
                              const struct kira_smmu_platform *p)
{
    unsigned int shifts[4] = {39, 30, 21, 12};
    uint64_t *table;
    unsigned int level;

    if (!d || !d->active || !p || (iova & 0xfffu))
        return KIRA_SMMU_INVALID;

    table = smmu_words(d->stage1_root);
    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t e = table[index];
        if (!(e & 1ull))
            return KIRA_SMMU_HARDWARE_ERROR;
        table = (uint64_t *)smmu_table_from_entry(p, e);
        if (!table)
            return KIRA_SMMU_MEMORY_FAILED;
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        if (!(table[index] & 1ull))
            return KIRA_SMMU_HARDWARE_ERROR;
        table[index] = 0;
        if (p->clean_cache)
            p->clean_cache(p->context, table, SMMU_PAGE_SIZE);
    }

    if (p->write_barrier)
        p->write_barrier(p->context);
    return smmu_invalidate_stream(d, p);
}

int kira_smmu_domain_destroy(struct kira_smmu_domain *d,
                             const struct kira_smmu_platform *p)
{
    int rc;

    if (!d || !d->active || !p)
        return KIRA_SMMU_INVALID;

    /* Invalidate the STE before freeing any translation structures. */
    smmu_words(d->stream_table)[d->stream_id * 8u] = 0;
    if (p->clean_cache)
        p->clean_cache(p->context, d->stream_table, SMMU_PAGE_SIZE);
    if (p->write_barrier)
        p->write_barrier(p->context);

    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK)
        return rc;

    if (p->free_dma_pages) {
        p->free_dma_pages(p->context, d->stage1_root,
                          d->stage1_root_physical, 1);
        p->free_dma_pages(p->context, d->context_table,
                          d->context_table_physical, 1);
        p->free_dma_pages(p->context, d->stream_table,
                          d->stream_table_physical, SMMU_STRTAB_PAGES);
    }

    d->active = 0;
    d->stage1_root = 0;
    d->context_table = 0;
    d->stream_table = 0;
    return KIRA_SMMU_OK;
}
