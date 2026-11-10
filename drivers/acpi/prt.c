/*
 * Bounded AML _PRT package decoder.
 *
 * This parser accepts only constant AML package contents: integer literals,
 * NameStrings, strings and nested Package/VarPackage objects. It does not
 * execute AML methods or evaluate namespace references. Link-device names
 * are handed to a platform resolver; a caller must reject unresolved links.
 *
 * PkgLength includes its own encoded bytes. The cursor exposed to package
 * consumers therefore starts after PkgLength and spans only the remaining
 * package body (NumElements plus package elements).
 */

#include "prt.h"

#define AML_ZERO_OP       0x00u
#define AML_ONE_OP        0x01u
#define AML_BYTE_PREFIX   0x0au
#define AML_WORD_PREFIX   0x0bu
#define AML_DWORD_PREFIX  0x0cu
#define AML_STRING_PREFIX 0x0du
#define AML_QWORD_PREFIX  0x0eu
#define AML_PACKAGE_OP    0x12u
#define AML_VAR_PACKAGE   0x13u
#define AML_ROOT_PREFIX   0x5cu
#define AML_PARENT_PREFIX 0x5eu
#define AML_DUAL_PREFIX   0x2eu
#define AML_MULTI_PREFIX  0x2fu
#define AML_ONES_OP       0xffu
#define AML_PATH_MAX      64u
#define AML_SCOPE_SEGMENTS 12u

struct aml_cursor {
    const uint8_t *p;
    size_t left;
};

struct aml_value {
    uint64_t integer;
    const uint8_t *string;
    size_t string_length;
    struct aml_cursor package;
    char name[AML_PATH_MAX];
    uint8_t type;
    uint8_t package_opcode;
};

enum {
    AML_VALUE_INTEGER = 1,
    AML_VALUE_STRING = 2,
    AML_VALUE_PACKAGE = 3,
    AML_VALUE_NAME = 4
};

static int take(struct aml_cursor *c, size_t n, const uint8_t **out)
{
    if (!c || n > c->left)
        return -1;
    if (out)
        *out = c->p;
    c->p += n;
    c->left -= n;
    return 0;
}

static int decode_pkg_length(struct aml_cursor *c, size_t *body_length)
{
    const uint8_t *p;
    uint8_t lead;
    unsigned int follow, i;
    size_t value, encoded;

    if (!c || !body_length || take(c, 1, &p) != 0)
        return -1;

    lead = p[0];
    follow = lead >> 6;
    encoded = 1u + follow;

    if (follow == 0) {
        value = lead & 0x3fu;
    } else {
        value = lead & 0x0fu;
        for (i = 0; i < follow; ++i) {
            if (take(c, 1, &p) != 0)
                return -1;
            value |= (size_t)p[0] << (4u + i * 8u);
        }
    }

    if (value < encoded)
        return -1;
    value -= encoded;
    if (value == 0 || value > c->left)
        return -1;

    *body_length = value;
    return 0;
}

static int decode_integer(struct aml_cursor *c, uint64_t *value)
{
    const uint8_t *p;
    unsigned int i;
    uint8_t op;
    size_t bytes = 0;
    uint64_t v = 0;

    if (!c || !value || take(c, 1, &p) != 0)
        return -1;
    op = p[0];

    switch (op) {
    case AML_ZERO_OP: *value = 0; return 0;
    case AML_ONE_OP: *value = 1; return 0;
    case AML_ONES_OP: *value = UINT64_MAX; return 0;
    case AML_BYTE_PREFIX: bytes = 1; break;
    case AML_WORD_PREFIX: bytes = 2; break;
    case AML_DWORD_PREFIX: bytes = 4; break;
    case AML_QWORD_PREFIX: bytes = 8; break;
    default: return -1;
    }

    if (take(c, bytes, &p) != 0)
        return -1;
    for (i = 0; i < bytes; ++i)
        v |= (uint64_t)p[i] << (i * 8u);
    *value = v;
    return 0;
}

static int valid_name_char(uint8_t ch, int first)
{
    if (ch >= 'A' && ch <= 'Z')
        return 1;
    if (ch == '_')
        return 1;
    return !first && ch >= '0' && ch <= '9';
}

static int decode_name_segment(struct aml_cursor *c, char out[5])
{
    const uint8_t *p;
    unsigned int i;

    if (take(c, 4, &p) != 0)
        return -1;
    for (i = 0; i < 4; ++i) {
        if (!valid_name_char(p[i], i == 0))
            return -1;
        out[i] = (char)p[i];
    }
    out[4] = '\0';
    return 0;
}

/* scope must be a canonical AML path such as "\\_SB.PCI0". */
static int decode_scope(const char *scope, char segments[][5],
                        unsigned int *count)
{
    const char *p;
    unsigned int n = 0;

    *count = 0;
    if (!scope || !scope[0])
        return 0;

    p = scope;
    if (*p == '\\')
        ++p;

    while (*p) {
        unsigned int i;
        if (n == AML_SCOPE_SEGMENTS)
            return -1;
        for (i = 0; i < 4; ++i) {
            if (!p[i] || p[i] == '.' || !valid_name_char((uint8_t)p[i], i == 0))
                return -1;
            segments[n][i] = p[i];
        }
        segments[n][4] = '\0';
        p += 4;
        ++n;
        if (!*p)
            break;
        if (*p++ != '.' || !*p)
            return -1;
    }

    *count = n;
    return 0;
}

