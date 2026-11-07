#include "smmu_v3.h"

/* SMMUv3 command queue management. Queue memory must be physically contiguous,
 * 64-byte aligned, non-cacheable/coherent as required by the platform DMA API.
 * This module never enables translation until the caller has installed a
 * valid stream table and supplied the queue memory.
 */
#define SMMU_CMDQ_BASE_LOG2_SHIFT 0u
#define SMMU_CMDQ_BASE_ADDR_MASK  (~0x3full)
#define SMMU_CMDQ_PROD_WRITTEN    (1u << 31)
#define SMMU_CMDQ_CONS_ERR        (1u << 24)
#define SMMU_CMDQ_CONS_ERR_SHIFT  24u
#define SMMU_CMDQ_TIMEOUT        1000000u
#define SMMU_CMD_SYNC             0x46u
#define SMMU_CMD_CFGI_ALL         0x04u
#define SMMU_CMD_TLBI_NH_ALL      0x10u

typedef struct __attribute__((aligned(16))) {
    uint64_t qw0, qw1;
} smmuv3_cmd_t;

typedef struct {
    smmuv3_mmio_t regs;
    volatile smmuv3_cmd_t *queue;
    uint64_t queue_phys;
    uint32_t entries;
    uint32_t producer;
} smmuv3_queue_t;

static int smmuv3_wait_consumer(smmuv3_queue_t *q, uint32_t target) {
    for (uint32_t n = 0; n < SMMU_CMDQ_TIMEOUT; ++n) {
        uint32_t cons;
        if (smmuv3_read32(&q->regs, SMMU_CMDQ_CONS, &cons)) return -1;
        uint32_t index = cons & 0xffffu;
        if (index == target) return 0;
        if ((cons & SMMU_CMDQ_CONS_ERR) != 0) return -2;
    }
    return -3;
}

static int smmuv3_submit(smmuv3_queue_t *q, uint64_t w0, uint64_t w1) {
    if (!q || !q->queue || q->entries < 2u ||
        (q->producer >= q->entries)) return -1;
    uint32_t next = (q->producer + 1u) % q->entries;
    uint32_t cons;
    if (smmuv3_read32(&q->regs, SMMU_CMDQ_CONS, &cons)) return -1;
    if (next == (cons & 0xffffu)) return -2; /* full */
    q->queue[q->producer].qw1 = w1;
    __asm__ volatile("dsb oshst" ::: "memory");
    q->queue[q->producer].qw0 = w0;
    __asm__ volatile("dsb oshst" ::: "memory");
    q->producer = next;
    if (smmuv3_write32(&q->regs, SMMU_CMDQ_PROD, next | SMMU_CMDQ_PROD_WRITTEN))
        return -1;
    return 0;
}

/* Configure a command queue, but leave SMMUEN clear. The physical address
 * must come from the kernel DMA allocator, not a virtual pointer cast. */
int smmuv3_queue_init(smmuv3_queue_t *q, volatile void *mmio, size_t mmio_len,
                     volatile smmuv3_cmd_t *queue, uint64_t queue_phys,
                     uint32_t log2_entries) {
    uint32_t entries;
    size_t bytes;
    if (!q || !mmio || !queue || !queue_phys || (queue_phys & 0x3fu) ||
        smmuv3_queue_geometry(log2_entries, sizeof(smmuv3_cmd_t), &entries, &bytes))
        return -1;
    if (bytes > 0x100000u || ((uintptr_t)queue & 0x3fu)) return -1;
    q->regs.base = (volatile uint8_t *)mmio;
    q->regs.length = mmio_len;
    q->queue = queue;
    q->queue_phys = queue_phys;
    q->entries = entries;
    q->producer = 0;
    uint32_t cr0;
    if (smmuv3_read32(&q->regs, SMMU_CR0, &cr0)) return -1;
    if (cr0 & SMMU_CR0_SMMUEN) return -2; /* require disabled device */
    for (uint32_t i = 0; i < entries; ++i) {
        q->queue[i].qw0 = 0;
        q->queue[i].qw1 = 0;
    }
    __asm__ volatile("dsb oshst" ::: "memory");
    /* CMDQ_BASE: address[51:6], log2size[4:0]. */
    uint64_t base = (queue_phys & SMMU_CMDQ_BASE_ADDR_MASK) |
                    (uint64_t)(log2_entries & 0x1fu);
    if (smmuv3_write32(&q->regs, SMMU_CMDQ_BASE, (uint32_t)base) ||
        smmuv3_write32(&q->regs, SMMU_CMDQ_BASE + 4u, (uint32_t)(base >> 32)) ||
        smmuv3_write32(&q->regs, SMMU_CMDQ_PROD, 0) ||
        smmuv3_write32(&q->regs, SMMU_CMDQ_CONS, 0))
        return -1;
    return 0;
}

/* Global invalidation is used during bring-up before per-ASID/VMID domains
 * exist. A SYNC command makes completion observable before returning. */
int smmuv3_global_invalidate(smmuv3_queue_t *q) {
    if (!q) return -1;
    if (smmuv3_submit(q, SMMU_CMD_CFGI_ALL, 0)) return -1;
    if (smmuv3_submit(q, SMMU_CMD_TLBI_NH_ALL, 0)) return -1;
    if (smmuv3_submit(q, SMMU_CMD_SYNC, 0)) return -1;
    return smmuv3_wait_consumer(q, q->producer);
}
