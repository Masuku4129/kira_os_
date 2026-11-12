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
#define SMMU_TTBR_ADDR 0x000ffffffffff000ull

/* SMMUv3 command opcodes. */
#define CMD_CFGI_STE 0x03u
#define CMD_CFGI_ALL 0x04u
#define CMD_TLBI_NH_ASID 0x11u
#define CMD_SYNC 0x46u

static uint64_t *smmu_words(void *p) { return (uint64_t *)p; }

/* Serialize shared stream-table setup and StreamID ownership. */
static void smmu_lock(struct kira_smmu *s)
{
    while (__atomic_exchange_n(&s->stream_table_lock, 1u, __ATOMIC_ACQUIRE)) {
        /* Short controller-local critical section; no allocation or I/O wait. */
    }
}

static void smmu_unlock(struct kira_smmu *s)
{
    __atomic_store_n(&s->stream_table_lock, 0u, __ATOMIC_RELEASE);
}

static int smmu_stream_id_claimed(const struct kira_smmu *s, uint32_t id)
{
    return (s->stream_ids[id >> 5] & (1u << (id & 31u))) != 0;
}

static void smmu_stream_id_set(struct kira_smmu *s, uint32_t id)
{
    s->stream_ids[id >> 5] |= 1u << (id & 31u);
}

static void smmu_stream_id_clear(struct kira_smmu *s, uint32_t id)
{
    s->stream_ids[id >> 5] &= ~(1u << (id & 31u));
}

static void smmu_zero(void *p, size_t bytes)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    size_t i;
    for (i = 0; i < bytes; ++i)
        b[i] = 0;
}

static int smmu_address_fits(uint8_t width, uint64_t base, size_t bytes)
{
    uint64_t maximum;
    if (!bytes || width < 32 || width > 64 ||
        base > UINT64_MAX - (uint64_t)(bytes - 1u))
        return 0;
    maximum = width == 64 ? UINT64_MAX : ((1ull << width) - 1ull);
    return base <= maximum && (uint64_t)(bytes - 1u) <= maximum - base;
}

