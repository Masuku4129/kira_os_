/*
 * ARM SMMUv3 command/event queue and global enable path.
 *
 * The implementation establishes the hardware queues and global control
 * state. It intentionally does not install stream-table entries or attach
 * devices: until a domain/context descriptor is explicitly programmed,
 * callers must keep DMA-capable devices quiesced.
 */

#include "arm_smmu.h"

#define SMMU_PAGE_SIZE       4096u
#define SMMU_QUEUE_ENTRIES   256u
#define SMMU_CMDQ_BYTES      (SMMU_QUEUE_ENTRIES * 16u)
#define SMMU_EVTQ_BYTES      (SMMU_QUEUE_ENTRIES * 32u)
#define SMMU_EVTQ_ALIGN      SMMU_EVTQ_BYTES
#define SMMU_EVTQ_ALLOC_PAGES ((SMMU_EVTQ_BYTES / SMMU_PAGE_SIZE) + (SMMU_EVTQ_ALIGN / SMMU_PAGE_SIZE) - 1u)
#define SMMU_REGISTER_MAP_SIZE (2u * SMMU_PAGE_SIZE)
#define SMMU_STRTAB_ENTRIES  256u
#define SMMU_STRTAB_BYTES    (SMMU_STRTAB_ENTRIES * 64u)
#define SMMU_STRTAB_PAGES    (SMMU_STRTAB_BYTES / SMMU_PAGE_SIZE)
#define SMMU_STRTAB_ALIGN    SMMU_STRTAB_BYTES
#define SMMU_STRTAB_ALLOC_PAGES (SMMU_STRTAB_PAGES + (SMMU_STRTAB_ALIGN / SMMU_PAGE_SIZE) - 1u)
#define SMMU_POLL_LIMIT      1000000u

#define SMMU_IDR0            0x0000u
#define SMMU_IDR0_ST_LVL_SHIFT 27u
#define SMMU_IDR0_ST_LVL_MASK  3u
#define SMMU_IDR1            0x0004u
#define SMMU_IDR1_SIDSIZE_MASK 0x3fu
#define SMMU_IDR5            0x0014u
#define SMMU_CR0             0x0020u
#define SMMU_CR0ACK          0x0024u
#define SMMU_CR1             0x0028u
#define SMMU_CR2             0x002cu
#define SMMU_GBPA            0x0044u
#define SMMU_IRQ_CTRL        0x0050u
#define SMMU_IRQ_CTRLACK     0x0054u
#define SMMU_GERROR          0x0060u
#define SMMU_GERRORN         0x0064u
#define SMMU_STRTAB_BASE     0x0080u
#define SMMU_STRTAB_BASE_CFG 0x0088u
#define SMMU_CMDQ_BASE       0x0090u
#define SMMU_CMDQ_PROD       0x0098u
#define SMMU_CMDQ_CONS       0x009cu
#define SMMU_EVTQ_BASE       0x00a0u
#define SMMU_EVTQ_PROD       0x10a8u
#define SMMU_EVTQ_CONS       0x10acu

#define SMMU_CR0_SMMUEN      (1u << 0)
#define SMMU_CR0_EVENTQEN    (1u << 2)
#define SMMU_CR0_CMDQEN      (1u << 3)
#define SMMU_CR0_PRIQEN      (1u << 4)
#define SMMU_CR0ACK_SMMUEN   (1u << 0)
#define SMMU_CR0ACK_EVENTQEN (1u << 2)
#define SMMU_CR0ACK_CMDQEN   (1u << 3)

#define SMMU_QUEUE_LOG2      8u
#define SMMU_QUEUE_BASE_ADDR_MASK 0x000fffffffffffe0ull
#define SMMU_STRTAB_BASE_ADDR_MASK 0x000fffffffffffc0ull
#define SMMU_IDR1_CMDQS_SHIFT 21u
#define SMMU_IDR1_EVENTQS_SHIFT 16u
#define SMMU_IDR1_QUEUE_SIZE_MASK 0x1fu
#define SMMU_IDR1_TABLES_PRESET (1u << 30)
#define SMMU_IDR1_QUEUES_PRESET (1u << 29)

