#ifndef KIRA_ARM_SMMU_H
#define KIRA_ARM_SMMU_H

#include <stddef.h>
#include <stdint.h>

struct kira_smmu_platform {
    void *context;
    volatile void *(*map_mmio)(void *context, uint64_t physical, size_t length);
    void *(*alloc_dma_pages)(void *context, size_t pages, uint64_t *physical);
    void (*free_dma_pages)(void *context, void *virtual_address,
                           uint64_t physical, size_t pages);
    void (*clean_cache)(void *context, const void *address, size_t length);
    void (*invalidate_cache)(void *context, const void *address, size_t length);
    void (*write_barrier)(void *context);
    void (*read_barrier)(void *context);
    void (*delay)(void *context);
};

struct kira_smmu {
    volatile uint8_t *registers;
    uint64_t physical_base;
    uint32_t idr0;
    uint32_t idr1;
    uint32_t idr5;
    uint32_t queue_entries;
    /* Number of StreamIDs addressable by the configured linear table. */
    uint32_t stream_id_limit;
    uint8_t enabled;
    void *cmdq;
    uint64_t cmdq_physical;
    void *evtq;
    uint64_t evtq_physical;
    uint32_t cmdq_producer;
    uint32_t evtq_consumer;
    /* Controller-owned linear stream table shared by every domain. */
    void *stream_table;
    uint64_t stream_table_physical;
    /* Raw over-allocation retained so the aligned table can be freed safely. */
    void *stream_table_allocation;
    uint64_t stream_table_allocation_physical;
    size_t stream_table_allocation_pages;
    uint32_t stream_table_lock;
    /* Serializes CMDQ publication through matching CMD_SYNC completion. */
    uint32_t cmdq_lock;
    uint32_t stream_ids[8];
};

enum kira_smmu_result {
    KIRA_SMMU_OK = 0,
    KIRA_SMMU_INVALID = -1,
    KIRA_SMMU_MAP_FAILED = -2,
    KIRA_SMMU_UNSUPPORTED = -3,
    KIRA_SMMU_MEMORY_FAILED = -4,
    KIRA_SMMU_TIMEOUT = -5,
    KIRA_SMMU_HARDWARE_ERROR = -6
};

int kira_smmu_v3_prepare(struct kira_smmu *smmu, uint64_t physical_base,
                         const struct kira_smmu_platform *platform);
int kira_smmu_v3_enable(struct kira_smmu *smmu,
                        const struct kira_smmu_platform *platform);
int kira_smmu_v3_disable(struct kira_smmu *smmu,
                         const struct kira_smmu_platform *platform);

#endif
