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

/* ARM IHI 0070: SMMUv3 linear stream-table and AArch64 stage-1 CD fields. */
#define STE0_VALID              (1ull << 0)
#define STE0_CFG_S1_TRANS       (5ull << 1)
#define STE0_CFG_BYPASS         (4ull << 1)
#define STE0_S1CTXPTR_MASK      0x000fffffffffffc0ull
#define STE1_S1DSS_SSID0        2ull
#define STE1_S1CIR_WBRA         (1ull << 2)
#define STE1_S1COR_WBRA         (1ull << 4)
#define STE1_S1CSH_ISH          (3ull << 6)
#define CD0_T0SZ_48BIT          16ull
#define CD0_IRGN0_WBRA          (1ull << 8)
#define CD0_ORGN0_WBRA          (1ull << 10)
#define CD0_SH0_ISH             (3ull << 12)
#define CD0_EPD1                (1ull << 30)
#define CD0_VALID               (1ull << 31)
#define CD0_IPS_SHIFT           32u
#define CD0_AA64                (1ull << 41)
#define CD0_R                   (1ull << 45)
#define CD0_A                   (1ull << 46)
#define CD0_ASET                (1ull << 47)
#define CD1_TTB0_MASK           0x0000fffffffffff0ull

/* SMMUv3 command opcodes. */
#define CMD_CFGI_STE 0x03u
#define CMD_CFGI_CD 0x05u
#define CMD_TLBI_NH_ASID 0x11u
#define CMD_SYNC 0x46u
#define CMD_CFGI_LEAF 1ull
#define CMD_TLBI_ASID_SHIFT 48u

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