static uint32_t smmu_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void smmu_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void smmu_write64(volatile uint8_t *base, uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static int smmu_wait_mask(struct kira_smmu *smmu, uint32_t offset,
                          uint32_t mask, uint32_t expected,
                          const struct kira_smmu_platform *platform)
{
    uint32_t i;
    for (i = 0; i < SMMU_POLL_LIMIT; ++i) {
        if (platform->read_barrier)
            platform->read_barrier(platform->context);
        if ((smmu_read32(smmu->registers, offset) & mask) == expected)
            return KIRA_SMMU_OK;
        if (platform->delay)
            platform->delay(platform->context);
    }
    return KIRA_SMMU_TIMEOUT;
}

static void smmu_zero_memory(void *memory, size_t bytes)
{
    volatile uint8_t *p = (volatile uint8_t *)memory;
    size_t i;
    for (i = 0; i < bytes; ++i)
        p[i] = 0;
}

static int smmu_alloc_queue(
    const struct kira_smmu_platform *platform,
    size_t allocation_pages, size_t queue_bytes, size_t alignment,
    void **allocation, uint64_t *allocation_physical,
    size_t *allocated_pages, void **queue_virtual,
    uint64_t *queue_physical)
{
    uint64_t aligned_physical;
    size_t offset;

    *allocation = platform->alloc_dma_pages(platform->context,
                                             allocation_pages,
                                             allocation_physical);
    *allocated_pages = allocation_pages;
    if (!*allocation || !*allocation_physical ||
        (*allocation_physical & (SMMU_PAGE_SIZE - 1u)) != 0 ||
        *allocation_physical >
            UINT64_MAX - (allocation_pages * SMMU_PAGE_SIZE)) {
        if (*allocation)
            platform->free_dma_pages(platform->context, *allocation,
                                     *allocation_physical, allocation_pages);
        *allocation = 0;
        *allocation_physical = 0;
        *allocated_pages = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }

    aligned_physical = (*allocation_physical + alignment - 1u) &
                       ~((uint64_t)alignment - 1u);
    offset = (size_t)(aligned_physical - *allocation_physical);
    if (offset + queue_bytes > allocation_pages * SMMU_PAGE_SIZE) {
        platform->free_dma_pages(platform->context, *allocation,
                                 *allocation_physical, allocation_pages);
        *allocation = 0;
        *allocation_physical = 0;
        *allocated_pages = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }

    *queue_virtual = (uint8_t *)*allocation + offset;
    *queue_physical = aligned_physical;
    smmu_zero_memory(*queue_virtual, queue_bytes);
    if (platform->clean_cache)
        platform->clean_cache(platform->context, *queue_virtual, queue_bytes);
    return KIRA_SMMU_OK;
}

int kira_smmu_v3_prepare(struct kira_smmu *smmu, uint64_t physical_base,
                         const struct kira_smmu_platform *platform)
{
    int rc;

    if (!smmu || !platform || !platform->map_mmio ||
        !platform->alloc_dma_pages || !platform->free_dma_pages ||
        !physical_base || (physical_base & 0xfffu))
        return KIRA_SMMU_INVALID;

    smmu->registers = (volatile uint8_t *)
        platform->map_mmio(platform->context, physical_base, SMMU_REGISTER_MAP_SIZE);
    if (!smmu->registers)
        return KIRA_SMMU_MAP_FAILED;

    smmu->physical_base = physical_base;
    smmu->idr0 = smmu_read32(smmu->registers, SMMU_IDR0);
    smmu->idr1 = smmu_read32(smmu->registers, SMMU_IDR1);
    smmu->idr5 = smmu_read32(smmu->registers, SMMU_IDR5);
    {
        uint32_t sid_bits = smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK;
        uint32_t cmdq_bits = (smmu->idr1 >> SMMU_IDR1_CMDQS_SHIFT) &
                             SMMU_IDR1_QUEUE_SIZE_MASK;
        uint32_t evtq_bits = (smmu->idr1 >> SMMU_IDR1_EVENTQS_SHIFT) &
                             SMMU_IDR1_QUEUE_SIZE_MASK;
        smmu->stream_id_limit = sid_bits >= 8u ? SMMU_STRTAB_ENTRIES :
                                (1u << sid_bits);
        /* This implementation uses 256-entry queues and programs their bases. */
        if ((smmu->idr1 & (SMMU_IDR1_TABLES_PRESET | SMMU_IDR1_QUEUES_PRESET)) ||
            cmdq_bits < SMMU_QUEUE_LOG2 || evtq_bits < SMMU_QUEUE_LOG2)
            return KIRA_SMMU_UNSUPPORTED;
    }

    /* This implementation programs a linear stream table. ST_LVL=2 requires
       a two-level table; ST_LVL=3 is reserved. Never write an incompatible format. */
    if (((smmu->idr0 >> SMMU_IDR0_ST_LVL_SHIFT) &
         SMMU_IDR0_ST_LVL_MASK) >= 2u)
        return KIRA_SMMU_UNSUPPORTED;

    /* SMMUv3 must expose at least stage 1 or stage 2 translation. */
    if ((smmu->idr0 & 0x3u) == 0)
        return KIRA_SMMU_UNSUPPORTED;

    /*
     * STRTAB_BASE/CFG are only writable while SMMUEN and its acknowledgement
     * are clear. Quiesce the controller before allocating/publishing tables.
     */
    smmu_write32(smmu->registers, SMMU_CR0, 0);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;

    smmu->queue_entries = SMMU_QUEUE_ENTRIES;
    smmu->stream_table = 0;
    smmu->stream_table_physical = 0;
    smmu->stream_table_allocation = 0;
    smmu->stream_table_allocation_physical = 0;
    smmu->stream_table_allocation_pages = 0;
    smmu->stream_table_lock = 0;
    smmu->cmdq_lock = 0;
    for (uint32_t i = 0; i < 8; ++i)
        smmu->stream_ids[i] = 0;
    /* Start from an unallocated queue state; every failure path can unwind. */
    smmu->cmdq = 0;
    smmu->cmdq_physical = 0;
    smmu->evtq = 0;
    smmu->evtq_physical = 0;
    smmu->evtq_allocation = 0;
    smmu->evtq_allocation_physical = 0;
    smmu->evtq_allocation_pages = 0;
    smmu->cmdq_producer = 0;
    smmu->evtq_consumer = 0;
    smmu->enabled = 0;

    /*
     * A linear table with LOG2SIZE=8 must be 16 KiB aligned. The DMA page
     * allocator guarantees only page alignment, so over-allocate and retain
     * the original allocation for the matching free operation.
     */
    smmu->stream_table_allocation =
        platform->alloc_dma_pages(platform->context, SMMU_STRTAB_ALLOC_PAGES,
                                  &smmu->stream_table_allocation_physical);
    smmu->stream_table_allocation_pages = SMMU_STRTAB_ALLOC_PAGES;
    if (!smmu->stream_table_allocation ||
        !smmu->stream_table_allocation_physical ||
        (smmu->stream_table_allocation_physical & (SMMU_PAGE_SIZE - 1u)) ||
        smmu->stream_table_allocation_physical >
            UINT64_MAX - (SMMU_STRTAB_ALLOC_PAGES * SMMU_PAGE_SIZE)) {
        if (smmu->stream_table_allocation)
            platform->free_dma_pages(platform->context,
                                     smmu->stream_table_allocation,
                                     smmu->stream_table_allocation_physical,
                                     SMMU_STRTAB_ALLOC_PAGES);
        smmu->stream_table_allocation = 0;
        smmu->stream_table_allocation_physical = 0;
        smmu->stream_table_allocation_pages = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }
    {
        uint64_t aligned_physical =
            (smmu->stream_table_allocation_physical + SMMU_STRTAB_ALIGN - 1u) &
            ~((uint64_t)SMMU_STRTAB_ALIGN - 1u);
        size_t offset = (size_t)(aligned_physical -
                                 smmu->stream_table_allocation_physical);
        if (offset + SMMU_STRTAB_BYTES >
            SMMU_STRTAB_ALLOC_PAGES * SMMU_PAGE_SIZE) {
            platform->free_dma_pages(platform->context,
                                     smmu->stream_table_allocation,
                                     smmu->stream_table_allocation_physical,
                                     SMMU_STRTAB_ALLOC_PAGES);
            smmu->stream_table_allocation = 0;
            smmu->stream_table_allocation_physical = 0;
            smmu->stream_table_allocation_pages = 0;
            return KIRA_SMMU_MEMORY_FAILED;
        }
        smmu->stream_table = (uint8_t *)smmu->stream_table_allocation + offset;
        smmu->stream_table_physical = aligned_physical;
    }
    smmu_zero_memory(smmu->stream_table, SMMU_STRTAB_BYTES);
    if (platform->clean_cache)
        platform->clean_cache(platform->context, smmu->stream_table,
                              SMMU_STRTAB_BYTES);

    {
        void *cmdq_allocation = 0;
        uint64_t cmdq_allocation_physical = 0;
        size_t cmdq_allocation_pages = 0;
        rc = smmu_alloc_queue(platform, 1u, SMMU_CMDQ_BYTES,
                              SMMU_PAGE_SIZE, &cmdq_allocation,
                              &cmdq_allocation_physical,
                              &cmdq_allocation_pages, &smmu->cmdq,
                              &smmu->cmdq_physical);
        if (rc == KIRA_SMMU_OK) {
            /* CMDQ alignment is one page, so the raw and visible bases match. */
            (void)cmdq_allocation_pages;
        }
    }
    if (rc != KIRA_SMMU_OK) {
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->stream_table_allocation,
                                     smmu->stream_table_allocation_physical,
                                     smmu->stream_table_allocation_pages);
        smmu->stream_table = 0;
        smmu->stream_table_physical = 0;
        smmu->stream_table_allocation = 0;
        smmu->stream_table_allocation_physical = 0;
        smmu->stream_table_allocation_pages = 0;
        return rc;
    }

    rc = smmu_alloc_queue(platform, SMMU_EVTQ_ALLOC_PAGES,
                          SMMU_EVTQ_BYTES, SMMU_EVTQ_ALIGN,
                          &smmu->evtq_allocation,
                          &smmu->evtq_allocation_physical,
                          &smmu->evtq_allocation_pages, &smmu->evtq,
                          &smmu->evtq_physical);
    if (rc != KIRA_SMMU_OK) {
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->cmdq,
                                     smmu->cmdq_physical, 1);
        smmu->cmdq = 0;
        smmu->cmdq_physical = 0;
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context,
                                     smmu->stream_table_allocation,
                                     smmu->stream_table_allocation_physical,
                                     smmu->stream_table_allocation_pages);
        smmu->stream_table = 0;
        smmu->stream_table_physical = 0;
        smmu->stream_table_allocation = 0;
        smmu->stream_table_allocation_physical = 0;
        smmu->stream_table_allocation_pages = 0;
        return rc;
    }

    /* Configure the controller-global linear stream table before SMMUEN. */
    smmu_write32(smmu->registers, SMMU_STRTAB_BASE_CFG,
                 (smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK) < 8u ?
                 (smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK) : 8u);
    smmu_write64(smmu->registers, SMMU_STRTAB_BASE,
                 (smmu->stream_table_physical & SMMU_STRTAB_BASE_ADDR_MASK));
    if (platform->write_barrier)
        platform->write_barrier(platform->context);

    return KIRA_SMMU_OK;
}