static void *smmu_alloc_pages(const struct kira_smmu_platform *p,
                              uint8_t address_width, size_t pages, uint64_t *pa)
{
    void *v = p->alloc_dma_pages(p->context, pages, pa);
    size_t bytes = pages * SMMU_PAGE_SIZE;
    if (!v || !*pa || (*pa & 0xfffu) ||
        !smmu_address_fits(address_width, *pa, bytes) ||
        (*pa & ~SMMU_TTBR_ADDR) != 0) {
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

static void *smmu_alloc(const struct kira_smmu_platform *p,
                        uint8_t address_width, uint64_t *pa)
{
    return smmu_alloc_pages(p, address_width, 1, pa);
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

static void smmu_cmdq_lock(struct kira_smmu *s)
{
    while (__atomic_exchange_n(&s->cmdq_lock, 1u, __ATOMIC_ACQUIRE)) {
        /* Keep command publication and its completion fence indivisible. */
    }
}

static void smmu_cmdq_unlock(struct kira_smmu *s)
{
    __atomic_store_n(&s->cmdq_lock, 0u, __ATOMIC_RELEASE);
}

static int smmu_invalidate_stream(struct kira_smmu_domain *d,
                                  const struct kira_smmu_platform *p)
{
    uint64_t command = (uint64_t)CMD_CFGI_STE |
                       ((uint64_t)d->stream_id << 32);
    int rc;

    /*
     * CFGI_STE and its CMD_SYNC form one ordered transaction. Without a
     * controller-wide lock, another producer could enqueue commands between
     * them and make the consumer pointer an invalid completion test.
     */
    smmu_cmdq_lock(d->smmu);
    rc = smmu_cmd_submit(d->smmu, p, command, 0, 0);
    if (rc == KIRA_SMMU_OK)
        rc = smmu_cmd_sync(d->smmu, p);
    smmu_cmdq_unlock(d->smmu);
    return rc;
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
            void *next = smmu_alloc(p, d->smmu->output_address_width, &pa);
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
    uint64_t ct_pa = 0, root_pa = 0;
    void *ct = 0, *root = 0;
    uint64_t *ste;
    int rc = KIRA_SMMU_OK;

    if (!d || !s || !p || !s->enabled || !p->alloc_dma_pages ||
        !p->free_dma_pages || !p->map_mmio || !s->stream_id_limit ||
        stream_id >= s->stream_id_limit)
        return KIRA_SMMU_INVALID;

    /* This domain uses stage 1; do not program it on a stage-2-only SMMU. */
    if (!(s->idr0 & (1u << 1)))
        return KIRA_SMMU_UNSUPPORTED;

    ct = smmu_alloc(p, s->output_address_width, &ct_pa);
    root = smmu_alloc(p, s->output_address_width, &root_pa);
    if (!ct || !root) {
        if (ct) p->free_dma_pages(p->context, ct, ct_pa, 1);
        if (root) p->free_dma_pages(p->context, root, root_pa, 1);
        return KIRA_SMMU_MEMORY_FAILED;
    }

    smmu_lock(s);
    if (smmu_stream_id_claimed(s, stream_id)) {
        rc = KIRA_SMMU_INVALID;
        goto fail_locked;
    }

    /*
     * The controller-global linear stream table is allocated and programmed
     * by kira_smmu_v3_prepare() before SMMUEN. Domains may only claim entries.
     */
    if (!s->stream_table || !s->stream_table_physical) {
        rc = KIRA_SMMU_INVALID;
        goto fail_locked;
    }

    ste = smmu_words(s->stream_table) + stream_id * 8u;
    if (ste[0] & 1ull) {
        rc = KIRA_SMMU_INVALID;
        goto fail_locked;
    }

    /* 64-byte STE: valid stage-1 entry points to this domain's CD. */
    ste[0] = 1ull | (1ull << 1);
    ste[1] = ct_pa & SMMU_TTBR_ADDR;
    ste[2] = 0;
    ste[3] = 0;
    if (p->clean_cache)
        p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    if (p->clean_cache)
        p->clean_cache(p->context, ct, SMMU_PAGE_SIZE);
    if (p->clean_cache)
        p->clean_cache(p->context, root, SMMU_PAGE_SIZE);
    if (p->write_barrier) p->write_barrier(p->context);

    smmu_stream_id_set(s, stream_id);
    d->smmu = s;
    d->stream_table = s->stream_table;
    d->context_table = ct;
    d->context_table_physical = ct_pa;
    d->stage1_root = root;
    d->stage1_root_physical = root_pa;
    d->stream_id = stream_id;
    d->active = 1;

    /* Invalidate the newly published STE before returning the domain. */
    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK) {
        /*
         * Hardware completion is unknown after a timeout/error. Keep the
         * STE, StreamID claim and translation pages pinned: freeing them
         * could leave the SMMU DMA-walking reclaimed memory. The caller may
         * retry kira_smmu_domain_destroy() to establish invalidation before
         * resources are released.
         */
        d->active = 1;
    }
    smmu_unlock(s);
    return rc;

fail_locked:
    smmu_unlock(s);
    p->free_dma_pages(p->context, root, root_pa, 1);
    p->free_dma_pages(p->context, ct, ct_pa, 1);
    return rc;
}

int kira_smmu_domain_map_4k(struct kira_smmu_domain *d,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_smmu_platform *p)
{
    int rc;

    if (!d || !d->active || !p || (iova & 0xfffu) ||
        (physical & 0xfffu) ||
        !smmu_address_fits(d->smmu->output_address_width, physical,
                            SMMU_PAGE_SIZE) ||
        (physical & ~SMMU_TTBR_ADDR))
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
    struct kira_smmu *s;
    uint64_t *ste;
    int rc;

    if (!d || !d->active || !d->smmu || !p || !p->free_dma_pages)
        return KIRA_SMMU_INVALID;

    s = d->smmu;
    smmu_lock(s);
    if (!s->stream_table || !smmu_stream_id_claimed(s, d->stream_id)) {
        smmu_unlock(s);
        return KIRA_SMMU_INVALID;
    }

    /* Clear only this domain's STE; the shared table remains controller-owned. */
    ste = smmu_words(s->stream_table) + d->stream_id * 8u;
    ste[0] = 0;
    if (p->clean_cache)
        p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    if (p->write_barrier) p->write_barrier(p->context);
    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK) {
        /* Keep the ID claimed and page tables allocated on uncertain hardware state. */
        smmu_unlock(s);
        return rc;
    }

    smmu_stream_id_clear(s, d->stream_id);
    smmu_unlock(s);

    p->free_dma_pages(p->context, d->stage1_root,
                      d->stage1_root_physical, 1);
    p->free_dma_pages(p->context, d->context_table,
                      d->context_table_physical, 1);

    d->active = 0;
    d->stage1_root = 0;
    d->context_table = 0;
    d->stream_table = 0;
    d->smmu = 0;
    return KIRA_SMMU_OK;
}
