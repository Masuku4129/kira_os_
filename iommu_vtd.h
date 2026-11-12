#ifndef KIRA_IOMMU_VTD_H
#define KIRA_IOMMU_VTD_H
#include <stdint.h>

#define VTD_MAX_UNITS 16
#define VTD_MAX_RMRR 32
#define VTD_MAX_SCOPES 128
#define VTD_MAX_PATH 8

typedef struct {
    uint8_t type, length, enumeration_id, start_bus;
    uint8_t path_count;
    struct { uint8_t device, function; } path[VTD_MAX_PATH];
} vtd_scope_t;

typedef struct {
    uint16_t segment;
    uint8_t include_all, flags;
    uint64_t register_base;
    uint16_t first_scope, scope_count;
} vtd_unit_t;

typedef struct {
    uint16_t segment;
    uint64_t base, limit;
    uint16_t first_scope, scope_count;
} vtd_rmrr_t;

typedef struct {
    uint8_t host_address_width, flags;
    uint16_t unit_count, rmrr_count, scope_count;
    vtd_unit_t units[VTD_MAX_UNITS];
    vtd_rmrr_t rmrr[VTD_MAX_RMRR];
    vtd_scope_t scopes[VTD_MAX_SCOPES];
} vtd_dmar_info_t;

/* Parse a complete ACPI DMAR table (including 36-byte SDT header).
 * Returns 0 on success; negative values identify malformed/unsupported input.
 * This discovers hardware topology only; it does not enable DMA translation. */
int vtd_dmar_parse(const void *table, uint32_t available, vtd_dmar_info_t *out);

#endif
