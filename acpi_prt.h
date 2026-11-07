#ifndef KIRA_ACPI_PRT_H
#define KIRA_ACPI_PRT_H
#include <stddef.h>
#include <stdint.h>

#define KIRA_ACPI_PRT_MAX 128u
typedef struct {
    uint32_t address;       /* PCI _ADR encoding: device in bits 31:16, function in 15:0 */
    uint32_t source_index;  /* GSI when Source is Zero; otherwise link-device index */
    uint8_t pin;             /* 0..3 = INTA..INTD */
    uint8_t direct_gsi;      /* Source == Zero */
    uint8_t source_name_len;
    char source_name[32];    /* bounded AML NameString spelling, empty for direct GSI */
} kira_acpi_prt_entry_t;

/* Extract direct-GSI and link-device routes from a raw AML byte range.
 * This is a bounded table extractor, not a general AML interpreter. It
 * rejects malformed packages and never evaluates AML methods or names. */
int kira_acpi_prt_parse(const uint8_t *aml, size_t length,
                        kira_acpi_prt_entry_t *entries, size_t capacity,
                        size_t *entry_count);
#endif