static int decode_name_string(struct aml_cursor *c, const char *scope,
                              char out[AML_PATH_MAX])
{
    char segments[AML_SCOPE_SEGMENTS][5];
    char name_segments[AML_SCOPE_SEGMENTS][5];
    unsigned int base_count = 0, name_count = 0, parents = 0, i;
    const uint8_t *p;
    size_t used = 0;
    int absolute = 0;

    if (!c || !out || !c->left)
        return -1;

    if (c->p[0] == AML_ROOT_PREFIX) {
        if (take(c, 1, &p) != 0)
            return -1;
        absolute = 1;
    } else {
        while (c->left && c->p[0] == AML_PARENT_PREFIX) {
            if (take(c, 1, &p) != 0)
                return -1;
            ++parents;
        }
    }

    if (!c->left)
        return -1;

    if (c->p[0] == 0x00u) {
        (void)take(c, 1, &p);
        out[0] = '\0';
        return 0;
    }

    if (!absolute) {
        if (decode_scope(scope, segments, &base_count) != 0 ||
            parents > base_count)
            return -1;
        base_count -= parents;
    }

    if (c->p[0] == AML_DUAL_PREFIX) {
        (void)take(c, 1, &p);
        name_count = 2;
    } else if (c->p[0] == AML_MULTI_PREFIX) {
        uint8_t n;
        (void)take(c, 1, &p);
        if (take(c, 1, &p) != 0)
            return -1;
        n = p[0];
        if (!n || n > AML_SCOPE_SEGMENTS)
            return -1;
        name_count = n;
    } else {
        name_count = 1;
    }

    if (base_count + name_count > AML_SCOPE_SEGMENTS)
        return -1;
    for (i = 0; i < name_count; ++i)
        if (decode_name_segment(c, name_segments[i]) != 0)
            return -1;

    if (base_count + name_count == 0) {
        out[0] = '\\';
        out[1] = '\0';
        return 0;
    }

    out[used++] = '\\';
    for (i = 0; i < base_count + name_count; ++i) {
        const char *segment = i < base_count ?
            segments[i] : name_segments[i - base_count];
        unsigned int j;

        if (i) {
            if (used + 1 >= AML_PATH_MAX)
                return -1;
            out[used++] = '.';
        }
        for (j = 0; j < 4; ++j) {
            if (used + 1 >= AML_PATH_MAX)
                return -1;
            out[used++] = segment[j];
        }
    }
    out[used] = '\0';
    return 0;
}

static int is_name_string_start(uint8_t op)
{
    return op == AML_ROOT_PREFIX || op == AML_PARENT_PREFIX ||
           op == AML_DUAL_PREFIX || op == AML_MULTI_PREFIX ||
           (op >= 'A' && op <= 'Z') || op == '_';
}

static int decode_value(struct aml_cursor *c, const char *scope,
                        struct aml_value *value)
{
    uint8_t op;
    size_t pkg_length;
    const uint8_t *p;
    struct aml_cursor body;

    if (!c || !value || !c->left)
        return -1;
    op = c->p[0];

    if (op == AML_ZERO_OP || op == AML_ONE_OP || op == AML_ONES_OP ||
        op == AML_BYTE_PREFIX || op == AML_WORD_PREFIX ||
        op == AML_DWORD_PREFIX || op == AML_QWORD_PREFIX) {
        if (decode_integer(c, &value->integer) != 0)
            return -1;
        value->type = AML_VALUE_INTEGER;
        return 0;
    }

    if (op == AML_STRING_PREFIX) {
        (void)take(c, 1, &p);
        value->string = c->p;
        {
            size_t n = 0;
            while (n < c->left && c->p[n] != 0)
                ++n;
            if (n == c->left)
                return -1;
            value->string_length = n;
            c->p += n + 1;
            c->left -= n + 1;
        }
        value->type = AML_VALUE_STRING;
        return 0;
    }

    if (op == AML_PACKAGE_OP || op == AML_VAR_PACKAGE) {
        (void)take(c, 1, &p);
        if (decode_pkg_length(c, &pkg_length) != 0)
            return -1;
        body.p = c->p;
        body.left = pkg_length;
        value->package = body;
        value->package_opcode = op;
        value->type = AML_VALUE_PACKAGE;
        c->p += pkg_length;
        c->left -= pkg_length;
        return 0;
    }

    if (is_name_string_start(op)) {
        if (decode_name_string(c, scope, value->name) != 0)
            return -1;
        value->type = AML_VALUE_NAME;
        return 0;
    }

    return -1;
}

static int package_elements(struct aml_value *package, const char *scope,
                            struct aml_cursor *elements, uint64_t *count)
{
    const uint8_t *p;
    struct aml_value count_value;

