#ifndef KIRA_SMMU_DOMAIN_H
#define KIRA_SMMU_DOMAIN_H

#include <stdint.h>
#include <stddef.h>
#include "arm_smmu.h"

#define KIRA_SMMU_MAP_READ  1u
#define KIRA_SMMU_MAP_WRITE 2u

/* Domain contract:
   - Zero-initialize before first passthrough/create call; do not copy after use.
   - stream_id must be below smmu->stream_id_limit.
   - A bypass domain is unrestricted DMA; a created stage-1 domain starts with
     no valid data mappings and grants access only through map_4k().
   - map/unmap operate on 4 KiB-aligned IOVA/physical addresses. A failed
     unmap/translation invalidation must be treated as still potentially live. */
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
    uint8_t bypass;
};

/* Install an explicit bypass STE for a stream; caller must quiesce that device. */
int kira_smmu_domain_passthrough(struct kira_smmu_domain *domain,
                                   struct kira_smmu *smmu,
                                   uint32_t stream_id,
                                   const struct kira_smmu_platform *platform);
/* Replace bypass with a private stage-1 context containing no data mappings. */
int kira_smmu_domain_create(struct kira_smmu_domain *domain,
                            struct kira_smmu *smmu,
                            uint32_t stream_id,
                            const struct kira_smmu_platform *platform);
/* Map one 4 KiB page with READ/WRITE permissions; all addresses must align. */
int kira_smmu_domain_map_4k(struct kira_smmu_domain *domain,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_smmu_platform *platform);
/* Remove one mapping and complete ASID invalidation. If the PTE is already
   absent, retry invalidation so a previous timeout can be safely recovered. */
int kira_smmu_domain_unmap_4k(struct kira_smmu_domain *domain,
                              uint64_t iova,
                              const struct kira_smmu_platform *platform);
int kira_smmu_domain_destroy(struct kira_smmu_domain *domain,
                             const struct kira_smmu_platform *platform);

#endif
