#include "acpi_prt.h"

static int aml_pkg_end(const uint8_t *p, const uint8_t *end,
                       const uint8_t **body, const uint8_t **limit) {
    if (p >= end) return -1;
    uint8_t lead = *p;
    unsigned follow = lead >> 6;
    if ((size_t)(end - p) < follow + 1u) return -1;
    uint32_t value = follow ? (lead & 0x0fu) : (lead & 0x3fu);
    for (unsigned i = 0; i < follow; i++)
        value |= (uint32_t)p[1 + i] << (4u + 8u * i);
    if (value < follow + 1u || value > (uint32_t)(end - p)) return -1;
    *body = p + follow + 1u;
    *limit = p + value;
    return 0;
}

static int aml_integer(const uint8_t **cursor, const uint8_t *end,
                       uint64_t *value) {
    const uint8_t *p = *cursor;
    if (p >= end) return -1;
    switch (*p++) {
    case 0x00: *value = 0; break;
    case 0x01: *value = 1; break;
    case 0xff: *value = UINT64_MAX; break;
    case 0x0a:
        if (p + 1 > end) return -1;
        *value = p[0]; p++; break;
    case 0x0b:
        if (p + 2 > end) return -1;
        *value = (uint64_t)p[0] | (uint64_t)p[1] << 8; p += 2; break;
    case 0x0c:
        if (p + 4 > end) return -1;
        *value = (uint64_t)p[0] | (uint64_t)p[1] << 8 |
                 (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24; p += 4; break;
    case 0x0e:
        if (p + 8 > end) return -1;
        *value = 0;
        for (unsigned i = 0; i < 8; i++) *value |= (uint64_t)p[i] << (8u * i);
        p += 8; break;
    default: return -1;
    }
    *cursor = p;
    return 0;
}

static int aml_namestring(const uint8_t **cursor, const uint8_t *end,
                          char out[128], uint8_t *out_len) {
    const uint8_t *p = *cursor;
    size_t n = 0;
    int rooted = 0;
    if (p < end && *p == '\\') { rooted = 1; out[n++] = *p++; }
    while (p < end && *p == '^') {
        if (rooted) return -1;
        if (n + 1 >= 128) return -1;
        out[n++] = *p++;
    }
    unsigned segments = 1;
    if (p < end && *p == 0x00) { p++; segments = 0; }
    else if (p < end && *p == 0x2e) { p++; segments = 2; }
    else if (p < end && *p == 0x2f) {
        p++;
        if (p >= end) return -1;
        segments = *p++;
        if (!segments || segments > 24) return -1;
    }
    if (!segments) return -1;
    for (unsigned i = 0; i < segments; i++) {
        if (p + 4 > end || n + 4 >= 128) return -1;
        for (unsigned j = 0; j < 4; j++) {
            uint8_t ch = p[j];
            int valid = j == 0 ? ((ch >= 'A' && ch <= 'Z') || ch == '_')
                               : ((ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9') || ch == '_');
            if (!valid) return -1;
            out[n++] = (char)ch;
        }
        p += 4;
        if (i + 1 < segments) out[n++] = '.';
    }
    out[n] = 0;
    *cursor = p;
    *out_len = (uint8_t)n;
    return 0;
}

static int parse_prt_package(const uint8_t *p, const uint8_t *end,
                             kira_acpi_prt_entry_t *out, size_t cap,
                             size_t *count) {
    const uint8_t *body, *pkg_end;
    if (aml_pkg_end(p, end, &body, &pkg_end) != 0 || body >= pkg_end) return -1;
    unsigned elements = *body++;
    for (unsigned i = 0; i < elements; i++) {
        if (body >= pkg_end || *body++ != 0x12) return -1;
        const uint8_t *ebody, *eend;
        if (aml_pkg_end(body, pkg_end, &ebody, &eend) != 0 || ebody >= eend) return -1;
        body = ebody;
        unsigned fields = *body++;
        if (fields != 4 || *count >= cap) return -1;
        uint64_t address, pin, source_index;
        if (aml_integer(&body, eend, &address) || aml_integer(&body, eend, &pin) ||
            address > UINT32_MAX || pin > 3) return -1;
        kira_acpi_prt_entry_t row;
        for (size_t z = 0; z < sizeof row; z++) ((uint8_t *)&row)[z] = 0;
        row.address = (uint32_t)address;
        row.pin = (uint8_t)pin;
        if (body >= eend) return -1;
        if (*body == 0x00) {
            body++;
            row.direct_gsi = 1;
        } else {
            if (aml_namestring(&body, eend, row.source_name, &row.source_name_len) != 0)
                return -1;
        }
        if (aml_integer(&body, eend, &source_index) || source_index > UINT32_MAX)
            return -1;
        if (body != eend) return -1;
        row.source_index = (uint32_t)source_index;
        out[(*count)++] = row;
        body = eend;
    }
    return body == pkg_end ? 0 : -1;
}

int kira_acpi_prt_parse(const uint8_t *aml, size_t length,
                        kira_acpi_prt_entry_t *entries, size_t capacity,
                        size_t *entry_count) {
    if (!aml || !entries || !capacity || !entry_count) return -1;
    *entry_count = 0;
    if (length > UINT32_MAX || (uintptr_t)aml > UINTPTR_MAX - length) return -1;
    const uint8_t *end = aml + length;
    int found = 0;
    for (size_t i = 1; i + 5 < length; i++) {
        if (aml[i] != '_' || aml[i+1] != 'P' || aml[i+2] != 'R' || aml[i+3] != 'T')
            continue;
        /* NameOp followed by an optional root prefix and the _PRT NameSeg. */
        if (aml[i-1] != 0x08 &&
            !(aml[i-1] == '\\' && i >= 2 && aml[i-2] == 0x08))
            continue;
        const uint8_t *p = aml + i + 4;
        if (p >= end || *p++ != 0x12) continue;

        const uint8_t *body, *package_end;
        if (aml_pkg_end(p, end, &body, &package_end) != 0) continue;

        /* Parse into a temporary array so malformed packages cannot leave
         * partially appended entries in the caller's result. */
        kira_acpi_prt_entry_t temporary[KIRA_ACPI_PRT_MAX];
        size_t temporary_count = 0;
        if (parse_prt_package(p, end, temporary, KIRA_ACPI_PRT_MAX,
                              &temporary_count) == 0) {
            if (temporary_count > capacity - *entry_count) return -1;
            for (size_t n = 0; n < temporary_count; n++)
                entries[(*entry_count)++] = temporary[n];
            found = 1;
        }
        /* A Package's nested bytes are data, not AML opcodes. Do not mistake
         * strings or integer payloads inside it for another Name(_PRT). */
        i = (size_t)(package_end - aml) - 1u;
    }
    return found ? 0 : -2;
}