    if (!package || package->type != AML_VALUE_PACKAGE ||
        !elements || !count)
        return -1;

    *elements = package->package;
    if (package->package_opcode == AML_PACKAGE_OP) {
        if (take(elements, 1, &p) != 0)
            return -1;
        *count = p[0];
        return 0;
    }

    if (package->package_opcode != AML_VAR_PACKAGE ||
        decode_value(elements, scope, &count_value) != 0 ||
        count_value.type != AML_VALUE_INTEGER)
        return -1;
    *count = count_value.integer;
    return 0;
}

static int route_append(struct kira_acpi_prt_table *table,
                        const struct kira_acpi_prt_route *route)
{
    size_t i;

    if (table->count == KIRA_ACPI_PRT_MAX_ROUTES)
        return KIRA_ACPI_PRT_CAPACITY;

    for (i = 0; i < table->count; ++i) {
        const struct kira_acpi_prt_route *old = &table->routes[i];
        if (old->device == route->device &&
            old->function == route->function && old->pin == route->pin)
            return KIRA_ACPI_PRT_INVALID;
    }

    table->routes[table->count++] = *route;
    return KIRA_ACPI_PRT_OK;
}

int kira_acpi_prt_parse(const uint8_t *aml, size_t aml_size,
                        const char *scope,
                        const struct kira_acpi_prt_ops *ops,
                        struct kira_acpi_prt_table *table)
{
    struct aml_cursor input, entries;
    struct aml_value outer;
    uint64_t count, index;
    int rc = KIRA_ACPI_PRT_INVALID;

    if (!aml || !aml_size || !table)
        return KIRA_ACPI_PRT_INVALID;

    table->count = 0;
    input.p = aml;
    input.left = aml_size;

    if (decode_value(&input, scope, &outer) != 0 ||
        outer.type != AML_VALUE_PACKAGE || input.left != 0 ||
        package_elements(&outer, scope, &entries, &count) != 0)
        goto fail;

    if (count > KIRA_ACPI_PRT_MAX_ROUTES) {
        rc = KIRA_ACPI_PRT_CAPACITY;
        goto fail;
    }

    for (index = 0; index < count; ++index) {
        struct aml_value route_package;
        struct aml_cursor fields;
        struct kira_acpi_prt_route route;
        struct aml_value device, pin, source, source_index;
        uint64_t address, function, device_number, route_count;

        if (decode_value(&entries, scope, &route_package) != 0 ||
            route_package.type != AML_VALUE_PACKAGE ||
            package_elements(&route_package, scope, &fields, &route_count) != 0 ||
            route_count != 4 ||
            decode_value(&fields, scope, &device) != 0 ||
            decode_value(&fields, scope, &pin) != 0 ||
            decode_value(&fields, scope, &source) != 0 ||
            decode_value(&fields, scope, &source_index) != 0 ||
            fields.left != 0 ||
            device.type != AML_VALUE_INTEGER ||
            pin.type != AML_VALUE_INTEGER ||
            source_index.type != AML_VALUE_INTEGER ||
            device.integer > 0xffffffffull || pin.integer > 3 ||
            source_index.integer > 0xffffffffull)
            goto fail;

        address = device.integer;
        device_number = (address >> 16) & 0xffffu;
        function = address & 0xffffu;
        if ((device_number > 31 && device_number != 0xffffu) ||
            (function > 7 && function != 0xffffu))
            goto fail;

        route.device = (uint16_t)device_number;
        route.function = (uint8_t)function;
        route.pin = (uint8_t)pin.integer;
        route.source_index = (uint32_t)source_index.integer;
        route.gsi = 0;
        route.source_name[0] = '\0';

        if (source.type == AML_VALUE_INTEGER && source.integer == 0) {
            route.source_kind = KIRA_ACPI_PRT_GSI;
            route.gsi = route.source_index;
        } else if (source.type == AML_VALUE_NAME && source.name[0]) {
            size_t n = 0;
            while (source.name[n] && n < sizeof(route.source_name))
                ++n;
            if (n == 0 || n >= sizeof(route.source_name))
                goto fail;
            for (size_t j = 0; j < n; ++j)
                route.source_name[j] = source.name[j];
            route.source_name[n] = '\0';
            route.source_kind = KIRA_ACPI_PRT_LINK_DEVICE;
            if (!ops || !ops->resolve_link) {
                rc = KIRA_ACPI_PRT_UNSUPPORTED;
                goto fail;
            }
            if (ops->resolve_link(ops->context, route.source_name,
                                  route.source_index, &route.gsi) != 0) {
                rc = KIRA_ACPI_PRT_RESOLVE_FAILED;
                goto fail;
            }
        } else {
            rc = KIRA_ACPI_PRT_UNSUPPORTED;
            goto fail;
        }

        rc = route_append(table, &route);
        if (rc != KIRA_ACPI_PRT_OK)
            goto fail;
    }

    if (entries.left != 0)
        goto fail;
    return KIRA_ACPI_PRT_OK;

fail:
    table->count = 0;
    return rc;
}
