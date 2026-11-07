#include "smmu_v3.h"

/* SMMUv3 command queue management. Queue memory must be physically contiguous,
 * 64-byte aligned, non-cacheable/coherent as required by the platform DMA API.
 * This module never enables translation until the caller has installed a
 * valid stream table and supplied the queue memory.
 */
#define SMMU_CMDQ_BASE_LOG2_SHIFT 0u
#define SMMU_CMDQ_BASE_ADDR_MASK  (~0x3full)
#define SMMU_Q_PTR_WRAP           (1u << 31)
#define SMMU_Q_PTR_INDEX_MASK     0x001fffffu
#define SMMU_CMDQ_CONS_ERR_MASK   0x7f000000u
#define SMMU_CMDQ_TIMEOUT        1000000u
#define SMMU_CMD_SYNC             0x46u
#define SMMU_CMD_CFGI_ALL         0x04u
#define SMMU_CMD_TLBI_NH_ALL      0x10u

static int smmuv3_wait_consumer(smmuv3_queue_t *q, uint32_t target) {
    for (uint32_t n = 0; n < SMMU_CMDQ_TIMEOUT; ++n) {
        uint32_t cons;
        if (smmuv3_read32(&q->regs, SMMU_CMDQ_CONS, &cons)) return -1;
        uint32_t index = cons & SMMU_Q_PTR_INDEX_MASK;
        uint32_t wrap = (cons & SMMU_Q_PTR_WRAP) != 0;
        if (index == (target & SMMU_Q_PTR_INDEX_MASK) &&
            wrap == ((target & SMMU_Q_PTR_WRAP) != 0)) return 0;
        if ((cons & SMMU_CMDQ_CONS_ERR_MASK) != 0) return -2;
    }
    return -3;
}

static int smmuv3_submit(smmuv3_queue_t *q, uint64_t w0, uint64_t w1) {
    if (!q || !q->queue || q->entries < 2u ||
        (q->producer >= q->entries)) return -1;
    uint32_t next = q->producer + 1u;
    uint32_t next_wrap = q->producer_wrap;
    if (next == q->entries) { next = 0; next_wrap ^= 1u; }
    uint32_t cons;
    if (smmuv3_read32(&q->regs, SMMU_CMDQ_CONS, &cons)) return -1;
    uint32_t cons_index = cons & SMMU_Q_PTR_INDEX_MASK;
    uint32_t cons_wrap = (cons & SMMU_Q_PTR_WRAP) != 0;
    if (next == cons_index && next_wrap != cons_wrap) return -2; /* full */
    q->queue[q->producer].qw1 = w1;
    __asm__ volatile("dsb oshst" ::: "memory");
    q->queue[q->producer].qw0 = w0;
    __asm__ volatile("dsb oshst" ::: "memory");
    if (smmuv3_write32(&q->regs, SMMU_CMDQ_PROD,
                       next | (next_wrap ? SMMU_Q_PTR_WRAP : 0u)))
        return -1;
    q->producer = next;
    q->producer_wrap = next_wrap;
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
    /* CMDQ_BASE: address[51:6], queue-size logarithm in bits [5:0]. */
    if (queue_phys & ~0x000fffffffffffc0ull) return -1;
    uint64_t base = (queue_phys & SMMU_CMDQ_BASE_ADDR_MASK) |
                    (uint64_t)(log2_entries & 0x3fu);
    if (smmuv3_write64(&q->regs, SMMU_CMDQ_BASE, base) ||
        smmuv3_write32(&q->regs, SMMU_CMDQ_PROD, 0) ||
        smmuv3_write32(&q->regs, SMMU_CMDQ_CONS, 0))
        return -1;
    return 0;
}

/* Enable the command queue while translation remains disabled. */
int smmuv3_queue_enable(smmuv3_queue_t *q) {
    if (!q || !q->queue) return -1;
    uint32_t cr0, ack;
    if (smmuv3_read32(&q->regs, SMMU_CR0, &cr0)) return -1;
    if (cr0 & SMMU_CR0_SMMUEN) return -2;
    if (!(cr0 & SMMU_CR0_CMDQEN)) {
        if (smmuv3_write32(&q->regs, SMMU_CR0, cr0 | SMMU_CR0_CMDQEN)) return -1;
    }
    for (uint32_t n = 0; n < SMMU_CMDQ_TIMEOUT; n++) {
        if (smmuv3_read32(&q->regs, SMMU_CR0ACK, &ack)) return -1;
        if (ack & SMMU_CR0_CMDQEN) return 0;
    }
    return -3;
}

/* Global invalidation is used during bring-up before per-ASID/VMID domains
 * exist. A SYNC command makes completion observable before returning. */
int smmuv3_global_invalidate(smmuv3_queue_t *q) {
    if (!q) return -1;
    uint32_t cr0;
    if (smmuv3_read32(&q->regs, SMMU_CR0, &cr0) ||
        !(cr0 & SMMU_CR0_CMDQEN)) return -2;
    if (smmuv3_submit(q, SMMU_CMD_CFGI_ALL, 0)) return -1;
    if (smmuv3_submit(q, SMMU_CMD_TLBI_NH_ALL, 0)) return -1;
    if (smmuv3_submit(q, SMMU_CMD_SYNC, 0)) return -1;
    return smmuv3_wait_consumer(q, q->producer | (q->producer_wrap ? SMMU_Q_PTR_WRAP : 0u));
}
