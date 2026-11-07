#include "drivers/kdrv/kdrv_catalogue.h"
#include "immune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "kdrv_catalogue_test:%d: %s\n", __LINE__, #x); return 1; } } while (0)

struct test_context {
    uint64_t floor;
    uint64_t now;
    int reject_signature;
};

static int test_sha(void *ctx, const void *data, size_t length, uint8_t digest[32])
{
    (void)ctx;
    sha256(data, length, digest);
    return 0;
}

static int test_ed25519(void *opaque, const uint8_t public_key[32],
                        const uint8_t signature[64],
                        const uint8_t *message, size_t message_length)
{
    struct test_context *ctx = (struct test_context *)opaque;
    size_t i;
    (void)public_key;
    if (ctx->reject_signature || message_length != 88 ||
        message[0] != KDRV_TRUST_FORMAT_VERSION)
        return -1;
    for (i = 0; i < 64; ++i)
        if (signature[i] != 0xa5)
            return -1;
    return 0;
}

static int read_floor(void *opaque, uint64_t *version)
{
    *version = ((struct test_context *)opaque)->floor;
    return 0;
}

static int write_floor(void *opaque, uint64_t version)
{
    ((struct test_context *)opaque)->floor = version;
    return 0;
}

static uint64_t trusted_time(void *opaque)
{
    return ((struct test_context *)opaque)->now;
}

static int make_trust(struct kdrv_trust_store *store,
                       struct kdrv_trust_record *record,
                       struct kdrv_trust_ops *ops,
                       struct test_context *ctx,
                       const char *payload, size_t length)
{
    struct kdrv_trust_key key;
    size_t i;
    memset(store, 0, sizeof(*store));
    memset(&key, 0, sizeof(key));
    memset(record, 0, sizeof(*record));
    memset(ctx, 0, sizeof(*ctx));
    memset(ops, 0, sizeof(*ops));
    memcpy(key.id, "TEST-CATALOGUE-1", 16);
    for (i = 0; i < sizeof(key.public_key); ++i)
        key.public_key[i] = (uint8_t)(i + 1);
    key.not_before = 100;
    key.not_after = 1000;
    if (kdrv_trust_add_key(store, &key) != KDRV_TRUST_OK) return 0;
    record->format_version = KDRV_TRUST_FORMAT_VERSION;
    memcpy(record->key_id, key.id, sizeof(key.id));
    record->catalogue_version = 7;
    record->issued_at = 200;
    record->expires_at = 900;
    record->payload_length = length;
    sha256(payload, length, record->payload_digest);
    memset(record->signature, 0xa5, sizeof(record->signature));
    ctx->floor = 6;
    ctx->now = 500;
    ops->context = ctx;
    ops->ed25519_verify = test_ed25519;
    ops->sha256 = test_sha;
    ops->read_version_floor = read_floor;
    ops->write_version_floor = write_floor;
    ops->trusted_time = trusted_time;
    return 1;
}

int main(void)
{
    static const char valid[] =
        "KIRA-KDRV-CATALOGUE\t1\t7\n"
        "rtl8139\t1.0\tx86_64\thttps://example.invalid/rtl.kdrv\thttps://example.invalid/rtl.sig\tpublisher-required\t128\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    char mutable_payload[sizeof(valid)];
    struct kdrv_trust_store store;
    struct kdrv_trust_record record;
    struct kdrv_trust_ops ops;
    struct kdrv_catalogue catalogue;
    struct test_context ctx;
    const struct kdrv_catalogue_entry *entry;

    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_OK);
    CHECK(catalogue.version == 7 && catalogue.count == 1);
    entry = kdrv_catalogue_find(&catalogue, "rtl8139", "x86_64");
    CHECK(entry != NULL);
    CHECK(entry->flags == KDRV_CATALOGUE_FLAG_PUBLISHER_REQUIRED);
    CHECK(entry->package_size == 128 && entry->package_digest[0] == 0x01);

    /* Catalogue verification must reject modified bytes before parsing. */
    memcpy(mutable_payload, valid, sizeof(valid));
    mutable_payload[sizeof(valid) - 3] ^= 1;
    CHECK(kdrv_catalogue_load(&store, &record, mutable_payload, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);

    /* The canonical parser rejects missing final LF, embedded NUL and unknown policy. */
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 2));
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 2,
                             &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    memcpy(mutable_payload, valid, sizeof(valid));
    mutable_payload[4] = '\0';
    sha256(mutable_payload, sizeof(valid) - 1, record.payload_digest);
    CHECK(kdrv_catalogue_load(&store, &record, mutable_payload, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_INVALID);

    {
        static const char bad_policy[] =
            "KIRA-KDRV-CATALOGUE\t1\t7\n"
            "rtl8139\t1.0\tx86_64\thttps://example.invalid/rtl.kdrv\thttps://example.invalid/rtl.sig\tmaybe\t128\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        CHECK(make_trust(&store, &record, &ops, &ctx, bad_policy, sizeof(bad_policy) - 1));
        CHECK(kdrv_catalogue_load(&store, &record, bad_policy, sizeof(bad_policy) - 1,
                                 &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
    }

    /* A cryptographic verifier rejection must never fall through to parsing. */
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    ctx.reject_signature = 1;
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    record.key_id[0] ^= 0x80;
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);

    /* A revoked publisher, expired metadata and stale catalogue are fail-closed. */
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    CHECK(kdrv_trust_revoke_key(&store, record.key_id) == KDRV_TRUST_OK);
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    ctx.now = 901;
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    ctx.floor = 8;
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);

    puts("kdrv_catalogue: authenticated parsing, publisher policy, tamper, revocation, expiry and rollback checks passed");
    return 0;
}
