#ifndef KIRA_ACPI_DMAR_H
#define KIRA_ACPI_DMAR_H
#include <stdint.h>
#include <stddef.h>

#define ACPI_DMAR_SIG "DMAR"
#define ACPI_DMAR_MAX_DRHD 32u
#define ACPI_DMAR_MAX_RMRR 32u

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
    uint8_t host_address_width, flags;
    uint8_t reserved[10];
} acpi_dmar_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type, length;
} acpi_dmar_struct_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type, length;
    uint8_t flags, segment;
    uint64_t register_base;
} acpi_dmar_drhd_t;

typedef struct __attribute__((packed)) {
    uint16_t type, length;
    uint16_t reserved, segment;
    uint64_t base, limit;
} acpi_dmar_rmrr_t;

typedef struct {
    const uint8_t *cursor;
    const uint8_t *end;
} acpi_dmar_iter_t;

static inline int acpi_dmar_validate(const void *table, size_t available) {
    if (!table || available < sizeof(acpi_dmar_header_t)) return -1;
    const acpi_dmar_header_t *h = (const acpi_dmar_header_t *)table;
    if (h->signature[0] != 'D' || h->signature[1] != 'M' ||
        h->signature[2] != 'A' || h->signature[3] != 'R') return -1;
    if (h->length < sizeof(*h) || h->length > available) return -1;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < h->length; ++i)
        sum = (uint8_t)(sum + ((const uint8_t *)table)[i]);
    return sum == 0 ? 0 : -1;
}
static inline void acpi_dmar_iter_init(acpi_dmar_iter_t *it, const void *table) {
    const acpi_dmar_header_t *h = (const acpi_dmar_header_t *)table;
    it->cursor = (const uint8_t *)table + sizeof(*h);
    it->end = (const uint8_t *)table + h->length;
}
static inline int acpi_dmar_iter_next(acpi_dmar_iter_t *it,
                                      const acpi_dmar_struct_header_t **out) {
    if (!it || !out || it->cursor == it->end) return 0;
    if (it->cursor > it->end ||
        (size_t)(it->end - it->cursor) < sizeof(**out)) return -1;
    const acpi_dmar_struct_header_t *h =
        (const acpi_dmar_struct_header_t *)(const void *)it->cursor;
    if (h->length < sizeof(*h) || (size_t)(it->end - it->cursor) < h->length)
        return -1;
    *out = h;
    it->cursor += h->length;
    return 1;
}
#endif
