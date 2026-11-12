#ifndef KIRA_VTD_DOMAIN_H
#define KIRA_VTD_DOMAIN_H

#include <stdint.h>
#include <stddef.h>
#include "intel_vtd.h"

#define KIRA_VTD_PAGE_SIZE 4096u
#define KIRA_VTD_MAP_READ  1u
#define KIRA_VTD_MAP_WRITE 2u

struct kira_vtd_domain {
    struct kira_vtd_unit *unit;
    void *root_context;
    uint64_t root_context_physical;
    void *second_level;
    uint64_t second_level_physical;
    uint32_t id;                    /* domain identifier in the context entry */
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t active;
};

/* Context entry with translation type 'pass-through': the device keeps seeing
   physical addresses. Needs ECAP.PT. Never replaces a translated context. */
int kira_vtd_context_passthrough(struct kira_vtd_unit *unit,
                                 uint16_t segment, uint8_t bus,
                                 uint8_t device, uint8_t function,
                                 const struct kira_vtd_platform *platform);

/* A translated domain with an empty 4-level table: every DMA of the device
   faults until kira_vtd_domain_map_4k() grants a page. An existing pass-through
   context of the same device is replaced; an existing domain is an error. */
int kira_vtd_domain_create(struct kira_vtd_domain *domain,
                           struct kira_vtd_unit *unit,
                           uint16_t segment, uint8_t bus,
                           uint8_t device, uint8_t function,
                           const struct kira_vtd_platform *platform);
/* Return the device to a pass-through context and free the domain's tables.
   The hardware caches are invalidated before any table page is released. */
int kira_vtd_domain_release(struct kira_vtd_domain *domain,
                            const struct kira_vtd_platform *platform);
int kira_vtd_domain_map_4k(struct kira_vtd_domain *domain,
                           uint64_t iova, uint64_t physical,
                           uint32_t permissions,
                           const struct kira_vtd_platform *platform);
int kira_vtd_domain_unmap_4k(struct kira_vtd_domain *domain,
                             uint64_t iova,
                             const struct kira_vtd_platform *platform);
struct kira_vtd_range {
    uint64_t iova;
    uint32_t pages;
};
/* Remove several ranges with one IOTLB invalidation. On invalidation failure,
   the caller must retain every backing page and retry the whole batch. */
int kira_vtd_domain_unmap_ranges(struct kira_vtd_domain *domain,
                                 const struct kira_vtd_range *ranges,
                                 size_t count,
                                 const struct kira_vtd_platform *platform);
int kira_vtd_domain_unmap_range(struct kira_vtd_domain *domain,
                                uint64_t iova, uint32_t pages,
                                const struct kira_vtd_platform *platform);

#endif
