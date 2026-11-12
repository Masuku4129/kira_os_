#include "iommu_vtd.h"
#include <stddef.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int checksum_ok(const uint8_t *p, uint32_t n) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Device Scope: type, length, reserved[2], enumeration ID, start bus,
 * followed by one or more (device,function) path elements. */
static int parse_scopes(const uint8_t *p, uint32_t length, uint16_t segment,
                        vtd_dmar_info_t *out, uint16_t *first, uint16_t *count) {
    (void)segment;
    *first = out->scope_count;
    *count = 0;
    uint32_t off = 0;
    while (off < length) {
        if (length - off < 6) return -1;
        const uint8_t *s = p + off;
        uint8_t sl = s[1];
        if (sl < 8 || sl > length - off || ((sl - 6) & 1)) return -2;
        uint32_t paths = (sl - 6) / 2;
        if (!paths || paths > VTD_MAX_PATH) return -3;
        if (out->scope_count >= VTD_MAX_SCOPES) return -4;
        vtd_scope_t *d = &out->scopes[out->scope_count++];
        d->type = s[0]; d->length = sl; d->enumeration_id = s[4]; d->start_bus = s[5];
        d->path_count = (uint8_t)paths;
        for (uint32_t i = 0; i < paths; ++i) {
            d->path[i].device = s[6 + i * 2];
            d->path[i].function = s[7 + i * 2];
            if (d->path[i].device > 31 || d->path[i].function > 7) return -5;
        }
        ++*count;
        off += sl;
    }
    return 0;
}

int vtd_dmar_parse(const void *table, uint32_t available, vtd_dmar_info_t *out) {
    if (!table || !out) return -1;
    const uint8_t *b = (const uint8_t *)table;
    if (available < 48) return -2;
    if (b[0] != 'D' || b[1] != 'M' || b[2] != 'A' || b[3] != 'R') return -3;
    uint32_t total = rd32(b + 4);
    if (total < 48 || total > available) return -4;
    if (!checksum_ok(b, total)) return -5;

    /* Clear output only after the input header and checksum are accepted. */
    uint8_t *z = (uint8_t *)out;
    for (size_t i = 0; i < sizeof(*out); ++i) z[i] = 0;
    out->host_address_width = b[36];
    out->flags = b[37];

    uint32_t off = 48;
    while (off < total) {
        if (total - off < 4) return -6;
        const uint8_t *s = b + off;
        uint16_t type = rd16(s), len = rd16(s + 2);
        if (len < 4 || len > total - off) return -7;

        if (type == 0) { /* DRHD */
            if (len < 16 || out->unit_count >= VTD_MAX_UNITS) return -8;
            vtd_unit_t *u = &out->units[out->unit_count];
            u->flags = s[4]; u->include_all = (uint8_t)(s[4] & 1);
            u->segment = rd16(s + 6); u->register_base = rd64(s + 8);
            if (!u->register_base || (u->register_base & 0xFFF)) return -9;
            int rc = parse_scopes(s + 16, len - 16, u->segment, out, &u->first_scope, &u->scope_count);
            if (rc) return -10 + rc;
            ++out->unit_count;
        } else if (type == 1) { /* RMRR */
            if (len < 24 || out->rmrr_count >= VTD_MAX_RMRR) return -16;
            vtd_rmrr_t *r = &out->rmrr[out->rmrr_count];
            r->segment = rd16(s + 6); r->base = rd64(s + 8); r->limit = rd64(s + 16);
            if ((r->base & 0xFFF) || (r->limit & 0xFFF) != 0xFFF || r->limit < r->base) return -17;
            int rc = parse_scopes(s + 24, len - 24, r->segment, out, &r->first_scope, &r->scope_count);
            if (rc) return -18 + rc;
            ++out->rmrr_count;
        } else if (type == 2) { /* ATSR */
            if (len < 8) return -19;
            uint16_t ignored_first, ignored_count;
            int rc = parse_scopes(s + 8, len - 8, rd16(s + 6), out, &ignored_first, &ignored_count);
            if (rc) return -20 + rc;
        } else if (type == 3) { /* RHSA */
            if (len < 20) return -21;
        } else if (type == 4) { /* ANDD */
            if (len < 9 || s[len - 1] != 0) return -22;
        } else if (type == 5) { /* SATC */
            if (len < 8) return -23;
        }
        /* Unknown future structures are skipped by their validated length. */
        off += len;
    }
    return 0;
}
