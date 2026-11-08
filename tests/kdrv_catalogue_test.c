#include "drivers/kdrv/kdrv_catalogue.h"
#include "immune.h"
#include "pk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "kdrv_catalogue_test:%d: %s\n", __LINE__, #x); return 1; } } while (0)

struct test_context {
    uint64_t floor;
    uint64_t now;
    int reject_signature;
    int reject_floor_write;
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
    struct test_context *ctx = (struct test_context *)opaque;
    if (ctx->reject_floor_write) return -1;
    ctx->floor = version;
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

static int real_ed25519(void *ctx, const uint8_t public_key[32],
                           const uint8_t signature[64],
                           const uint8_t *message, size_t message_length)
{
    (void)ctx;
    return ed25519_verify(public_key, message, message_length, signature);
}

static int test_real_ed25519_catalogue(void)
{
    static const char payload[] =
        "KIRA-KDRV-CATALOGUE\t1\t7\n"
        "rtl8139\t1.0\tx86_64\thttps://example.invalid/rtl.kdrv\thttps://example.invalid/rtl.sig\tpublisher-required\t128\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    static const uint8_t public_key[32] = {
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
    };
    static const uint8_t signature[64] = {
        0xd1,0x69,0x61,0x2f,0xca,0x1c,0xcb,0x94,
        0xb3,0x1a,0xf5,0xc0,0xeb,0x4d,0xd9,0xfc,
        0xc3,0x86,0x0c,0xaa,0xd9,0x84,0xaf,0x51,
        0x01,0xf6,0x65,0xef,0xed,0x1a,0x4a,0x7b,
        0x02,0x6c,0x1f,0xec,0x2a,0x11,0x2d,0x24,
        0xc4,0x60,0xbc,0x34,0x83,0xe9,0x5f,0x4a,
        0xd8,0x20,0x74,0x59,0x85,0x98,0x96,0xbf,
        0xd7,0x10,0x0b,0xa1,0xcc,0xf5,0x9d,0x09
    };
    struct kdrv_trust_store store = {0};
    struct kdrv_trust_key key = {0};
    struct kdrv_trust_record record = {0};
    struct kdrv_trust_ops ops = {0};
    struct test_context ctx = { .floor = 6, .now = 500 };
    struct kdrv_catalogue catalogue;

    memcpy(key.id, "TEST-CATALOGUE-1", 16);
    memcpy(key.public_key, public_key, sizeof(public_key));
    key.not_before = 100;
    key.not_after = 1000;
    CHECK(kdrv_trust_add_key(&store, &key) == KDRV_TRUST_OK);

    record.format_version = KDRV_TRUST_FORMAT_VERSION;
    memcpy(record.key_id, key.id, sizeof(key.id));
    record.catalogue_version = 7;
    record.issued_at = 200;
    record.expires_at = 900;
    record.payload_length = sizeof(payload) - 1;
    sha256(payload, sizeof(payload) - 1, record.payload_digest);
    memcpy(record.signature, signature, sizeof(signature));

    ops.context = &ctx;
    ops.ed25519_verify = real_ed25519;
    ops.sha256 = test_sha;
    ops.read_version_floor = read_floor;
    ops.write_version_floor = write_floor;
    ops.trusted_time = trusted_time;

    CHECK(kdrv_catalogue_load(&store, &record, payload, sizeof(payload) - 1,
                              &ops, &catalogue) == KDRV_CATALOGUE_OK);
    CHECK(catalogue.version == 7 && catalogue.count == 1);
    CHECK(ctx.floor == 7);
    return 0;
}

int main(void)
{
    CHECK(test_real_ed25519_catalogue() == 0);
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
    CHECK(ctx.floor == 7); /* accepted catalogue version is persisted */
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

    /* A malformed but correctly signed catalogue must not advance the floor. */
    {
        static const char malformed[] = "KIRA-KDRV-CATALOGUE\t1\t7\ninvalid\n";
        CHECK(make_trust(&store, &record, &ops, &ctx, malformed, sizeof(malformed) - 1));
        CHECK(kdrv_catalogue_load(&store, &record, malformed, sizeof(malformed) - 1,
                                 &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
        CHECK(ctx.floor == 6);
    }

    /* If durable floor storage fails, a valid catalogue is not accepted. */
    CHECK(make_trust(&store, &record, &ops, &ctx, valid, sizeof(valid) - 1));
    ctx.reject_floor_write = 1;
    CHECK(kdrv_catalogue_load(&store, &record, valid, sizeof(valid) - 1,
                             &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(ctx.floor == 6);

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