int kira_smmu_v3_enable(struct kira_smmu *smmu,
                        const struct kira_smmu_platform *platform)
{
    uint64_t queue_base;
    uint32_t control;
    int rc;

    if (!smmu || !platform || !smmu->registers ||
        !smmu->cmdq || !smmu->evtq)
        return KIRA_SMMU_INVALID;

    if (smmu->enabled)
        return KIRA_SMMU_OK;

    /* Disable the unit before changing queue base registers. */
    smmu_write32(smmu->registers, SMMU_CR0, 0);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;

    queue_base = (smmu->cmdq_physical & SMMU_QUEUE_BASE_ADDR_MASK) |
                 SMMU_QUEUE_LOG2;
    smmu_write64(smmu->registers, SMMU_CMDQ_BASE, queue_base);
    smmu_write32(smmu->registers, SMMU_CMDQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_CMDQ_CONS, 0);

    queue_base = (smmu->evtq_physical & SMMU_QUEUE_BASE_ADDR_MASK) |
                 SMMU_QUEUE_LOG2;
    smmu_write64(smmu->registers, SMMU_EVTQ_BASE, queue_base);
    smmu_write32(smmu->registers, SMMU_EVTQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_EVTQ_CONS, 0);

    if (platform->clean_cache) {
        platform->clean_cache(platform->context, smmu->cmdq,
                              SMMU_CMDQ_BYTES);
        platform->clean_cache(platform->context, smmu->evtq,
                              SMMU_EVTQ_BYTES);
    }
    if (platform->write_barrier)
        platform->write_barrier(platform->context);

    control = SMMU_CR0_CMDQEN | SMMU_CR0_EVENTQEN | SMMU_CR0_SMMUEN;
    smmu_write32(smmu->registers, SMMU_CR0, control);

    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_SMMUEN,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_SMMUEN, platform);
    if (rc != KIRA_SMMU_OK) {
        smmu_write32(smmu->registers, SMMU_CR0, 0);
        return rc;
    }

    smmu->enabled = 1;
    return KIRA_SMMU_OK;
}

int kira_smmu_v3_disable(struct kira_smmu *smmu,
                         const struct kira_smmu_platform *platform)
{
    int rc;

    if (!smmu || !platform || !smmu->registers)
        return KIRA_SMMU_INVALID;

    smmu_write32(smmu->registers, SMMU_CR0, 0);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;

    smmu->enabled = 0;
    return KIRA_SMMU_OK;
}
