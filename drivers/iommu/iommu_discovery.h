#ifndef KIRA_IOMMU_DISCOVERY_H
#define KIRA_IOMMU_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include "intel_vtd.h"
#include "arm_smmu.h"

struct kira_iommu_discovery_ops {
    void *context;
    int (*reserve_physical)(void *context, uint64_t base, uint64_t length);
    void (*report)(void *context, const char *name, int status,
                   uint64_t address);
};

struct kira_iommu_inventory;
struct kira_dmar_inventory;

int kira_iommu_reserve_dmar_regions(
    const struct kira_dmar_inventory *dmar,
    const struct kira_iommu_discovery_ops *ops);
int kira_iommu_prepare_intel_units(
    const struct kira_dmar_inventory *dmar,
    struct kira_vtd_unit *units,
    size_t unit_capacity,
    size_t *units_prepared,
    const struct kira_vtd_platform *platform,
    const struct kira_iommu_discovery_ops *ops);

#endif
