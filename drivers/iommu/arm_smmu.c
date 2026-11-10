/*
 * ARM SMMUv3 command/event queue and global enable path.
 *
 * The implementation establishes the hardware queues and global control
 * state. It intentionally does not install stream-table entries or attach
 * devices: until a domain/context descriptor is explicitly programmed,
 * callers must keep DMA-capable devices quiesced.
 */

#include "arm_smmu.h"
#include "arm_smmu_domain.h"

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
#define SMMU_EVTQ_PROD       0x00a8u
#define SMMU_EVTQ_CONS       0x00acu
#define SMMU_EVTQ_OVACK      (1u << 31)
#define SMMU_IDR0_ST_LVL_SHIFT 27u
#define SMMU_IDR0_ST_LVL_MASK  3u
#define SMMU_EVTQ_OVF         (1u << 31)
#define SMMU_EVTQ_PTR_MASK    0x1ffu

#define SMMU_CR0_SMMUEN      (1u << 0)
#define SMMU_CR0_EVENTQEN    (1u << 2)
#define SMMU_CR0_CMDQEN      (1u << 3)
#define SMMU_CR0_PRIQEN      (1u << 4)
#define SMMU_CR0ACK_SMMUEN   (1u << 0)
#define SMMU_CR0ACK_EVENTQEN (1u << 2)
#define SMMU_CR0ACK_CMDQEN   (1u << 3)
#define SMMU_GBPA_UPDATE     (1u << 31)
#define SMMU_GBPA_ABORT      (1u << 20)
#define SMMU_CR1_CONFIG      ((3u << 10) | (1u << 8) | (1u << 6) | \
                              (3u << 4) | (1u << 2) | 1u)
#define SMMU_CR2_CONFIG      ((1u << 2) | (1u << 1))

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

