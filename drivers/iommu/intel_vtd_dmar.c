/*
 * KIRA OS — Intel VT-d ACPI DMAR discovery.
 *
 * Freestanding parser for the ACPI DMAR table. This module deliberately
 * performs discovery only: it does not claim DMA isolation until hardware
 * registers, translation structures, invalidation and fault handling are
 * implemented by the VT-d runtime.
 */

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
    uint8_t reserved;
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
    uint16_t unit_count;
    uint16_t rmrr_count;
    uint16_t scope_count;
    struct kira_dmar_unit units[KIRA_DMAR_MAX_UNITS];
    struct kira_dmar_reserved_region rmrrs[KIRA_DMAR_MAX_RMRR];
    struct kira_dmar_scope_record scopes[KIRA_DMAR_MAX_SCOPES];
};

typedef int (*kira_dmar_unit_fn)(const struct kira_dmar_unit *, void *);
typedef int (*kira_dmar_rmrr_fn)(const struct kira_dmar_reserved_region *, void *);

static uint16_t dmar_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t dmar_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t dmar_u64(const uint8_t *p)
{
    return (uint64_t)dmar_u32(p) |
           ((uint64_t)dmar_u32(p + 4) << 32);
}

static int dmar_checksum_valid(const uint8_t *bytes, size_t length)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < length; ++i)
        sum = (uint8_t)(sum + bytes[i]);

    return sum == 0;
}

static int dmar_parse_scopes(const uint8_t *bytes,
                             size_t length,
                             struct kira_dmar_inventory *out,
                             uint16_t *first,
                             uint16_t *count)
{
    size_t offset = 0;

    *first = out->scope_count;
    *count = 0;

    while (offset < length) {
        const uint8_t *s = bytes + offset;
        uint8_t scope_length;
        size_t path_bytes;
        size_t path_count;
        size_t i;
        struct kira_dmar_scope_record *record;

        if (length - offset < 6)
            return KIRA_DMAR_E_STRUCTURE;

        scope_length = s[1];
        if (scope_length < 8 || scope_length > length - offset)
            return KIRA_DMAR_E_STRUCTURE;

        path_bytes = (size_t)scope_length - 6;
        if ((path_bytes & 1u) != 0)
            return KIRA_DMAR_E_STRUCTURE;

        path_count = path_bytes / 2;
        if (path_count == 0 || path_count > KIRA_DMAR_MAX_SCOPE_PATH)
            return KIRA_DMAR_E_STRUCTURE;

        if (out->scope_count >= KIRA_DMAR_MAX_SCOPES)
            return KIRA_DMAR_E_CAPACITY;

        record = &out->scopes[out->scope_count++];
        record->type = s[0];
        record->enumeration_id = s[4];
        record->start_bus = s[5];
        record->path_length = (uint8_t)path_count;

        for (i = 0; i < path_count; ++i) {
            uint8_t device = s[6 + i * 2];
            uint8_t function = s[7 + i * 2];

            /* PCI device is 0..31; function is 0..7. */
            if (device > 31 || function > 7)
                return KIRA_DMAR_E_STRUCTURE;

            record->path[i].device = device;
            record->path[i].function = function;
        }

        ++(*count);
        offset += scope_length;
    }

    return offset == length ? KIRA_DMAR_OK : KIRA_DMAR_E_STRUCTURE;
}

/*
 * Parse and validate one DMAR table into a caller-owned inventory.
 * The caller must provide a physically mapped, readable table buffer.
 * No hardware register is touched by this function.
 */
int kira_dmar_parse(const void *table,
                    size_t available,
                    struct kira_dmar_inventory *out)
{
    const uint8_t *bytes = (const uint8_t *)table;
    const struct kira_dmar_header *header;
    uint32_t table_length;
    size_t offset;

    if (!table || !out || available < sizeof(struct kira_dmar_header))
        return KIRA_DMAR_E_ARGUMENT;

    header = (const struct kira_dmar_header *)table;
    if (dmar_u32(bytes) != KIRA_DMAR_SIGNATURE)
        return KIRA_DMAR_E_HEADER;

    table_length = dmar_u32(bytes + 4);
    if (table_length < sizeof(*header) || table_length > available)
        return KIRA_DMAR_E_LENGTH;

    if (!dmar_checksum_valid(bytes, table_length))
        return KIRA_DMAR_E_HEADER;

    if (header->host_address_width < 32 ||
        header->host_address_width > 64)
        return KIRA_DMAR_E_STRUCTURE;

    {
        size_t i;
        for (i = 0; i < sizeof(header->reserved); ++i) {
            if (header->reserved[i] != 0)
                return KIRA_DMAR_E_STRUCTURE;
        }
    }

