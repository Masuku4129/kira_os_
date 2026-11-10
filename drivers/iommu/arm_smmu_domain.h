#ifndef KIRA_SMMU_DOMAIN_H
#define KIRA_SMMU_DOMAIN_H

#include <stdint.h>
#include <stddef.h>
#include "arm_smmu.h"

#define KIRA_SMMU_MAP_READ  1u
#define KIRA_SMMU_MAP_WRITE 2u

/* Caller must zero-initialize a domain before its first create call. */
struct kira_smmu_domain {
    struct kira_smmu *smmu;
    /* Alias of the controller-owned table; never freed by a domain. */
    void *stream_table;
    void *context_table;
    uint64_t context_table_physical;
    void *stage1_root;
    uint64_t stage1_root_physical;
    /* Kernel metadata list owns every intermediate translation-table page. */
    void *table_pages;
    uint32_t stream_id;
    uint16_t asid;
    uint32_t lock;
    uint8_t active;
};

int kira_smmu_domain_create(struct kira_smmu_domain *domain,
                            struct kira_smmu *smmu,
                            uint32_t stream_id,
                            const struct kira_smmu_platform *platform);
int kira_smmu_domain_map_4k(struct kira_smmu_domain *domain,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_smmu_platform *platform);
int kira_smmu_domain_unmap_4k(struct kira_smmu_domain *domain,
                              uint64_t iova,
                              const struct kira_smmu_platform *platform);
int kira_smmu_domain_destroy(struct kira_smmu_domain *domain,
                             const struct kira_smmu_platform *platform);

#endif
