#include "../drivers/kdrv/kdrv_catalogue.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "kdrv_catalogue_test: FAIL line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

typedef struct {
    uint64_t floor;
    uint64_t now;
    unsigned writes;
    unsigned verifies;
    int fail_write;
    int ignore_write;
    uint64_t race_floor;
} TestTrust;

static int test_hash(void *ctx, const void *data, size_t length, uint8_t digest[32])
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    size_t i;
    (void)ctx;
    for (i = 0; i < length; ++i)
        h = (h ^ p[i]) * 16777619u;
    for (i = 0; i < 32; ++i) {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        digest[i] = (uint8_t)(h >> ((i & 3u) * 8u));
    }
    return 0;
}

static int test_verify(void *ctx, const uint8_t public_key[32],
                       const uint8_t signature[64],
                       const uint8_t *message, size_t message_length)
{
    TestTrust *t = (TestTrust *)ctx;
    (void)public_key;
    (void)signature;
    if (!message || message_length != 88u)
        return -1;
    ++t->verifies;
    return 0; /* The test isolates catalogue policy; Ed25519 is tested separately. */
}

static int read_floor(void *ctx, uint64_t *version)
{
    *version = ((TestTrust *)ctx)->floor;
    return 0;
}

static int write_floor(void *ctx, uint64_t version)
{
    TestTrust *t = (TestTrust *)ctx;
    if (t->fail_write)
        return -1;
    if (!t->ignore_write && version > t->floor)
        t->floor = version;
    /* Simulate a concurrent newer catalogue committing after our read. */
    if (t->race_floor > t->floor)
        t->floor = t->race_floor;
    ++t->writes;
    return 0;
}

static uint64_t trusted_time(void *ctx)
{
    return ((TestTrust *)ctx)->now;
}

