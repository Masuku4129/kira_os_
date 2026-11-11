#include "kdrv_manifest.h"
#include "pk.h"
#include <stddef.h>

static void copy_bytes(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int kdrv_manifest_verify(const void *data, uint32_t len, const uint8_t package_sha[32],
                         uint32_t version, uint32_t abi,
                         const kdrv_manifest_key_t *keys, uint32_t key_count) {
    kdrv_manifest_t m;
    if (!data || !package_sha || !keys || !key_count || key_count > KDRV_MANIFEST_MAX_KEYS)
        return KDRV_MANIFEST_E_INPUT;
    if (len != sizeof m) return KDRV_MANIFEST_E_SIZE;
    copy_bytes(&m, data, sizeof m);
    if (m.magic != KDRV_MANIFEST_MAGIC || m.format != KDRV_MANIFEST_FORMAT || m.size != sizeof m || m.flags != 0)
        return KDRV_MANIFEST_E_FORMAT;
    if (!m.version || !version || !abi || m.version != version || m.abi != abi ||
        !bytes_equal(m.package_sha, package_sha, sizeof m.package_sha))
        return KDRV_MANIFEST_E_BINDING;

    const kdrv_manifest_key_t *key = 0;
    for (uint32_t i = 0; i < key_count; i++) {
        if (bytes_equal((const uint8_t *)keys[i].key_id, (const uint8_t *)m.key_id, KDRV_MANIFEST_KEY_ID_SIZE)) {
            if (key) return KDRV_MANIFEST_E_KEY; /* duplicate IDs make trust selection ambiguous */
            key = &keys[i];
        }
    }
    if (!key) return KDRV_MANIFEST_E_KEY;
    if (key->revoked) return KDRV_MANIFEST_E_REVOKED;
    if (ed25519_verify(key->public_key, (const uint8_t *)data,
                       (size_t)offsetof(kdrv_manifest_t, signature), m.signature) != 0)
        return KDRV_MANIFEST_E_SIGNATURE;
    return KDRV_MANIFEST_OK;
}