static int smmu_update_gbpa(struct kira_smmu *smmu,
                            const struct kira_smmu_platform *platform,
                            uint32_t set, uint32_t clear)
{
    uint32_t value;
    int rc;

    rc = smmu_wait_mask(smmu, SMMU_GBPA, SMMU_GBPA_UPDATE, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;

    value = smmu_read32(smmu->registers, SMMU_GBPA);
    value &= ~clear;
    value |= set;
    smmu_write32(smmu->registers, SMMU_GBPA, value | SMMU_GBPA_UPDATE);
    platform->write_barrier(platform->context);

    rc = smmu_wait_mask(smmu, SMMU_GBPA, SMMU_GBPA_UPDATE, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;
    return (smmu_read32(smmu->registers, SMMU_GBPA) & set) == set ?
           KIRA_SMMU_OK : KIRA_SMMU_HARDWARE_ERROR;
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
        ((uintptr_t)*allocation & (SMMU_PAGE_SIZE - 1u)) != 0 ||
        (*allocation_physical & (SMMU_PAGE_SIZE - 1u)) != 0 ||
        *allocation_physical > UINT64_MAX - (alignment - 1u) ||
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
    if ((aligned_physical & ~SMMU_QUEUE_BASE_ADDR_MASK) != 0 ||
        offset + queue_bytes > allocation_pages * SMMU_PAGE_SIZE) {
        platform->free_dma_pages(platform->context, *allocation,
                                 *allocation_physical, allocation_pages);
        *allocation = 0;
        *allocation_physical = 0;
        *allocated_pages = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }

    *queue_virtual = (uint8_t *)*allocation + offset;
    *queue_physical = aligned_physical;
    if (((uintptr_t)*queue_virtual & (alignment - 1u)) != 0) {
        platform->free_dma_pages(platform->context, *allocation,
                                 *allocation_physical, allocation_pages);
        *allocation = 0;
        *allocation_physical = 0;
        *allocated_pages = 0;
        *queue_virtual = 0;
        *queue_physical = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }
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
        !platform->clean_cache || !platform->write_barrier ||
        !platform->read_barrier || !platform->quiesce_devices ||
        !platform->report_event || !platform->invalidate_cache ||
        !physical_base || (physical_base & 0xfffu) ||
        physical_base > UINT64_MAX - SMMU_REGISTER_MAP_SIZE)
        return KIRA_SMMU_INVALID;

    if (smmu->enabled || smmu->stream_table || smmu->stream_table_allocation ||
        smmu->cmdq || smmu->evtq || smmu->evtq_allocation)
        return KIRA_SMMU_INVALID;

    /* Stop every PCI bus master before probing or changing SMMU state. If any
       device cannot be quiesced, leave the controller untouched. */
    if (platform->quiesce_devices(platform->context) != 0)
        return KIRA_SMMU_HARDWARE_ERROR;

    smmu->registers = (volatile uint8_t *)
        platform->map_mmio(platform->context, physical_base, SMMU_REGISTER_MAP_SIZE);
    if (!smmu->registers ||
        ((uintptr_t)smmu->registers & (SMMU_PAGE_SIZE - 1u)) != 0) {
        smmu->registers = 0;
        return KIRA_SMMU_MAP_FAILED;
    }

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

    /* The installed table is linear; two-level-only and reserved ST_LVL
       encodings must be rejected before programming STRTAB_BASE_CFG. */
    if (((smmu->idr0 >> SMMU_IDR0_ST_LVL_SHIFT) &
         SMMU_IDR0_ST_LVL_MASK) >= 2u)
        return KIRA_SMMU_UNSUPPORTED;

    /* SMMUv3 identifies itself through the architecture ID registers. */
    if ((smmu->idr0 & 0x3u) == 0)
        return KIRA_SMMU_UNSUPPORTED;

    /* Do not take over a live firmware-owned SMMU or destroy its domains. */
    if (smmu_read32(smmu->registers, SMMU_CR0ACK) &
        (SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN | SMMU_CR0ACK_CMDQEN))
        return KIRA_SMMU_UNSUPPORTED;

    /*
     * The device set is already quiesced. The GBPA abort policy below keeps
     * bypass closed while translation is disabled and during later failures.
     */
    smmu_write32(smmu->registers, SMMU_CR0, 0);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc != KIRA_SMMU_OK)
        return rc;

    /* Keep global bypass closed even if any later allocation step fails. */
    rc = smmu_update_gbpa(smmu, platform, SMMU_GBPA_ABORT, 0);
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
    smmu->evtq_lock = 0;
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
        ((uintptr_t)smmu->stream_table_allocation & (SMMU_PAGE_SIZE - 1u)) ||
        (smmu->stream_table_allocation_physical & (SMMU_PAGE_SIZE - 1u)) ||
        smmu->stream_table_allocation_physical >
            UINT64_MAX - (SMMU_STRTAB_ALIGN - 1u) ||
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
        if ((aligned_physical & ~SMMU_STRTAB_BASE_ADDR_MASK) != 0 ||
            offset + SMMU_STRTAB_BYTES >
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
        if (((uintptr_t)smmu->stream_table & (SMMU_STRTAB_ALIGN - 1u)) != 0) {
            platform->free_dma_pages(platform->context,
                                     smmu->stream_table_allocation,
                                     smmu->stream_table_allocation_physical,
                                     smmu->stream_table_allocation_pages);
            smmu->stream_table = 0;
            smmu->stream_table_physical = 0;
            smmu->stream_table_allocation = 0;
            smmu->stream_table_allocation_physical = 0;
            smmu->stream_table_allocation_pages = 0;
            return KIRA_SMMU_MEMORY_FAILED;
        }
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
    platform->write_barrier(platform->context);

    return KIRA_SMMU_OK;
}

static void smmu_stream_table_lock(struct kira_smmu *smmu)
{
    while (__atomic_exchange_n(&smmu->stream_table_lock, 1u, __ATOMIC_ACQUIRE)) {
    }
}

static void smmu_stream_table_unlock(struct kira_smmu *smmu)
{
    __atomic_store_n(&smmu->stream_table_lock, 0u, __ATOMIC_RELEASE);
}

int kira_smmu_v3_enable(struct kira_smmu *smmu,
                        const struct kira_smmu_platform *platform)
{
    uint64_t queue_base;
    uint32_t control = 0;
    int rc;

    if (!smmu || !platform || !smmu->registers ||
        !smmu->cmdq || !smmu->evtq || !platform->clean_cache ||
        !platform->write_barrier || !platform->read_barrier ||
        !platform->quiesce_devices || !platform->report_event ||
        !platform->invalidate_cache)
        return KIRA_SMMU_INVALID;

    smmu_stream_table_lock(smmu);
    if (smmu->enabled == 1u) {
        smmu_stream_table_unlock(smmu);
        return KIRA_SMMU_OK;
    }
    if (smmu->enabled != 0u) {
        smmu_stream_table_unlock(smmu);
        return KIRA_SMMU_HARDWARE_ERROR;
    }

    smmu->enabled = 2u; /* Block domain creation until every enable step commits. */
    while (__atomic_exchange_n(&smmu->evtq_lock, 1u, __ATOMIC_ACQUIRE)) {
        if (platform->delay)
            platform->delay(platform->context);
    }

    if (platform->quiesce_devices(platform->context) != 0) {
        smmu->enabled = 0u;
        rc = KIRA_SMMU_HARDWARE_ERROR;
        goto out;
    }
    if ((smmu_read32(smmu->registers, SMMU_GBPA) & SMMU_GBPA_ABORT) == 0) {
        rc = KIRA_SMMU_HARDWARE_ERROR;
        goto failed_state;
    }

    /* Disable first and prove the controller is idle before reprogramming. */
    smmu_write32(smmu->registers, SMMU_CR0, 0);
    platform->write_barrier(platform->context);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc != KIRA_SMMU_OK)
        goto failed_state;

    /* Table and queue fetches are Normal WB, Inner Shareable. */
    smmu_write32(smmu->registers, SMMU_CR1, SMMU_CR1_CONFIG);
    smmu_write32(smmu->registers, SMMU_CR2, SMMU_CR2_CONFIG);

    queue_base = (smmu->cmdq_physical & SMMU_QUEUE_BASE_ADDR_MASK) |
                 SMMU_QUEUE_LOG2;
    smmu_write64(smmu->registers, SMMU_CMDQ_BASE, queue_base);
    smmu->cmdq_producer = 0;
    smmu_write32(smmu->registers, SMMU_CMDQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_CMDQ_CONS, 0);

    queue_base = (smmu->evtq_physical & SMMU_QUEUE_BASE_ADDR_MASK) |
                 SMMU_QUEUE_LOG2;
    smmu_write64(smmu->registers, SMMU_EVTQ_BASE, queue_base);
    smmu->evtq_consumer = 0;
    smmu_write32(smmu->registers, SMMU_EVTQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_EVTQ_CONS, 0);

    platform->clean_cache(platform->context, smmu->cmdq, SMMU_CMDQ_BYTES);
    platform->clean_cache(platform->context, smmu->evtq, SMMU_EVTQ_BYTES);
    platform->write_barrier(platform->context);

    control = SMMU_CR0_CMDQEN;
    smmu_write32(smmu->registers, SMMU_CR0, control);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK, SMMU_CR0ACK_CMDQEN,
                        SMMU_CR0ACK_CMDQEN, platform);
    if (rc != KIRA_SMMU_OK)
        goto failed_state;

    /* Mandatory reset-time configuration and non-secure TLB invalidation. */
    rc = kira_smmu_v3_invalidate_all(smmu, platform);
    if (rc != KIRA_SMMU_OK)
        goto failed_state;

    control |= SMMU_CR0_EVENTQEN;
    smmu_write32(smmu->registers, SMMU_CR0, control);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN, platform);
    if (rc != KIRA_SMMU_OK)
        goto failed_state;

    control |= SMMU_CR0_SMMUEN;
    smmu_write32(smmu->registers, SMMU_CR0, control);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_SMMUEN,
                        SMMU_CR0ACK_CMDQEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_SMMUEN, platform);
    if (rc != KIRA_SMMU_OK)
        goto failed_state;

    smmu->enabled = 1u;
    rc = KIRA_SMMU_OK;
    goto out;

