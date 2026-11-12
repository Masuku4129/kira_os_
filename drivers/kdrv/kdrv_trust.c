/*
 * KDRV signed-catalogue trust and anti-rollback policy.
 *
 * Cryptographic primitives and monotonic storage are platform services:
 * this module refuses verification if either service is unavailable.
 * The signed message is a canonical, fixed-width little-endian encoding of
 * the record fields through payload_digest; the signature bytes are excluded.
 */

#include "kdrv_trust.h"

#define KDRV_SIGNED_HEADER_SIZE 88u

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (i * 8));
}

static int bytes_all_zero(const uint8_t *p, size_t n)
{
    uint8_t value = 0;
    size_t i;
    for (i = 0; i < n; ++i)
        value |= p[i];
    return value == 0;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; ++i)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static struct kdrv_trust_key *find_key(struct kdrv_trust_store *store,
                                       const uint8_t id[KDRV_TRUST_KEY_ID_SIZE])
{
    size_t i;
    for (i = 0; i < store->key_count; ++i)
        if (bytes_equal(store->keys[i].id, id, KDRV_TRUST_KEY_ID_SIZE))
            return &store->keys[i];
    return 0;
}

int kdrv_trust_add_key(struct kdrv_trust_store *store,
                       const struct kdrv_trust_key *key)
{
    size_t i;
    if (!store || !key || store->key_count > KDRV_TRUST_MAX_KEYS ||
        bytes_all_zero(key->id, KDRV_TRUST_KEY_ID_SIZE) ||
        bytes_all_zero(key->public_key, sizeof(key->public_key)) ||
        key->not_after <= key->not_before)
        return KDRV_TRUST_INVALID;

    for (i = 0; i < store->key_count; ++i) {
        if (bytes_equal(store->keys[i].id, key->id, KDRV_TRUST_KEY_ID_SIZE)) {
            /* Key identifiers are immutable: replacement requires revocation. */
            if (!bytes_equal(store->keys[i].public_key, key->public_key, 32))
                return KDRV_TRUST_INVALID;
            if (store->keys[i].revoked)
                return KDRV_TRUST_REVOKED_KEY;
            return KDRV_TRUST_OK;
        }
    }

    if (store->key_count == KDRV_TRUST_MAX_KEYS)
        return KDRV_TRUST_CAPACITY;

    store->keys[store->key_count++] = *key;
    return KDRV_TRUST_OK;
}

int kdrv_trust_revoke_key(struct kdrv_trust_store *store,
                          const uint8_t key_id[KDRV_TRUST_KEY_ID_SIZE])
{
    struct kdrv_trust_key *key;
    if (!store || !key_id || store->key_count > KDRV_TRUST_MAX_KEYS)
        return KDRV_TRUST_INVALID;
    key = find_key(store, key_id);
    if (!key)
        return KDRV_TRUST_UNKNOWN_KEY;
    key->revoked = 1;
    return KDRV_TRUST_OK;
}

int kdrv_trust_advance_floor(const struct kdrv_trust_ops *ops,
                             uint64_t verified_version)
{
    uint64_t current = 0;
    if (!ops || !ops->read_version_floor || !ops->write_version_floor)
        return KDRV_TRUST_STORAGE_ERROR;
    if (ops->read_version_floor(ops->context, &current) != 0)
        return KDRV_TRUST_STORAGE_ERROR;
    if (verified_version < current)
        return KDRV_TRUST_ROLLBACK;
    if (verified_version == current)
        return KDRV_TRUST_OK;
    /* The storage callback must implement an atomic monotonic max update:
       concurrent catalogue loads must never let an older version overwrite a
       newer floor. Read back as a second line of defence against false success. */
    if (ops->write_version_floor(ops->context, verified_version) != 0)
        return KDRV_TRUST_STORAGE_ERROR;
    if (ops->read_version_floor(ops->context, &current) != 0)
        return KDRV_TRUST_STORAGE_ERROR;
    if (current < verified_version)
        return KDRV_TRUST_STORAGE_ERROR;
    /* A newer catalogue may have advanced the floor after our first read.
       The atomic max write protects storage, but accepting this older load
       would still let its caller install stale packages. */
    if (current > verified_version)
        return KDRV_TRUST_ROLLBACK;
    return KDRV_TRUST_OK;
}

int kdrv_trust_verify_catalogue(struct kdrv_trust_store *store,
                                const struct kdrv_trust_record *record,
                                const void *payload, size_t payload_length,
                                const struct kdrv_trust_ops *ops)
{
    struct kdrv_trust_key *key;
    uint8_t digest[KDRV_TRUST_DIGEST_SIZE];
    uint8_t message[KDRV_SIGNED_HEADER_SIZE];
    uint64_t now, floor = 0;
    size_t off = 0;

    if (!store || store->key_count > KDRV_TRUST_MAX_KEYS ||
        !record || (!payload && payload_length) || !ops ||
        !ops->sha256 || !ops->ed25519_verify ||
        !ops->read_version_floor || !ops->trusted_time)
        return KDRV_TRUST_INVALID;

    if (record->format_version != KDRV_TRUST_FORMAT_VERSION ||
        record->payload_length != payload_length ||
        record->expires_at <= record->issued_at)
        return KDRV_TRUST_INVALID;

    key = find_key(store, record->key_id);
    if (!key)
        return KDRV_TRUST_UNKNOWN_KEY;
    if (key->revoked)
        return KDRV_TRUST_REVOKED_KEY;

    now = ops->trusted_time(ops->context);
    if (now < key->not_before || now >= key->not_after ||
        now < record->issued_at || now >= record->expires_at)
        return KDRV_TRUST_INVALID;

    if (ops->sha256(ops->context, payload, payload_length, digest) != 0)
        return KDRV_TRUST_INVALID;
    if (!bytes_equal(digest, record->payload_digest, sizeof(digest)))
        return KDRV_TRUST_BAD_SIGNATURE;

    put_le32(message + off, record->format_version); off += 4;
    put_le32(message + off, record->flags); off += 4;
    for (size_t i = 0; i < KDRV_TRUST_KEY_ID_SIZE; ++i)
        message[off++] = record->key_id[i];
    put_le64(message + off, record->catalogue_version); off += 8;
    put_le64(message + off, record->issued_at); off += 8;
    put_le64(message + off, record->expires_at); off += 8;
    put_le64(message + off, record->payload_length); off += 8;
    for (size_t i = 0; i < KDRV_TRUST_DIGEST_SIZE; ++i)
        message[off++] = record->payload_digest[i];

    if (off != sizeof(message))
        return KDRV_TRUST_INVALID;

    if (ops->ed25519_verify(ops->context, key->public_key,
                            record->signature, message, sizeof(message)) != 0)
        return KDRV_TRUST_BAD_SIGNATURE;

    if (ops->read_version_floor(ops->context, &floor) != 0)
        return KDRV_TRUST_STORAGE_ERROR;
    if (record->catalogue_version < floor)
        return KDRV_TRUST_ROLLBACK;

    return KDRV_TRUST_OK;
}
