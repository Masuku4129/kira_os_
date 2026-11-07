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
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t active;
};

int kira_vtd_domain_create(struct kira_vtd_domain *domain,
                           struct kira_vtd_unit *unit,
                           uint16_t segment, uint8_t bus,
                           uint8_t device, uint8_t function,
                           const struct kira_vtd_platform *platform);
int kira_vtd_domain_map_4k(struct kira_vtd_domain *domain,
                           uint64_t iova, uint64_t physical,
                           uint32_t permissions,
                           const struct kira_vtd_platform *platform);
int kira_vtd_domain_unmap_4k(struct kira_vtd_domain *domain,
                             uint64_t iova,
                             const struct kira_vtd_platform *platform);

#endif
