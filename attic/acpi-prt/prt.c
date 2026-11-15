/*
 * Bounded AML _PRT package decoder.
 *
 * This parser deliberately accepts only the AML object forms used by the
 * evaluated PCI routing package: integers, strings, package elements and
 * the four-element route package. It never executes AML methods. Dynamic
 * NameString/Method evaluation is the responsibility of the ACPI namespace
 * interpreter; the supplied byte range must be its evaluated _PRT result.
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
#define AML_NULL_NAME     0x00u

struct aml_cursor {
    const uint8_t *p;
    size_t left;
};

struct aml_value {
    uint64_t integer;
    const uint8_t *string;
    size_t string_length;
    struct aml_cursor package;
    uint8_t type;
};

enum {
    AML_VALUE_INTEGER = 1,
    AML_VALUE_STRING = 2,
    AML_VALUE_PACKAGE = 3
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

static int decode_pkg_length(struct aml_cursor *c, size_t *length)
{
    const uint8_t *p;
    uint8_t lead;
    unsigned int follow, i;
    size_t value;

    if (take(c, 1, &p) != 0)
        return -1;
    lead = p[0];
    follow = lead >> 6;

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

    if (value == 0 || value > c->left)
        return -1;
    *length = value;
    return 0;
}

static int decode_integer(struct aml_cursor *c, uint64_t *value)
{
    const uint8_t *p;
    unsigned int i;
    uint8_t op;
    size_t bytes = 0;
    uint64_t v = 0;

    if (take(c, 1, &p) != 0)
        return -1;
    op = p[0];

    switch (op) {
    case AML_ZERO_OP: *value = 0; return 0;
    case AML_ONE_OP: *value = 1; return 0;
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

static int decode_name_segment(struct aml_cursor *c, char out[5])
{
    const uint8_t *p;
    unsigned int i;

    if (take(c, 4, &p) != 0)
        return -1;
    for (i = 0; i < 4; ++i) {
        uint8_t ch = p[i];
        if (!((ch >= 'A' && ch <= 'Z') || ch == '_' ||
              (i && ch >= '0' && ch <= '9')))
            return -1;
        out[i] = (char)ch;
    }
    out[4] = '\0';
    return 0;
}

static int decode_name_string(struct aml_cursor *c, const char *scope,
                              char out[64])
{
    char segments[16][5];
    unsigned int count = 0, i;
    const uint8_t *p;
    size_t used = 0;
    int absolute = 0;

    if (c->left && c->p[0] == AML_ROOT_PREFIX) {
        if (take(c, 1, &p) != 0)
            return -1;
        absolute = 1;
    } else {
        while (c->left && c->p[0] == AML_PARENT_PREFIX) {
            if (take(c, 1, &p) != 0)
                return -1;
        }
    }

    if (!c->left)
        return -1;

    if (c->p[0] == AML_NULL_NAME) {
        (void)take(c, 1, &p);
        out[0] = '\0';
        return 0;
    }

    if (c->p[0] == AML_DUAL_PREFIX) {
        (void)take(c, 1, &p);
        count = 2;
    } else if (c->p[0] == AML_MULTI_PREFIX) {
        uint8_t n;
        (void)take(c, 1, &p);
        if (take(c, 1, &p) != 0)
            return -1;
        n = p[0];
        if (!n || n > 16)
            return -1;
        count = n;
    } else {
        count = 1;
    }

    for (i = 0; i < count; ++i)
        if (decode_name_segment(c, segments[i]) != 0)
            return -1;

    if (!absolute && scope && scope[0]) {
        size_t j = 0;
        while (scope[j] && used + 1 < 64) {
            out[used++] = scope[j++];
        }
        if (used && out[used - 1] != '.')
            out[used++] = '.';
    }

    for (i = 0; i < count; ++i) {
        size_t j;
        if (used && out[used - 1] != '.')
            out[used++] = '.';
        for (j = 0; segments[i][j]; ++j) {
            if (used + 1 >= 64)
                return -1;
            out[used++] = segments[i][j];
        }
    }
    out[used] = '\0';
    return 0;
}

static int decode_value(struct aml_cursor *c, const char *scope,
                        struct aml_value *value)
{
    const uint8_t *p;
    uint8_t op;
    size_t pkg_length;
    struct aml_cursor body;

    if (take(c, 1, &p) != 0)
        return -1;
    op = p[0];
    c->p--;
    c->left++;

    if (op == AML_ZERO_OP || op == AML_ONE_OP ||
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
        if (decode_pkg_length(c, &pkg_length) != 0 || !c->left)
            return -1;
        body.p = c->p;
        body.left = pkg_length;
        value->package = body;
        value->type = AML_VALUE_PACKAGE;
        c->p += pkg_length;
        c->left -= pkg_length;
        return 0;
    }

    (void)scope;
    return -1;
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
    struct aml_cursor input;
    struct aml_value outer;
    uint64_t count;
    uint64_t index;

    if (!aml || !aml_size || !table || !ops || !ops->resolve_link)
        return KIRA_ACPI_PRT_INVALID;

    table->count = 0;
    input.p = aml;
    input.left = aml_size;

    if (decode_value(&input, scope, &outer) != 0 ||
        outer.type != AML_VALUE_PACKAGE)
        return KIRA_ACPI_PRT_INVALID;

    {
        struct aml_cursor count_cursor = outer.package;
        struct aml_value count_value;
        if (decode_value(&count_cursor, scope, &count_value) != 0 ||
            count_value.type != AML_VALUE_INTEGER)
            return KIRA_ACPI_PRT_INVALID;
        count = count_value.integer;
    }

    if (count > KIRA_ACPI_PRT_MAX_ROUTES)
        return KIRA_ACPI_PRT_CAPACITY;

    {
        struct aml_cursor entries = outer.package;
        struct aml_value ignored;
        if (decode_value(&entries, scope, &ignored) != 0)
            return KIRA_ACPI_PRT_INVALID;

        for (index = 0; index < count; ++index) {
            struct aml_value route_package;
            struct aml_cursor fields;
            struct aml_value device, pin, source, source_index;
            struct kira_acpi_prt_route route;
            uint64_t address;

            if (decode_value(&entries, scope, &route_package) != 0 ||
                route_package.type != AML_VALUE_PACKAGE)
                return KIRA_ACPI_PRT_INVALID;

            fields = route_package.package;
            if (decode_value(&fields, scope, &device) != 0 ||
                decode_value(&fields, scope, &pin) != 0 ||
                decode_value(&fields, scope, &source) != 0 ||
                decode_value(&fields, scope, &source_index) != 0 ||
                device.type != AML_VALUE_INTEGER ||
                pin.type != AML_VALUE_INTEGER ||
                source_index.type != AML_VALUE_INTEGER ||
                device.integer > 0xffffffffull || pin.integer > 3 ||
                source_index.integer > 0xffffffffull)
                return KIRA_ACPI_PRT_INVALID;

            address = device.integer;
            route.device = (uint16_t)((address >> 16) & 0xffffu);
            route.function = (uint8_t)(address & 0xffffu);
            if (route.function > 7 && route.function != 0xffffu)
                return KIRA_ACPI_PRT_INVALID;
            route.pin = (uint8_t)pin.integer;
            route.source_index = (uint32_t)source_index.integer;
            route.gsi = 0;
            route.source_name[0] = '\0';

            if (source.type == AML_VALUE_STRING) {
                size_t n = source.string_length;
                if (n >= sizeof(route.source_name))
                    return KIRA_ACPI_PRT_INVALID;
                for (size_t j = 0; j < n; ++j)
                    route.source_name[j] = (char)source.string[j];
                route.source_name[n] = '\0';
                route.source_kind = KIRA_ACPI_PRT_LINK_DEVICE;
                if (ops->resolve_link(ops->context, route.source_name,
                                      route.source_index, &route.gsi) != 0)
                    return KIRA_ACPI_PRT_RESOLVE_FAILED;
            } else if (source.type == AML_VALUE_INTEGER &&
                       source.integer == 0) {
                route.source_kind = KIRA_ACPI_PRT_GSI;
                route.gsi = route.source_index;
            } else {
                return KIRA_ACPI_PRT_UNSUPPORTED;
            }

            {
                int rc = route_append(table, &route);
                if (rc != KIRA_ACPI_PRT_OK)
                    return rc;
            }
        }
    }

    return KIRA_ACPI_PRT_OK;
}
