#ifndef KIRA_ACPI_PRT_H
#define KIRA_ACPI_PRT_H

#include <stddef.h>
#include <stdint.h>

#define KIRA_ACPI_PRT_MAX_ROUTES 256u
#define KIRA_ACPI_PRT_MAX_LINKS  64u

enum kira_acpi_prt_source_kind {
    KIRA_ACPI_PRT_GSI = 0,
    KIRA_ACPI_PRT_LINK_DEVICE = 1
};

struct kira_acpi_prt_route {
    uint16_t device;
    uint8_t function;
    uint8_t pin;
    uint32_t source_index;
    uint32_t gsi;
    uint8_t source_kind;
    char source_name[64];
};

struct kira_acpi_prt_table {
    struct kira_acpi_prt_route routes[KIRA_ACPI_PRT_MAX_ROUTES];
    size_t count;
};

struct kira_acpi_prt_ops {
    void *context;
    int (*resolve_link)(void *context, const char *path,
                        uint32_t source_index, uint32_t *gsi);
};

enum kira_acpi_prt_status {
    KIRA_ACPI_PRT_OK = 0,
    KIRA_ACPI_PRT_INVALID = -1,
    KIRA_ACPI_PRT_UNSUPPORTED = -2,
    KIRA_ACPI_PRT_CAPACITY = -3,
    KIRA_ACPI_PRT_RESOLVE_FAILED = -4
};

/*
 * Decode a constant AML Package/VarPackage containing the _PRT entries.
 * aml begins with the package opcode. scope is the containing namespace path
 * in dotted ASL form (for example "\\_SB.PCI0"); short NameSegs are padded
 * to their four-byte AML representation.
 *
 * Only literal integers, nested packages and static NameStrings are
 * accepted. AML methods, dynamic expressions and link-device resource
 * evaluation are not executed here. A named link source requires ops and
 * must be resolved by the caller; direct-GSI (NullName) routes need no ops.
 */
int kira_acpi_prt_parse(const uint8_t *aml, size_t aml_size,
                        const char *scope,
                        const struct kira_acpi_prt_ops *ops,
                        struct kira_acpi_prt_table *table);

#endif