static void *smmu_alloc_pages(const struct kira_smmu_platform *p,
                              size_t pages, uint64_t *pa)
{
    void *v = p->alloc_dma_pages(p->context, pages, pa);
    size_t bytes = pages * SMMU_PAGE_SIZE;
    if (!v || !*pa || (*pa & 0xfffu) ||
        ((uintptr_t)v & (SMMU_PAGE_SIZE - 1u))) {
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

struct smmu_table_page {
    void *virtual_address;
    uint64_t physical_address;
    struct smmu_table_page *next;
};

static unsigned smmu_output_bits(const struct kira_smmu *s)
{
    static const unsigned bits[6] = {32, 36, 40, 42, 44, 48};
    unsigned encoding = s->idr5 & 7u;
    return bits[encoding <= 5u ? encoding : 5u];
}

static int smmu_physical_valid(const struct kira_smmu *s, uint64_t physical)
{
    return physical < (1ull << smmu_output_bits(s));
}

static void *smmu_table_page_alloc(struct kira_smmu_domain *d,
                                   const struct kira_smmu_platform *p,
                                   uint64_t *physical)
{
    struct smmu_table_page *record;
    void *memory = smmu_alloc(p, physical);

    if (!memory)
        return 0;
    if (!smmu_physical_valid(d->smmu, *physical)) {
        p->free_dma_pages(p->context, memory, *physical, 1);
        *physical = 0;
        return 0;
    }
    record = (struct smmu_table_page *)p->alloc_metadata(p->context,
                                                         sizeof(*record));
    if (!record) {
        p->free_dma_pages(p->context, memory, *physical, 1);
        *physical = 0;
        return 0;
    }
    record->virtual_address = memory;
    record->physical_address = *physical;
    record->next = (struct smmu_table_page *)d->table_pages;
    d->table_pages = record;
    return memory;
}

static void *smmu_table_page_find(struct kira_smmu_domain *d, uint64_t physical)
{
    struct smmu_table_page *page =
        (struct smmu_table_page *)d->table_pages;
    physical &= SMMU_TTBR_ADDR;
    while (page) {
        if (page->physical_address == physical)
            return page->virtual_address;
        page = page->next;
    }
    return 0;
}

static void smmu_table_page_list_free(struct smmu_table_page *page,
                                       const struct kira_smmu_platform *p)
{
    while (page) {
        struct smmu_table_page *next = page->next;
        p->free_dma_pages(p->context, page->virtual_address,
                          page->physical_address, 1);
        p->free_metadata(p->context, page);
        page = next;
    }
}

static void smmu_table_pages_free(struct kira_smmu_domain *d,
                                  const struct kira_smmu_platform *p)
{
    struct smmu_table_page *page =
        (struct smmu_table_page *)d->table_pages;
    d->table_pages = 0;
    smmu_table_page_list_free(page, p);
}

static void smmu_domain_lock(struct kira_smmu_domain *d)
{
    while (__atomic_exchange_n(&d->lock, 1u, __ATOMIC_ACQUIRE)) {
    }
}

static void smmu_domain_unlock(struct kira_smmu_domain *d)
{
    __atomic_store_n(&d->lock, 0u, __ATOMIC_RELEASE);
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
        if (p->read_barrier)
            p->read_barrier(p->context);
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
        uint32_t consumer;
        if (p->read_barrier)
            p->read_barrier(p->context);
        consumer = *(volatile uint32_t *)(s->registers + SMMU_CMDQ_CONS) &
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
    rc = smmu_cmd_submit(d->smmu, p, command, CMD_CFGI_LEAF, 0);
    if (rc == KIRA_SMMU_OK)
        rc = smmu_cmd_sync(d->smmu, p);
    smmu_cmdq_unlock(d->smmu);
    return rc;
}

int kira_smmu_domain_passthrough(struct kira_smmu_domain *d,
                                   struct kira_smmu *s,
                                   uint32_t stream_id,
                                   const struct kira_smmu_platform *p)
{
    uint64_t *ste;
    int rc;

    if (!d || !s || !p || !p->clean_cache || !p->write_barrier ||
        !p->read_barrier || !s->registers || !s->stream_table)
        return KIRA_SMMU_INVALID;

    smmu_domain_lock(d);
    if (s->enabled != 1u || !s->stream_id_limit ||
        stream_id >= s->stream_id_limit || stream_id >= 256u ||
        d->active || d->smmu || d->table_pages) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }

    smmu_lock(s);
    if (s->enabled != 1u || smmu_stream_id_claimed(s, stream_id)) {
        smmu_unlock(s);
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }
    ste = smmu_words(s->stream_table) + stream_id * 8u;
    if (ste[0] & STE0_VALID) {
        smmu_unlock(s);
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }

    d->smmu = s;
    d->stream_table = s->stream_table;
    d->stream_id = stream_id;
    d->active = 1u;
    d->bypass = 1u;

    /* Evict any cached invalid STE before publishing an explicit bypass STE. */
    ste[0] = 0;
    ste[1] = ste[2] = ste[3] = 0;
    p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    p->write_barrier(p->context);
    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK) {
        d->active = 0;
        d->bypass = 0;
        d->smmu = 0;
        d->stream_table = 0;
        smmu_unlock(s);
        smmu_domain_unlock(d);
        return rc;
    }

    ste[0] = STE0_VALID | STE0_CFG_BYPASS;
    p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    p->write_barrier(p->context);
    smmu_stream_id_set(s, stream_id);
    rc = smmu_invalidate_stream(d, p);
    /* On timeout retain the live bypass STE and StreamID claim for safe retry. */
    smmu_unlock(s);
    smmu_domain_unlock(d);
    return rc;
}

static int smmu_invalidate_cd(struct kira_smmu_domain *d,
                              const struct kira_smmu_platform *p)
{
    uint64_t command = (uint64_t)CMD_CFGI_CD |
                       ((uint64_t)d->stream_id << 32);
    int rc;

    smmu_cmdq_lock(d->smmu);
    rc = smmu_cmd_submit(d->smmu, p, command, CMD_CFGI_LEAF, 0);
    if (rc == KIRA_SMMU_OK)
        rc = smmu_cmd_sync(d->smmu, p);
    smmu_cmdq_unlock(d->smmu);
    return rc;
}

static int smmu_invalidate_asid(struct kira_smmu_domain *d,
                                const struct kira_smmu_platform *p)
{
    uint64_t command = (uint64_t)CMD_TLBI_NH_ASID |
                       ((uint64_t)d->asid << CMD_TLBI_ASID_SHIFT);
    int rc;

    smmu_cmdq_lock(d->smmu);
    rc = smmu_cmd_submit(d->smmu, p, command, 0, 0);
    if (rc == KIRA_SMMU_OK)
        rc = smmu_cmd_sync(d->smmu, p);
    smmu_cmdq_unlock(d->smmu);
    return rc;
}

