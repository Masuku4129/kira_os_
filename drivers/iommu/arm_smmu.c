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
#define SMMU_QUEUE_BYTES     (SMMU_QUEUE_ENTRIES * 16u)
#define SMMU_STRTAB_ENTRIES  256u
#define SMMU_STRTAB_BYTES    (SMMU_STRTAB_ENTRIES * 64u)
#define SMMU_STRTAB_PAGES    (SMMU_STRTAB_BYTES / SMMU_PAGE_SIZE)
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

#define SMMU_CR0_SMMUEN      (1u << 0)
#define SMMU_CR0_EVENTQEN    (1u << 2)
#define SMMU_CR0_CMDQEN      (1u << 3)
#define SMMU_CR0_PRIQEN      (1u << 4)
#define SMMU_CR0ACK_SMMUEN   (1u << 0)
#define SMMU_CR0ACK_EVENTQEN (1u << 2)
#define SMMU_CR0ACK_CMDQEN   (1u << 3)

#define SMMU_QUEUE_LOG2      8u
#define SMMU_QUEUE_BASE_VALID 1ull
#define SMMU_QUEUE_BASE_LOG2_SHIFT  q

static uint32_t smmu_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static uint64_t smmu_read64(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint64_t *)(base + offset);
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

static int smmu_alloc_queue(struct kira_smmu *smmu,
                            const struct kira_smmu_platform *platform,
                            void **virtual_address,
                            uint64_t *physical_address)
{
    *virtual_address = platform->alloc_dma_pages(platform->context, 1,
                                                  physical_address);
    if (!*virtual_address || !*physical_address ||
        (*physical_address & (SMMU_PAGE_SIZE - 1u)) != 0) {
        if (*virtual_address && platform->free_dma_pages)
            platform->free_dma_pages(platform->context, *virtual_address,
                                     *physical_address, 1);
        *virtual_address = 0;
        *physical_address = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }

    smmu_zero_memory(*virtual_address, SMMU_PAGE_SIZE);
    if (platform->clean_cache)
        platform->clean_cache(platform->context, *virtual_address,
                              SMMU_PAGE_SIZE);
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
        platform->map_mmio(platform->context, physical_base, SMMU_PAGE_SIZE);
    if (!smmu->registers)
        return KIRA_SMMU_MAP_FAILED;

    smmu->physical_base = physical_base;
    smmu->idr0 = smmu_read32(smmu->registers, SMMU_IDR0);
    smmu->idr1 = smmu_read32(smmu->registers, SMMU_IDR1);
    smmu->idr5 = smmu_read32(smmu->registers, SMMU_IDR5);
    {
        uint32_t sid_bits = smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK;
        smmu->stream_id_limit = sid_bits >= 8u ? SMMU_STRTAB_ENTRIES :
                                (1u << sid_bits);
    }

    /* SMMUv3 identifies itself through the architecture ID registers. */
    if ((smmu->idr0 & 0x3u) == 0)
        return KIRA_SMMU_UNSUPPORTED;

    smmu->queue_entries = SMMU_QUEUE_ENTRIES;
    smmu->stream_table = 0;
    smmu->stream_table_physical = 0;
    smmu->stream_table_lock = 0;
    smmu->cmdq_lock = 0;
    for (uint32_t i = 0; i < 8; ++i)
        smmu->stream_ids[i] = 0;
    /* Start from an unallocated queue state; every failure path can unwind. */
    smmu->cmdq = 0;
    smmu->cmdq_physical = 0;
    smmu->evtq = 0;
    smmu->evtq_physical = 0;
    smmu->cmdq_producer = 0;
    smmu->evtq_consumer = 0;
    smmu->enabled = 0;

    smmu->stream_table = platform->alloc_dma_pages(platform->context,
                            SMMU_STRTAB_PAGES,
                            &smmu->stream_table_physical);
    if (!smmu->stream_table || !smmu->stream_table_physical ||
        (smmu->stream_table_physical & 0xfffu)) {
        if (smmu->stream_table && platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->stream_table,
                                     smmu->stream_table_physical,
                                     SMMU_STRTAB_PAGES);
        smmu->stream_table = 0;
        smmu->stream_table_physical = 0;
        return KIRA_SMMU_MEMORY_FAILED;
    }
    smmu_zero_memory(smmu->stream_table, SMMU_STRTAB_BYTES);
    if (platform->clean_cache)
        platform->clean_cache(platform->context, smmu->stream_table,
                              SMMU_STRTAB_BYTES);

    rc = smmu_alloc_queue(smmu, platform, &smmu->cmdq,
                          &smmu->cmdq_physical);
    if (rc != KIRA_SMMU_OK) {
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->stream_table,
                                     smmu->stream_table_physical,
                                     SMMU_STRTAB_PAGES);
        smmu->stream_table = 0;
        smmu->stream_table_physical = 0;
        return rc;
    }

    rc = smmu_alloc_queue(smmu, platform, &smmu->evtq,
                          &smmu->evtq_physical);
    if (rc != KIRA_SMMU_OK) {
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->cmdq,
                                     smmu->cmdq_physical, 1);
        smmu->cmdq = 0;
        smmu->cmdq_physical = 0;
        if (platform->free_dma_pages)
            platform->free_dma_pages(platform->context, smmu->stream_table,
                                     smmu->stream_table_physical,
                                     SMMU_STRTAB_PAGES);
        smmu->stream_table = 0;
        smmu->stream_table_physical = 0;
        return rc;
    }

    /* Configure the controller-global linear stream table before SMMUEN. */
    smmu_write32(smmu->registers, SMMU_STRTAB_BASE_CFG,
                 (smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK) < 8u ?
                 (smmu->idr1 & SMMU_IDR1_SIDSIZE_MASK) : 8u);
    smmu_write64(smmu->registers, SMMU_STRTAB_BASE,
                 (smmu->stream_table_physical & ~0xfffull) | 1ull);
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

    queue_base = (smmu->cmdq_physical & ~0xfffull) |
                 SMMU_QUEUE_BASE_VALID;
    smmu_write64(smmu->registers, SMMU_CMDQ_BASE, queue_base);
    smmu_write32(smmu->registers, SMMU_CMDQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_CMDQ_CONS, 0);

    queue_base = (smmu->evtq_physical & ~0xfffull) |
                 SMMU_QUEUE_BASE_VALID;
    smmu_write64(smmu->registers, SMMU_EVTQ_BASE, queue_base);
    smmu_write32(smmu->registers, SMMU_EVTQ_PROD, 0);
    smmu_write32(smmu->registers, SMMU_EVTQ_CONS, 0);

    if (platform->clean_cache) {
        platform->clean_cache(platform->context, smmu->cmdq,
                              SMMU_QUEUE_BYTES);
        platform->clean_cache(platform->context, smmu->evtq,
                              SMMU_QUEUE_BYTES);
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