failed_state:
    /* Leave bypass-abort asserted; if shutdown cannot be confirmed, state=2
       prevents domains from being attached to a controller of unknown state. */
    smmu_write32(smmu->registers, SMMU_CR0, 0);
    platform->write_barrier(platform->context);
    if (smmu_wait_mask(smmu, SMMU_CR0ACK,
                       SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                       SMMU_CR0ACK_CMDQEN, 0, platform) == KIRA_SMMU_OK)
        smmu->enabled = 0u;
    else
        smmu->enabled = 2u;
out:
    __atomic_store_n(&smmu->evtq_lock, 0u, __ATOMIC_RELEASE);
    smmu_stream_table_unlock(smmu);
    return rc;
}

static uint32_t smmu_evtq_next(uint32_t pointer)
{
    uint32_t index = pointer & 0xffu;
    uint32_t next = (pointer & 0x100u) | ((index + 1u) & 0xffu);
    if (index == 0xffu)
        next ^= 0x100u;
    return next;
}

int kira_smmu_v3_poll_events(struct kira_smmu *smmu,
                             const struct kira_smmu_platform *platform,
                             uint32_t max_events, uint32_t *processed)
{
    uint32_t producer_raw, producer, consumer, count = 0;
    int overflow;

    if (!smmu || smmu->enabled != 1u || !smmu->registers ||
        !smmu->evtq || !platform || !platform->read_barrier ||
        !platform->write_barrier || !platform->invalidate_cache ||
        !platform->report_event || !processed || max_events == 0)
        return KIRA_SMMU_INVALID;

    *processed = 0;
    while (__atomic_exchange_n(&smmu->evtq_lock, 1u, __ATOMIC_ACQUIRE)) {
        if (platform->delay)
            platform->delay(platform->context);
    }
    if (smmu->enabled != 1u) {
        __atomic_store_n(&smmu->evtq_lock, 0u, __ATOMIC_RELEASE);
        return KIRA_SMMU_INVALID;
    }

    platform->read_barrier(platform->context);
    producer_raw = smmu_read32(smmu->registers, SMMU_EVTQ_PROD);
    overflow = (producer_raw & SMMU_EVTQ_OVF) != 0;
    producer = producer_raw & SMMU_EVTQ_PTR_MASK;
    consumer = smmu->evtq_consumer & SMMU_EVTQ_PTR_MASK;

    while (consumer != producer && count < max_events) {
        uint64_t event[4];
        volatile uint64_t *entry =
            (volatile uint64_t *)((uint8_t *)smmu->evtq +
                                  (consumer & 0xffu) * 32u);
        unsigned int i;

        platform->invalidate_cache(platform->context, (const void *)entry, 32u);
        platform->read_barrier(platform->context);
        for (i = 0; i < 4u; ++i)
            event[i] = entry[i];
        platform->report_event(platform->context, event);
        consumer = smmu_evtq_next(consumer);
        ++count;
    }

    /* Do not acknowledge or discard entries that did not fit this batch. */
    if (consumer != producer) {
        *processed = count;
        __atomic_store_n(&smmu->evtq_lock, 0u, __ATOMIC_RELEASE);
        return KIRA_SMMU_HARDWARE_ERROR;
    }

    smmu->evtq_consumer = consumer;
    *processed = count;
    platform->write_barrier(platform->context);
    smmu_write32(smmu->registers, SMMU_EVTQ_CONS,
                 consumer | (overflow ? SMMU_EVTQ_OVACK : 0u));
    __atomic_store_n(&smmu->evtq_lock, 0u, __ATOMIC_RELEASE);
    return overflow ? KIRA_SMMU_HARDWARE_ERROR : KIRA_SMMU_OK;
}