    /* Reset only after the complete table header has passed validation. */
    {
        size_t i;
        uint8_t *p = (uint8_t *)out;
        for (i = 0; i < sizeof(*out); ++i)
            p[i] = 0;
    }

    out->host_address_width = header->host_address_width;
    out->interrupt_remapping_supported = (header->flags & 0x1u) != 0;
    out->x2apic_opt_out = (header->flags & 0x2u) != 0;

    offset = sizeof(*header);
    while (offset < table_length) {
        const uint8_t *entry = bytes + offset;
        uint16_t type;
        uint16_t length;
        int rc;

        if (table_length - offset < sizeof(struct kira_dmar_structure))
            return KIRA_DMAR_E_STRUCTURE;

        type = dmar_u16(entry);
        length = dmar_u16(entry + 2);

        if (length < sizeof(struct kira_dmar_structure) ||
            length > table_length - offset)
            return KIRA_DMAR_E_STRUCTURE;

        if (type == 0) {
            struct kira_dmar_unit *unit;
            uint16_t first, count;

            if (length < sizeof(struct kira_dmar_drhd) ||
                out->unit_count >= KIRA_DMAR_MAX_UNITS)
                return length < sizeof(struct kira_dmar_drhd)
                    ? KIRA_DMAR_E_STRUCTURE : KIRA_DMAR_E_CAPACITY;

            if (entry[5] != 0)
                return KIRA_DMAR_E_STRUCTURE;

            unit = &out->units[out->unit_count];
            unit->include_all = entry[4] & 0x1u;
            unit->segment = dmar_u16(entry + 6);
            unit->register_base = dmar_u64(entry + 8);

            if ((unit->register_base & 0xfffu) != 0 ||
                unit->register_base == 0)
                return KIRA_DMAR_E_STRUCTURE;

            rc = dmar_parse_scopes(entry + sizeof(struct kira_dmar_drhd),
                                   length - sizeof(struct kira_dmar_drhd),
                                   out, &first, &count);
            if (rc != KIRA_DMAR_OK)
                return rc;

            unit->first_scope = first;
            unit->scope_count = count;
            ++out->unit_count;
        } else if (type == 1) {
            struct kira_dmar_reserved_region *region;
            uint16_t first, count;

            if (length < sizeof(struct kira_dmar_rmrr) ||
                out->rmrr_count >= KIRA_DMAR_MAX_RMRR)
                return length < sizeof(struct kira_dmar_rmrr)
                    ? KIRA_DMAR_E_STRUCTURE : KIRA_DMAR_E_CAPACITY;

            if (dmar_u16(entry + 4) != 0)
                return KIRA_DMAR_E_STRUCTURE;

            region = &out->rmrrs[out->rmrr_count];
            region->segment = dmar_u16(entry + 6);
            region->base = dmar_u64(entry + 8);
            region->limit = dmar_u64(entry + 16);

            if (region->base > region->limit ||
                (region->base & 0xfffu) != 0 ||
                (region->limit & 0xfffu) != 0xfffu)
                return KIRA_DMAR_E_STRUCTURE;

            rc = dmar_parse_scopes(entry + sizeof(struct kira_dmar_rmrr),
                                   length - sizeof(struct kira_dmar_rmrr),
                                   out, &first, &count);
            if (rc != KIRA_DMAR_OK)
                return rc;

            region->first_scope = first;
            region->scope_count = count;
            ++out->rmrr_count;
        } else {
            /*
             * Unknown DMAR structures are skipped by their validated length,
             * as required for forward-compatible ACPI table parsing.
             */
        }

        offset += length;
    }

    return offset == table_length ? KIRA_DMAR_OK : KIRA_DMAR_E_STRUCTURE;
}

/* Enumerate discovered units only after parsing has succeeded. */
int kira_dmar_for_each_unit(const struct kira_dmar_inventory *inventory,
                            kira_dmar_unit_fn callback,
                            void *context)
{
    uint16_t i;

    if (!inventory || !callback)
        return KIRA_DMAR_E_ARGUMENT;

    for (i = 0; i < inventory->unit_count; ++i) {
        if (callback(&inventory->units[i], context) != 0)
            return KIRA_DMAR_E_CALLBACK;
    }

    return KIRA_DMAR_OK;
}

/* Enumerate reserved-memory regions; callers must reserve them before DMA. */
int kira_dmar_for_each_rmrr(const struct kira_dmar_inventory *inventory,
                            kira_dmar_rmrr_fn callback,
                            void *context)
{
    uint16_t i;

    if (!inventory || !callback)
        return KIRA_DMAR_E_ARGUMENT;

    for (i = 0; i < inventory->rmrr_count; ++i) {
        if (callback(&inventory->rmrrs[i], context) != 0)
            return KIRA_DMAR_E_CALLBACK;
    }

    return KIRA_DMAR_OK;
}
