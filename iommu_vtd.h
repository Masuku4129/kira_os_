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
    uint32_t register_bytes;       /* size of the register window (DRHD Size field) */
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

/* Device scope types (ACPI DMAR, Device Scope structure). */
#define VTD_SCOPE_ENDPOINT 1
#define VTD_SCOPE_BRIDGE   2
#define VTD_SCOPE_IOAPIC   3
#define VTD_SCOPE_HPET     4
#define VTD_SCOPE_ACPI_DEV 5

/* PCI bridge topology callback used to walk multi-element scope paths: for the
   bridge at bus:dev.fn returns 0 and its secondary/subordinate bus numbers,
   non-zero if no bridge is there. */
typedef int (*vtd_bridge_fn)(void *context, uint8_t bus, uint8_t dev, uint8_t fn,
                             uint8_t *secondary, uint8_t *subordinate);

/* 1 when the scope names the function bus:dev.fn (an endpoint scope exactly, a
   bridge scope the bridge itself and everything on the buses behind it). */
int vtd_scope_covers(const vtd_scope_t *scope, uint8_t bus, uint8_t dev, uint8_t fn,
                     vtd_bridge_fn bridge, void *context);

/* Index of the DMAR unit that translates DMA of the function, or -1: a unit
   whose scope names it wins over an INCLUDE_PCI_ALL unit of the same segment. */
int vtd_dmar_unit_of(const vtd_dmar_info_t *info, uint16_t segment,
                     uint8_t bus, uint8_t dev, uint8_t fn,
                     vtd_bridge_fn bridge, void *context);

/* Index of the first reserved-memory region (RMRR) whose scope names the
   function, or -1. Such a device must keep access to that region. */
int vtd_dmar_rmrr_of(const vtd_dmar_info_t *info, uint16_t segment,
                     uint8_t bus, uint8_t dev, uint8_t fn,
                     vtd_bridge_fn bridge, void *context);

#endif