int kira_smmu_v3_disable(struct kira_smmu *smmu,
                         const struct kira_smmu_platform *platform)
{
    unsigned int i;
    uint8_t prior_state;
    int rc = KIRA_SMMU_OK;

    if (!smmu || !platform || !smmu->registers ||
        !platform->read_barrier || !platform->write_barrier ||
        !platform->quiesce_devices || !platform->report_event ||
        !platform->invalidate_cache)
        return KIRA_SMMU_INVALID;

    smmu_stream_table_lock(smmu);
    prior_state = smmu->enabled;
    /* Never turn translation off while any StreamID still owns a domain. */
    for (i = 0; i < 8u; ++i) {
        if (smmu->stream_ids[i] != 0) {
            smmu_stream_table_unlock(smmu);
            return KIRA_SMMU_INVALID;
        }
    }

    if (smmu->enabled == 0u) {
        smmu_stream_table_unlock(smmu);
        return KIRA_SMMU_OK;
    }
    if (smmu->enabled == 1u) {
        uint32_t processed;
        do {
            rc = kira_smmu_v3_poll_events(smmu, platform,
                                          SMMU_QUEUE_ENTRIES, &processed);
            if (rc != KIRA_SMMU_OK) {
                smmu_stream_table_unlock(smmu);
                return rc;
            }
        } while (processed == SMMU_QUEUE_ENTRIES);
    } else if (smmu->enabled != 2u) {
        smmu_stream_table_unlock(smmu);
        return KIRA_SMMU_HARDWARE_ERROR;
    }

    /* Prevent new event consumers and new domains while quiescing hardware. */
    smmu->enabled = 2u;
    while (__atomic_exchange_n(&smmu->evtq_lock, 1u, __ATOMIC_ACQUIRE)) {
        if (platform->delay)
            platform->delay(platform->context);
    }

    if (platform->quiesce_devices(platform->context) != 0) {
        /* Translation is still active; permit retry, but no domain changes
           have been made and no controller register has been disabled. */
        smmu->enabled = prior_state == 1u ? 1u : 2u;
        rc = KIRA_SMMU_HARDWARE_ERROR;
        goto out;
    }

    rc = smmu_update_gbpa(smmu, platform, SMMU_GBPA_ABORT, 0);
    if (rc != KIRA_SMMU_OK)
        goto out; /* state 2 keeps new domains blocked */

    smmu_write32(smmu->registers, SMMU_CR0, 0);
    platform->write_barrier(platform->context);
    rc = smmu_wait_mask(smmu, SMMU_CR0ACK,
                        SMMU_CR0ACK_SMMUEN | SMMU_CR0ACK_EVENTQEN |
                        SMMU_CR0ACK_CMDQEN, 0, platform);
    if (rc == KIRA_SMMU_OK)
        smmu->enabled = 0u;
    /* On timeout, keep state 2 and the global abort policy asserted. */

out:
    __atomic_store_n(&smmu->evtq_lock, 0u, __ATOMIC_RELEASE);
    smmu_stream_table_unlock(smmu);
    return rc;
}
