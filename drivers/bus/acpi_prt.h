#ifndef KIRA_ACPI_PRT_H
#define KIRA_ACPI_PRT_H
#include <stddef.h>
#include <stdint.h>
#define KIRA_ACPI_PRT_MAX_ENTRIES 256u
#define KIRA_ACPI_PRT_SOURCE_MAX 64u
struct kira_acpi_prt_entry {
 uint32_t device; uint8_t function; uint8_t pin; uint16_t reserved;
 uint32_t source_index; char source[KIRA_ACPI_PRT_SOURCE_MAX]; uint8_t link_device;
};
struct kira_acpi_prt_table { struct kira_acpi_prt_entry entries[KIRA_ACPI_PRT_MAX_ENTRIES]; size_t count; };
enum kira_aml_status { KIRA_AML_OK=0, KIRA_AML_NOT_FOUND=-1, KIRA_AML_MALFORMED=-2, KIRA_AML_CAPACITY=-3 };
int kira_acpi_parse_prt(const uint8_t *aml, size_t length, struct kira_acpi_prt_table *table);
#endif
