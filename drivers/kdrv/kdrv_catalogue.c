/*
 * Authenticated KDRV system catalogue parser.
 *
 * The parser first verifies the detached Ed25519 trust record over the
 * complete original file. It then applies a deliberately small, canonical
 * tab-separated grammar. Unknown flags, malformed hex, duplicate package
 * names for one architecture, embedded NULs and overlong lines fail closed.
 */

#include "kdrv_catalogue.h"

static int eq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b)
        return 0;
    while (a[i] && b[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static int copy_field(char *out, size_t capacity,
                      const char *start, size_t length)
{
    size_t i;
    if (!capacity || length == 0 || length >= capacity)
        return -1;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)start[i];
        if (c < 0x21 || c > 0x7e || c == '\\')
            return -1;
        out[i] = (char)c;
    }
    out[length] = '\0';
    return 0;
}

static int parse_u64(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (!n)
        return -1;
    for (i = 0; i < n; ++i) {
        unsigned int d;
        if (s[i] < '0' || s[i] > '9')
            return -1;
        d = (unsigned int)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u)
            return -1;
        v = v * 10u + d;
    }
    *out = v;
    return 0;
}

static int parse_hex_digest(const char *s, size_t n, uint8_t out[32])
{
    size_t i;
    if (n != 64)
        return -1;
    for (i = 0; i < 32; ++i) {
        unsigned int hi, lo;
        char a = s[i * 2], b = s[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = (unsigned int)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned int)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') hi = (unsigned int)(a - 'A' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned int)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned int)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') lo = (unsigned int)(b - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int split_fields(char *line, char **fields, size_t expected)
{
    size_t count = 0;
    char *p = line;
    if (!line || !fields || !expected)
        return -1;
    while (count < expected) {
        char *tab = p;
        fields[count++] = p;
        while (*tab && *tab != '\t')
            ++tab;
        if (count == expected)
            return *tab ? -1 : 0;
        if (!*tab)
            return -1;
        *tab = '\0';
        p = tab + 1;
    }
    return -1;
}

static int valid_identifier(const char *s)
{
    size_t i, n = slen(s);
    if (!n || n >= KDRV_CATALOGUE_NAME_MAX)
        return 0;
    for (i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

static int valid_version(const char *s)
{
    size_t i, n = slen(s);
    if (!n || s[0] < '0' || s[0] > '9')
        return 0;
    for (i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_' || c == '+'))
            return 0;
    }
    return 1;
}

static int valid_url(const char *s)
{
    /* Check the bounded string length before inspecting the scheme prefix. */
    if (!s || slen(s) < 9u)
        return 0;
    return (s[0] == 'h' && s[1] == 't' && s[2] == 't' &&
            s[3] == 'p' && s[4] == 's' && s[5] == ':' &&
            s[6] == '/' && s[7] == '/' && s[8] != '\0');
}

static int parse_entry(char *line, struct kdrv_catalogue_entry *entry)
{
    char *f[8];
    size_t n;
    uint64_t flags, size;

    if (split_fields(line, f, 8) != 0)
        return KDRV_CATALOGUE_INVALID;

    n = slen(f[0]);
    if (copy_field(entry->name, sizeof(entry->name), f[0], n) != 0 ||
        !valid_identifier(entry->name))
        return KDRV_CATALOGUE_INVALID;

    n = slen(f[1]);
    if (copy_field(entry->version, sizeof(entry->version), f[1], n) != 0 ||
        !valid_version(entry->version))
        return KDRV_CATALOGUE_INVALID;

    n = slen(f[2]);
    if (copy_field(entry->architecture, sizeof(entry->architecture), f[2], n) != 0)
        return KDRV_CATALOGUE_INVALID;

    n = slen(f[3]);
    if (copy_field(entry->package_url, sizeof(entry->package_url), f[3], n) != 0 ||
        !valid_url(entry->package_url))
        return KDRV_CATALOGUE_INVALID;

    n = slen(f[4]);
    if (copy_field(entry->signature_url, sizeof(entry->signature_url), f[4], n) != 0 ||
        !valid_url(entry->signature_url))
        return KDRV_CATALOGUE_INVALID;

    if (eq(f[5], "publisher-required"))
        flags = KDRV_CATALOGUE_FLAG_PUBLISHER_REQUIRED;
    else if (eq(f[5], "optional"))
        flags = 0;
    else
        return KDRV_CATALOGUE_INVALID;

    if (parse_u64(f[6], slen(f[6]), &size) != 0 || size == 0 ||
        size > 32ull * 1024ull * 1024ull ||
        parse_hex_digest(f[7], slen(f[7]), entry->package_digest) != 0)
        return KDRV_CATALOGUE_INVALID;

    entry->flags = (uint32_t)flags;
    entry->package_size = size;
    return KDRV_CATALOGUE_OK;
}

int kdrv_catalogue_load(struct kdrv_trust_store *trust,
                        const struct kdrv_trust_record *signature,
                        const void *bytes, size_t length,
                        const struct kdrv_trust_ops *trust_ops,
                        struct kdrv_catalogue *catalogue)
{
    char *copy;
    size_t pos = 0;
    uint64_t header_version = 0;
    int header_seen = 0;

    if (!trust || !signature || !bytes || !length ||
        length > 1024u * 1024u || !trust_ops || !catalogue)
        return KDRV_CATALOGUE_INVALID;

    if (kdrv_trust_verify_catalogue(trust, signature, bytes, length,
                                    trust_ops) != KDRV_TRUST_OK)
        return KDRV_CATALOGUE_UNTRUSTED;

    copy = (char *)bytes;
    for (pos = 0; pos < length; ++pos)
        if (copy[pos] == '\0')
            return KDRV_CATALOGUE_INVALID;

    {
        char *work = (char *)bytes;
        size_t offset = 0;
        catalogue->count = 0;
        catalogue->version = 0;

        while (offset < length) {
            char line[KDRV_CATALOGUE_LINE_MAX];
            size_t n = 0, i;
            char *fields[3];

            while (offset + n < length && work[offset + n] != '\n') {
                if (n + 1 >= sizeof(line))
                    return KDRV_CATALOGUE_INVALID;
                line[n] = work[offset + n];
                ++n;
            }
            if (offset + n == length)
                return KDRV_CATALOGUE_INVALID; /* canonical final LF required */
            line[n] = '\0';
            offset += n + 1;

            if (!n || line[0] == '#')
                continue;

            if (!header_seen) {
                if (split_fields(line, fields, 3) != 0 ||
                    !eq(fields[0], "KIRA-KDRV-CATALOGUE") ||
                    !eq(fields[1], "1") ||
                    parse_u64(fields[2], slen(fields[2]), &header_version) != 0 ||
                    header_version != signature->catalogue_version)
                    return KDRV_CATALOGUE_INVALID;
                catalogue->version = header_version;
                header_seen = 1;
                continue;
            }

            if (catalogue->count == KDRV_CATALOGUE_MAX_ENTRIES)
                return KDRV_CATALOGUE_CAPACITY;

            {
                struct kdrv_catalogue_entry entry;
                int rc = parse_entry(line, &entry);
                if (rc != KDRV_CATALOGUE_OK)
                    return rc;

                for (i = 0; i < catalogue->count; ++i)
                    if (eq(catalogue->entries[i].name, entry.name) &&
                        eq(catalogue->entries[i].architecture,
                           entry.architecture))
                        return KDRV_CATALOGUE_DUPLICATE;

                catalogue->entries[catalogue->count++] = entry;
            }
        }
    }

    if (!header_seen)
        return KDRV_CATALOGUE_INVALID;

    /* Advance only after every signed entry has passed canonical parsing.
       Otherwise a malformed but correctly signed catalogue could permanently
       raise the rollback floor and deny recovery with the previous release. */
    {
        int floor_rc = kdrv_trust_advance_floor(trust_ops, header_version);
        if (floor_rc == KDRV_TRUST_ROLLBACK)
            return KDRV_CATALOGUE_UNTRUSTED;
        if (floor_rc != KDRV_TRUST_OK)
            return KDRV_CATALOGUE_STORAGE_ERROR;
    }

    return KDRV_CATALOGUE_OK;
}

const struct kdrv_catalogue_entry *
kdrv_catalogue_find(const struct kdrv_catalogue *catalogue,
                    const char *name, const char *architecture)
{
    size_t i;
    if (!catalogue || !name || !architecture)
        return 0;
    for (i = 0; i < catalogue->count; ++i)
        if (eq(catalogue->entries[i].name, name) &&
            eq(catalogue->entries[i].architecture, architecture))
            return &catalogue->entries[i];
    return 0;
}
