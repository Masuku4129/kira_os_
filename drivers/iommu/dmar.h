#ifndef KIRA_DMAR_H
#define KIRA_DMAR_H

#include <stddef.h>
#include <stdint.h>

#define KIRA_DMAR_SIGNATURE       0x52414d44u /* "DMAR" */
#define KIRA_DMAR_MAX_UNITS       32u
#define KIRA_DMAR_MAX_RMRR        64u
#define KIRA_DMAR_MAX_SCOPES      256u
#define KIRA_DMAR_MAX_SCOPE_PATH  16u

#define KIRA_DMAR_OK              0
#define KIRA_DMAR_E_ARGUMENT     -1
#define KIRA_DMAR_E_HEADER       -2
#define KIRA_DMAR_E_LENGTH       -3
#define KIRA_DMAR_E_STRUCTURE    -4
#define KIRA_DMAR_E_CAPACITY     -5
#define KIRA_DMAR_E_CALLBACK     -6

#if defined(__GNUC__) || defined(__clang__)
#define KIRA_PACKED __attribute__((packed))
#else
#define KIRA_PACKED
#endif

struct KIRA_PACKED kira_acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct KIRA_PACKED kira_dmar_header {
    struct kira_acpi_sdt_header acpi;
    uint8_t host_address_width;
    uint8_t flags;
    uint8_t reserved[10];
};

struct KIRA_PACKED kira_dmar_structure {
    uint16_t type;
    uint16_t length;
};

struct KIRA_PACKED kira_dmar_drhd {
    struct kira_dmar_structure header;
    uint8_t flags;
    uint16_t segment;
    uint64_t register_base;
};

struct KIRA_PACKED kira_dmar_rmrr {
    struct kira_dmar_structure header;
    uint16_t reserved;
    uint16_t segment;
    uint64_t base;
    uint64_t limit;
};

struct KIRA_PACKED kira_dmar_scope {
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint8_t enumeration_id;
    uint8_t start_bus;
    uint8_t path[];
};

struct kira_dmar_path {
    uint8_t device;
    uint8_t function;
};

struct kira_dmar_unit {
    uint16_t segment;
    uint8_t include_all;
    uint64_t register_base;
    uint16_t first_scope;
    uint16_t scope_count;
};

struct kira_dmar_reserved_region {
    uint16_t segment;
    uint64_t base;
    uint64_t limit; /* inclusive, as specified by ACPI */
    uint16_t first_scope;
    uint16_t scope_count;
};

struct kira_dmar_scope_record {
    uint8_t type;
    uint8_t enumeration_id;
    uint8_t start_bus;
    uint8_t path_length;
    struct kira_dmar_path path[KIRA_DMAR_MAX_SCOPE_PATH];
};

struct kira_dmar_inventory {
    uint8_t host_address_width;
    uint8_t interrupt_remapping_supported;
    uint8_t x2apic_opt_out;
    uint8_t dma_ctrl_platform_opt_in;
    uint16_t unit_count;
    uint16_t rmrr_count;
    uint16_t scope_count;
    struct kira_dmar_unit units[KIRA_DMAR_MAX_UNITS];
    struct kira_dmar_reserved_region rmrrs[KIRA_DMAR_MAX_RMRR];
    struct kira_dmar_scope_record scopes[KIRA_DMAR_MAX_SCOPES];
};

typedef int (*kira_dmar_unit_fn)(const struct kira_dmar_unit *, void *);
typedef int (*kira_dmar_rmrr_fn)(const struct kira_dmar_reserved_region *, void *);

int kira_dmar_parse(const void *table, size_t available,
                    struct kira_dmar_inventory *out);
int kira_dmar_for_each_unit(const struct kira_dmar_inventory *inventory,
                            kira_dmar_unit_fn callback, void *context);
int kira_dmar_for_each_rmrr(const struct kira_dmar_inventory *inventory,
                            kira_dmar_rmrr_fn callback, void *context);

#endif