static void *smmu_table_from_entry(struct kira_smmu_domain *d,
                                   uint64_t entry)
{
    if (!(entry & 1ull))
        return 0;
    return smmu_table_page_find(d, entry & SMMU_TTBR_ADDR);
}

static int smmu_map_page(struct kira_smmu_domain *d,
                         uint64_t iova, uint64_t physical,
                         uint32_t permissions,
                         const struct kira_smmu_platform *p)
{
    unsigned int shifts[4] = {39, 30, 21, 12};
    uint64_t *table = smmu_words(d->stage1_root);
    unsigned int level;

    if (iova >= (1ull << 48) || !smmu_physical_valid(d->smmu, physical) ||
        (permissions & ~(KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE)) ||
        !(permissions & KIRA_SMMU_MAP_READ) ||
        !(permissions & (KIRA_SMMU_MAP_READ | KIRA_SMMU_MAP_WRITE)))
        return KIRA_SMMU_INVALID;

    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t entry = table[index];

        if (!(entry & 1ull)) {
            uint64_t next_physical = 0;
            void *next = smmu_table_page_alloc(d, p, &next_physical);
            if (!next)
                return KIRA_SMMU_MEMORY_FAILED;
            table[index] = (next_physical & SMMU_TTBR_ADDR) | 3ull;
            p->clean_cache(p->context, table, SMMU_PAGE_SIZE);
            table = smmu_words(next);
        } else {
            table = (uint64_t *)smmu_table_from_entry(d, entry);
            if (!table)
                return KIRA_SMMU_HARDWARE_ERROR;
        }
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        uint64_t descriptor = (physical & SMMU_TTBR_ADDR) |
                              3ull | (3ull << 8) | (1ull << 10) |
                              (1ull << 11) | (1ull << 53) | (1ull << 54);
        /* PXN/UXN: DMA translations never permit instruction fetch. */
        if (table[index] & 1ull)
            return KIRA_SMMU_HARDWARE_ERROR;

        /* Stage-1 AP=10 is read-only at EL1; AP=00 is read/write.
           AArch64 stage 1 has no write-only permission encoding. */
        if (!(permissions & KIRA_SMMU_MAP_WRITE))
            descriptor |= 1ull << 7;
        table[index] = descriptor;
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

    if (!d || !s || !p)
        return KIRA_SMMU_INVALID;

    smmu_domain_lock(d);
    if (s->enabled != 1u || !s->registers || !s->cmdq ||
        !p->alloc_dma_pages || !p->free_dma_pages ||
        !p->alloc_metadata || !p->free_metadata || !s->stream_id_limit ||
        s->stream_id_limit > 256u || stream_id >= s->stream_id_limit ||
        stream_id >= 256u || d->active || d->smmu || d->table_pages) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }

    /* This domain uses AArch64 stage 1 with 4 KiB granules and <=48-bit PA. */
    if (!(s->idr0 & (1u << 1)) ||
        (((s->idr0 >> 2) & 3u) != 2u && ((s->idr0 >> 2) & 3u) != 3u) ||
        !(s->idr5 & (1u << 4)) || (s->idr5 & 7u) > 6u) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_UNSUPPORTED;
    }
    if (!p->clean_cache || !p->write_barrier || !p->read_barrier) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }
    if (!(s->idr0 & (1u << 12)) && stream_id >= 255u) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_UNSUPPORTED; /* Reserve ASID 0; use unique 8-bit ASIDs. */
    }

    d->smmu = s;
    d->table_pages = 0;
    ct = smmu_alloc(p, &ct_pa);
    root = smmu_table_page_alloc(d, p, &root_pa);
    if (!ct || !root || !smmu_physical_valid(s, ct_pa)) {
        if (ct) p->free_dma_pages(p->context, ct, ct_pa, 1);
        smmu_table_pages_free(d, p);
        d->smmu = 0;
        smmu_domain_unlock(d);
        return KIRA_SMMU_MEMORY_FAILED;
    }

    smmu_lock(s);
    if (s->enabled != 1u || smmu_stream_id_claimed(s, stream_id)) {
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

    /*
     * A linear context-descriptor table uses SSID 0 here (S1CDMAX=0).
     * CD0 configures a 48-bit AArch64 VA, 4 KiB granules, inner-shareable
     * WB tables, disables TTBR1 and selects the largest supported OAS up
     * to 48 bits. CD1 supplies the stage-1 root table.
     */
    {
        uint64_t oas = s->idr5 & 7u;
        uint64_t *cd = smmu_words(ct);
        if (oas > 5u) oas = 5u;
        d->asid = (uint16_t)(stream_id + 1u);
        cd[0] = CD0_T0SZ_48BIT | CD0_IRGN0_WBRA | CD0_ORGN0_WBRA |
                CD0_SH0_ISH | CD0_EPD1 | CD0_VALID |
                (oas << CD0_IPS_SHIFT) | CD0_AA64 | CD0_R | CD0_A | CD0_ASET |
                ((uint64_t)d->asid << 48);
        cd[1] = root_pa & CD1_TTB0_MASK;
        cd[3] = 0xffull; /* MAIR AttrIndx 0 = Normal, inner/outer WB memory. */
    }

    /*
     * SMMU can cache a previously invalid STE. First publish the non-valid
     * words and invalidate that cached state; only then publish V=1.
     */
    ste[0] = 0;
    ste[1] = STE1_S1DSS_SSID0 | STE1_S1CIR_WBRA |
             STE1_S1COR_WBRA | STE1_S1CSH_ISH;
    ste[2] = 0;
    ste[3] = 0;
    p->clean_cache(p->context, ct, SMMU_PAGE_SIZE);
    p->clean_cache(p->context, root, SMMU_PAGE_SIZE);
    p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    p->write_barrier(p->context);

    d->smmu = s;
    d->stream_table = s->stream_table;
    d->context_table = ct;
    d->context_table_physical = ct_pa;
    d->stage1_root = root;
    d->stage1_root_physical = root_pa;
    d->stream_id = stream_id;
    d->active = 1;
    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK) {
        d->active = 0;
        d->smmu = 0;
        d->stream_table = 0;
        d->context_table = 0;
        d->stage1_root = 0;
        d->stage1_root_physical = 0;
        smmu_unlock(s);
        smmu_table_pages_free(d, p);
        p->free_dma_pages(p->context, ct, ct_pa, 1);
        smmu_domain_unlock(d);
        return rc;
    }

    ste[0] = STE0_VALID | STE0_CFG_S1_TRANS |
             (ct_pa & STE0_S1CTXPTR_MASK);
    p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    p->write_barrier(p->context);
    smmu_stream_id_set(s, stream_id);

    rc = smmu_invalidate_stream(d, p);
    if (rc != KIRA_SMMU_OK) {
        /*
         * The live STE may already be cached. Keep every referenced page and
         * the StreamID claim pinned until a later destroy completes CFGI+SYNC.
         */
        d->active = 1;
    }
    smmu_unlock(s);
    smmu_domain_unlock(d);
    return rc;

