#ifndef KIRA_KDRV_CATALOGUE_H
#define KIRA_KDRV_CATALOGUE_H

#include <stddef.h>
#include <stdint.h>
#include "kdrv_trust.h"

#define KDRV_CATALOGUE_MAX_ENTRIES 512u
#define KDRV_CATALOGUE_LINE_MAX 512u
#define KDRV_CATALOGUE_NAME_MAX 64u
#define KDRV_CATALOGUE_URL_MAX 256u
#define KDRV_CATALOGUE_VERSION_MAX 32u
#define KDRV_CATALOGUE_FLAG_PUBLISHER_REQUIRED 0x00000001u

struct kdrv_catalogue_entry {
    char name[KDRV_CATALOGUE_NAME_MAX];
    char version[KDRV_CATALOGUE_VERSION_MAX];
    char architecture[16];
    char package_url[KDRV_CATALOGUE_URL_MAX];
    char signature_url[KDRV_CATALOGUE_URL_MAX];
    uint32_t flags;
    uint8_t package_digest[32];
    uint64_t package_size;
};

struct kdrv_catalogue {
    uint64_t version;
    size_t count;
    struct kdrv_catalogue_entry entries[KDRV_CATALOGUE_MAX_ENTRIES];
};

enum kdrv_catalogue_status {
    KDRV_CATALOGUE_OK = 0,
    KDRV_CATALOGUE_INVALID = -1,
    KDRV_CATALOGUE_UNTRUSTED = -2,
    KDRV_CATALOGUE_CAPACITY = -3,
    KDRV_CATALOGUE_DUPLICATE = -4,
    KDRV_CATALOGUE_STORAGE_ERROR = -5
};

/*
 * Authenticate the exact catalogue bytes before parsing any entry.
 * The detached record must sign the complete byte stream using the KDRV
 * trust-record format. After every entry passes parsing, the monotonic
 * catalogue-version floor is durably advanced; storage failure rejects load.
 * The grammar is strict UTF-8-compatible ASCII:
 *
 * KIRA-KDRV-CATALOGUE<TAB>1<TAB>version<LF>
 * name<TAB>version<TAB>arch<TAB>package-url<TAB>signature-url<TAB>flags<TAB>size<TAB>sha256hex<LF>
 */
int kdrv_catalogue_load(struct kdrv_trust_store *trust,
                        const struct kdrv_trust_record *signature,
                        const void *bytes, size_t length,
                        const struct kdrv_trust_ops *trust_ops,
                        struct kdrv_catalogue *catalogue);
const struct kdrv_catalogue_entry *
kdrv_catalogue_find(const struct kdrv_catalogue *catalogue,
                    const char *name, const char *architecture);

#endif
