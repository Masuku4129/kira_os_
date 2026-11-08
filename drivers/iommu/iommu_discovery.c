/*
 * IOMMU discovery orchestration.
 *
 * This module turns validated ACPI DMAR inventory into explicit reserved
 * memory and VT-d unit objects. Any RMRR reservation failure is propagated;
 * callers must not enable bus mastering while discovery is incomplete.
 */

#include "iommu_discovery.h"
#include "intel_vtd.h"
#include "dmar.h"

static void iommu_report(const struct kira_iommu_discovery_ops *ops,
                         const char *name, int status, uint64_t address)
{
    if (ops && ops->report)
        ops->report(ops->context, name, status, address);
}

int kira_iommu_reserve_dmar_regions(
    const struct kira_dmar_inventory *dmar,
    const struct kira_iommu_discovery_ops *ops)
{
    uint16_t i;

    if (!dmar || !ops || !ops->reserve_physical)
        return KIRA_VTD_INVALID;

    for (i = 0; i < dmar->rmrr_count; ++i) {
        const struct kira_dmar_reserved_region *region = &dmar->rmrrs[i];
        uint64_t length;

        if (region->base > region->limit ||
            region->limit == UINT64_MAX)
            return KIRA_VTD_INVALID;

        length = region->limit - region->base + 1u;
        if (ops->reserve_physical(ops->context, region->base, length) != 0) {
            iommu_report(ops, "DMAR RMRR reservation failed",
                         KIRA_VTD_HARDWARE_ERROR, region->base);
            return KIRA_VTD_HARDWARE_ERROR;
        }

        iommu_report(ops, "DMAR RMRR reserved", KIRA_VTD_OK, region->base);
    }

    return KIRA_VTD_OK;
}

int kira_iommu_prepare_intel_units(
    const struct kira_dmar_inventory *dmar,
    struct kira_vtd_unit *units,
    size_t unit_capacity,
    size_t *units_prepared,
    const struct kira_vtd_platform *platform,
    const struct kira_iommu_discovery_ops *ops)
{
    uint16_t i;
    size_t prepared = 0;

    if (!dmar || !units || !units_prepared || !platform)
        return KIRA_VTD_INVALID;

    *units_prepared = 0;
    if (dmar->unit_count > unit_capacity)
        return KIRA_VTD_INVALID;

    for (i = 0; i < dmar->unit_count; ++i) {
        const struct kira_dmar_unit *source = &dmar->units[i];
        int rc;

        /*
         * A DMAR unit is only selected for this segment when the caller
         * provides a matching PCI segment. This implementation accepts
         * segment zero; nonzero segments remain reported but untouched.
         */
        if (source->segment != 0) {
            iommu_report(ops, "VT-d nonzero PCI segment not activated",
                         KIRA_VTD_UNSUPPORTED, source->register_base);
            continue;
        }

        rc = kira_vtd_unit_prepare(&units[prepared],
                                   source->register_base, 4096u,
                                   dmar->host_address_width, platform);
        iommu_report(ops, "VT-d unit prepare", rc, source->register_base);
        if (rc != KIRA_VTD_OK)
            return rc;

        ++prepared;
    }

    *units_prepared = prepared;
    return KIRA_VTD_OK;
}