fail_locked:
    smmu_unlock(s);
    smmu_table_pages_free(d, p);
    p->free_dma_pages(p->context, ct, ct_pa, 1);
    d->smmu = 0;
    d->stream_table = 0;
    d->context_table = 0;
    d->stage1_root = 0;
    d->stage1_root_physical = 0;
    smmu_domain_unlock(d);
    return rc;
}

int kira_smmu_domain_map_4k(struct kira_smmu_domain *d,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_smmu_platform *p)
{
    int rc;

    if (!d || !p || !p->clean_cache ||
        !p->write_barrier || !p->read_barrier ||
        (iova & 0xfffu) || (physical & 0xfffu))
        return KIRA_SMMU_INVALID;

    smmu_domain_lock(d);
    if (!d->active || d->bypass || !d->stage1_root || !d->context_table) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }
    rc = smmu_map_page(d, iova, physical, permissions, p);
    if (rc == KIRA_SMMU_OK) {
        p->write_barrier(p->context);
        /* Keep the mapping and its backing memory pinned if this times out. */
        rc = smmu_invalidate_asid(d, p);
    }
    smmu_domain_unlock(d);
    return rc;
}

int kira_smmu_domain_unmap_4k(struct kira_smmu_domain *d,
                              uint64_t iova,
                              const struct kira_smmu_platform *p)
{
    unsigned int shifts[4] = {39, 30, 21, 12};
    uint64_t *table;
    unsigned int level;
    int rc;

    if (!d || !p || !p->clean_cache ||
        !p->write_barrier || !p->read_barrier ||
        (iova & 0xfffu) || iova >= (1ull << 48))
        return KIRA_SMMU_INVALID;

    smmu_domain_lock(d);
    if (!d->active) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }
    table = smmu_words(d->stage1_root);
    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t e = table[index];
        if (!(e & 1ull)) {
            smmu_domain_unlock(d);
            return KIRA_SMMU_HARDWARE_ERROR;
        }
        table = (uint64_t *)smmu_table_from_entry(d, e);
        if (!table) {
            smmu_domain_unlock(d);
            return KIRA_SMMU_HARDWARE_ERROR;
        }
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        if (!(table[index] & 1ull)) {
            /* A prior unmap may have cleared the PTE but timed out waiting for
               TLBI completion. Retry the ASID invalidation before allowing the
               caller to release the backing page. */
            p->write_barrier(p->context);
            rc = smmu_invalidate_asid(d, p);
            smmu_domain_unlock(d);
            return rc;
        }
        table[index] = 0;
        if (p->clean_cache)
            p->clean_cache(p->context, table, SMMU_PAGE_SIZE);
    }

    p->write_barrier(p->context);
    /* The caller must not free the unmapped physical page until this succeeds. */
    rc = smmu_invalidate_asid(d, p);
    smmu_domain_unlock(d);
    return rc;
}

