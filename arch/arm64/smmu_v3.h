#ifndef KIRA_ARM64_SMMU_V3_H
#define KIRA_ARM64_SMMU_V3_H

#include <stdint.h>
#include <stddef.h>

/* Arm IHI 0070 SMMUv3 register offsets and architectural fields.
 * This header deliberately contains only architectural definitions; platform
 * discovery and MMIO mapping remain the responsibility of the platform layer. */
#define SMMU_IDR0       0x0000u
#define SMMU_IDR1       0x0004u
#define SMMU_IDR2       0x0008u
#define SMMU_IDR3       0x000cu
#define SMMU_IDR5       0x0014u
#define SMMU_CR0        0x0020u
#define SMMU_CR0ACK     0x0024u
#define SMMU_CR1        0x0028u
#define SMMU_CR2        0x002cu
#define SMMU_GBPA       0x0044u
#define SMMU_GERROR     0x0060u
#define SMMU_GERRORN    0x0064u
#define SMMU_GERRORIRQ  0x0068u
#define SMMU_STRTAB_BASE 0x0080u
#define SMMU_STRTAB_BASE_CFG 0x0088u
#define SMMU_CMDQ_BASE  0x0090u
#define SMMU_CMDQ_PROD  0x0098u
#define SMMU_CMDQ_CONS  0x009cu
#define SMMU_EVTQ_BASE  0x00a0u
#define SMMU_EVTQ_PROD  0x00a8u
#define SMMU_EVTQ_CONS  0x00acu
#define SMMU_PRIQ_BASE  0x00b0u
#define SMMU_PRIQ_PROD  0x00b8u
#define SMMU_PRIQ_CONS  0x00bcu
#define SMMU_CMDQ_ENT_SIZE 16u
#define SMMU_CMDQ_MAX_LOG2 19u

#define SMMU_CR0_SMMUEN (1u << 0)
#define SMMU_CR0_EVTQEN (1u << 2)
#define SMMU_CR0_CMDQEN (1u << 1)
#define SMMU_CR0_PRIQEN (1u << 3)
#define SMMU_CR0_ATSCHK (1u << 4)
#define SMMU_GBPA_ABORT  (1u << 20)
#define SMMU_GBPA_UPDATE (1u << 31)

#define SMMU_IDR0_STALL_MODEL_SHIFT 24u
#define SMMU_IDR0_STALL_MODEL_MASK (3u << SMMU_IDR0_STALL_MODEL_SHIFT)
#define SMMU_IDR0_TTF_SHIFT 2u
#define SMMU_IDR0_TTF_MASK (3u << SMMU_IDR0_TTF_SHIFT)
#define SMMU_IDR0_S1P (1u << 1)
#define SMMU_IDR0_S2P (1u << 0)
#define SMMU_IDR1_CMDQS_SHIFT 16u
#define SMMU_IDR1_CMDQS_MASK (31u << SMMU_IDR1_CMDQS_SHIFT)

/* Validate queue geometry before allocating or programming any queue. */
static inline int smmuv3_queue_geometry(uint32_t log2_entries, uint32_t entry_size,
                                         uint32_t *entries, size_t *bytes) {
    if (!entries || !bytes || log2_entries < 1 ||
        log2_entries > SMMU_CMDQ_MAX_LOG2 || entry_size != SMMU_CMDQ_ENT_SIZE)
        return -1;
    *entries = 1u << log2_entries;
    *bytes = (size_t)(*entries) * entry_size;
    return 0;
}

typedef struct {
    volatile uint8_t *base;
    size_t length;
} smmuv3_mmio_t;

typedef struct {
    uint64_t qw0, qw1;
} smmuv3_cmd_t;

typedef struct {
    smmuv3_mmio_t regs;
    volatile smmuv3_cmd_t *queue;
    uint64_t queue_phys;
    uint32_t entries;
    uint32_t producer;
    uint32_t producer_wrap;
} smmuv3_queue_t;

typedef struct {
    smmuv3_cmd_t *entries;
    uint32_t count;
    uint32_t producer;
    uint32_t consumer;
} smmuv3_sw_queue_t;

/* Bind only memory already allocated by the platform DMA allocator. The
 * caller remains responsible for DMA addressability and cache maintenance. */
static inline int smmuv3_swq_bind(smmuv3_sw_queue_t *q, void *memory,
                                   size_t bytes, uint32_t log2_entries) {
    uint32_t count;
    size_t required;
    if (!q || !memory ||
        smmuv3_queue_geometry(log2_entries, SMMU_CMDQ_ENT_SIZE,
                              &count, &required) != 0 ||
        bytes < required || ((uintptr_t)memory & 63u))
        return -1;
    q->entries = (smmuv3_cmd_t *)memory;
    q->count = count;
    q->producer = 0;
    q->consumer = 0;
    for (size_t i = 0; i < required; i++)
        ((volatile uint8_t *)memory)[i] = 0;
    return 0;
}

/* Software staging queue. It intentionally does not write CMDQ_PROD:
 * hardware publication must additionally perform platform DMA sync and
 * use the SMMU's observed CONS pointer. */
static inline int smmuv3_swq_push(smmuv3_sw_queue_t *q,
                                  const smmuv3_cmd_t *cmd) {
    if (!q || !q->entries || !cmd || !q->count) return -1;
    uint32_t next = (q->producer + 1u) & (q->count - 1u);
    if (next == q->consumer) return -2;
    q->entries[q->producer] = *cmd;
    __asm__ volatile("" ::: "memory");
    q->producer = next;
    return 0;
}

static inline int smmuv3_swq_pop(smmuv3_sw_queue_t *q,
                                 smmuv3_cmd_t *cmd) {
    if (!q || !q->entries || !cmd || !q->count) return -1;
    if (q->consumer == q->producer) return -2;
    __asm__ volatile("" ::: "memory");
    *cmd = q->entries[q->consumer];
    q->consumer = (q->consumer + 1u) & (q->count - 1u);
    return 0;
}

static inline int smmuv3_range_valid(const smmuv3_mmio_t *m, uint32_t off, size_t width) {
    return m && m->base && width && off <= m->length && width <= m->length - off;
}
static inline int smmuv3_read32(const smmuv3_mmio_t *m, uint32_t off, uint32_t *out) {
    if (!out || !smmuv3_range_valid(m, off, 4)) return -1;
    *out = *(volatile uint32_t *)(void *)(m->base + off);
    return 0;
}
static inline int smmuv3_write64(const smmuv3_mmio_t *m, uint32_t off, uint64_t value) {
    if (!smmuv3_range_valid(m, off, 8) || (off & 7u)) return -1;
    *(volatile uint64_t *)(void *)(m->base + off) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
    return 0;
}
static inline int smmuv3_write32(const smmuv3_mmio_t *m, uint32_t off, uint32_t value) {
    if (!smmuv3_range_valid(m, off, 4)) return -1;
    *(volatile uint32_t *)(void *)(m->base + off) = value;
    __asm__ volatile("" ::: "memory");
    return 0;
}

#endif