static size_t make_catalogue(char *out, size_t capacity, unsigned version,
                             int valid_entry)
{
    int n = snprintf(out, capacity,
        "KIRA-KDRV-CATALOGUE\t1\t%u\n"
        "%s\n",
        version,
        valid_entry == 1
        ? "net\t1.0\tx86_64\thttps://example.org/net.kdrv\thttps://example.org/net.sig\tpublisher-required\t35\t0000000000000000000000000000000000000000000000000000000000000000"
        : valid_entry == 2
        ? "net\t..\tx86_64\thttps://example.org/net.kdrv\thttps://example.org/net.sig\tpublisher-required\t35\t0000000000000000000000000000000000000000000000000000000000000000"
        : valid_entry == 3
        ? "net\t1.0\tx86_64\thttps:///evil.kdrv\thttps://example.org/net.sig\tpublisher-required\t35\t0000000000000000000000000000000000000000000000000000000000000000"
        : "net\t1.0\tx86_64\tx\thttps://example.org/net.sig\tpublisher-required\t35\t0000000000000000000000000000000000000000000000000000000000000000");
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

static void make_record(struct kdrv_trust_record *record,
                        const char *bytes, size_t length, unsigned version,
                        const struct kdrv_trust_ops *ops)
{
    memset(record, 0, sizeof(*record));
    record->format_version = KDRV_TRUST_FORMAT_VERSION;
    record->catalogue_version = version;
    record->issued_at = 100;
    record->expires_at = 1000;
    record->payload_length = length;
    record->key_id[0] = 0x42;
    (void)ops->sha256(ops->context, bytes, length, record->payload_digest);
}

int main(void)
{
    struct kdrv_trust_store store;
    struct kdrv_trust_key key, bad_key;
    struct kdrv_trust_record record;
    struct kdrv_catalogue catalogue;
    TestTrust state = { 0, 500, 0, 0, 0, 0, 0 };
    struct kdrv_trust_ops ops = {
        &state, test_verify, test_hash, read_floor, write_floor, trusted_time, 0
    };
    /* Host mock models the hardware-monotonic contract; it is not hardware evidence. */
    ops.storage_flags = KDRV_TRUST_STORAGE_HW_MONOTONIC;
    char bytes[512];
    size_t length;

    memset(&store, 0, sizeof(store));
    memset(&key, 0, sizeof(key));
    key.id[0] = 0x42;
    key.not_before = 1;
    key.not_after = 2000;
    key.public_key[0] = 1;
    bad_key = key;
    memset(bad_key.id, 0, sizeof bad_key.id);
    CHECK(kdrv_trust_add_key(&store, &bad_key) == KDRV_TRUST_INVALID);
    bad_key = key;
    memset(bad_key.public_key, 0, sizeof bad_key.public_key);
    CHECK(kdrv_trust_add_key(&store, &bad_key) == KDRV_TRUST_INVALID);
    CHECK(kdrv_trust_add_key(&store, &key) == KDRV_TRUST_OK);

    length = make_catalogue(bytes, sizeof(bytes), 5, 1);
    CHECK(length != 0);
    make_record(&record, bytes, length, 5, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_OK);
    CHECK(catalogue.version == 5 && catalogue.count == 1);
    CHECK(state.floor == 5 && state.writes == 1);

    /* A plain writable-disk/CRC backend must not be accepted as a secure rollback anchor. */
    {
        struct kdrv_trust_ops unanchored = ops;
        unanchored.storage_flags = 0;
        CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &unanchored,
                                  &catalogue) == KDRV_CATALOGUE_STORAGE_ERROR);
        CHECK(state.floor == 5 && state.writes == 1);
    }

    /* Re-reading the current version is allowed and does not rewrite storage. */
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_OK);
    CHECK(state.floor == 5 && state.writes == 1);

    /* Reserved trust flags and a zero catalogue version are not accepted. */
    record.flags = 1;
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    record.flags = 0;
    record.catalogue_version = 0;
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    record.catalogue_version = 5;

    /* Expiration is exclusive: the exact expiry instant is already invalid. */
    state.now = 1000;
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(catalogue.count == 0 && catalogue.version == 0);
    state.now = 500;

    /* A validly signed older catalogue cannot roll back the accepted floor. */
    length = make_catalogue(bytes, sizeof(bytes), 4, 1);
    make_record(&record, bytes, length, 4, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(state.floor == 5);

    /* A signed but malformed catalogue must not burn a newer rollback floor. */
    length = make_catalogue(bytes, sizeof(bytes), 6, 0);
    make_record(&record, bytes, length, 6, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
    CHECK(catalogue.count == 0 && catalogue.version == 0);
    CHECK(state.floor == 5);

    /* Signed catalogue metadata cannot smuggle a dot-segment version. */
    length = make_catalogue(bytes, sizeof(bytes), 6, 2);
    make_record(&record, bytes, length, 6, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
    CHECK(state.floor == 5);

    /* HTTPS scheme without an authority is not a fetchable package URL. */
    length = make_catalogue(bytes, sizeof(bytes), 6, 3);
    make_record(&record, bytes, length, 6, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_INVALID);
    CHECK(state.floor == 5);

    /* Durable floor failure is not reported as successful catalogue acceptance. */
    length = make_catalogue(bytes, sizeof(bytes), 6, 1);
    make_record(&record, bytes, length, 6, &ops);
    state.fail_write = 1;
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_STORAGE_ERROR);
    CHECK(state.floor == 5);
    state.fail_write = 0;
    state.ignore_write = 1; /* callback falsely reports success without persisting */
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_STORAGE_ERROR);
    CHECK(state.floor == 5);
    state.ignore_write = 0;

    /* A newer catalogue can win between our floor read and atomic max write.
       The old catalogue must not be returned as accepted even though storage
       correctly keeps the higher floor. */
    state.race_floor = 7;
    length = make_catalogue(bytes, sizeof(bytes), 6, 1);
    make_record(&record, bytes, length, 6, &ops);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);
    CHECK(state.floor == 7);
    state.race_floor = 0;

    /* Corrupted trust-store metadata is rejected before key-array traversal. */
    store.key_count = KDRV_TRUST_MAX_KEYS + 1u;
    CHECK(kdrv_trust_add_key(&store, &key) == KDRV_TRUST_INVALID);
    CHECK(kdrv_catalogue_load(&store, &record, bytes, length, &ops, &catalogue) == KDRV_CATALOGUE_UNTRUSTED);

    puts("kdrv_catalogue: signature, canonical parsing and durable anti-rollback policy PASS");
    return 0;
}