int kira_smmu_domain_destroy(struct kira_smmu_domain *d,
                             const struct kira_smmu_platform *p)
{
    struct kira_smmu *s;
    void *context_table;
    uint64_t context_table_physical;
    struct smmu_table_page *table_pages;
    uint64_t *ste;
    int rc;

    if (!d || !p || !p->free_dma_pages || !p->free_metadata ||
        !p->clean_cache || !p->write_barrier || !p->read_barrier)
        return KIRA_SMMU_INVALID;

    smmu_domain_lock(d);
    if (!d->active || !d->smmu) {
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }
    s = d->smmu;
    smmu_lock(s);
    if (!s->stream_table || !smmu_stream_id_claimed(s, d->stream_id)) {
        smmu_unlock(s);
        smmu_domain_unlock(d);
        return KIRA_SMMU_INVALID;
    }

    /* Clear only this domain's STE; the shared table remains controller-owned. */
    ste = smmu_words(s->stream_table) + d->stream_id * 8u;
    ste[0] = 0;
    p->clean_cache(p->context, s->stream_table, SMMU_STRTAB_BYTES);
    p->write_barrier(p->context);
    rc = smmu_invalidate_stream(d, p);
    if (rc == KIRA_SMMU_OK && !d->bypass)
        rc = smmu_invalidate_cd(d, p);
    if (rc == KIRA_SMMU_OK && !d->bypass)
        rc = smmu_invalidate_asid(d, p);
    if (rc != KIRA_SMMU_OK) {
        /* Keep the ID claimed and every translation page pinned on uncertainty. */
        smmu_unlock(s);
        smmu_domain_unlock(d);
        return rc;
    }

    smmu_stream_id_clear(s, d->stream_id);
    context_table = d->context_table;
    context_table_physical = d->context_table_physical;
    table_pages = (struct smmu_table_page *)d->table_pages;
    d->table_pages = 0;
    d->active = 0;
    d->smmu = 0;
    d->stream_table = 0;
    d->context_table = 0;
    d->context_table_physical = 0;
    d->asid = 0;
    d->bypass = 0;
    d->stage1_root = 0;
    d->stage1_root_physical = 0;
    smmu_unlock(s);
    smmu_domain_unlock(d);

    smmu_table_page_list_free(table_pages, p);
    if (context_table)
        p->free_dma_pages(p->context, context_table,
                          context_table_physical, 1);
    return KIRA_SMMU_OK;
}
