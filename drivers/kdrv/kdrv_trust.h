#ifndef KIRA_KDRV_TRUST_H
#define KIRA_KDRV_TRUST_H

#include <stddef.h>
#include <stdint.h>

#define KDRV_TRUST_KEY_ID_SIZE 16u
#define KDRV_TRUST_SIGNATURE_SIZE 64u
#define KDRV_TRUST_DIGEST_SIZE 32u
#define KDRV_TRUST_MAX_KEYS 32u
#define KDRV_TRUST_MAX_REVOCATIONS 128u
#define KDRV_TRUST_FORMAT_VERSION 1u

/* Only a platform backend backed by TPM/secure-element/firmware monotonic state may set this bit. */
#define KDRV_TRUST_STORAGE_HW_MONOTONIC 0x00000001u
#define KDRV_TRUST_STORAGE_KNOWN_FLAGS KDRV_TRUST_STORAGE_HW_MONOTONIC

enum kdrv_trust_result {
    KDRV_TRUST_OK = 0,
    KDRV_TRUST_INVALID = -1,
    KDRV_TRUST_UNKNOWN_KEY = -2,
    KDRV_TRUST_REVOKED_KEY = -3,
    KDRV_TRUST_BAD_SIGNATURE = -4,
    KDRV_TRUST_ROLLBACK = -5,
    KDRV_TRUST_STORAGE_ERROR = -6,
    KDRV_TRUST_CAPACITY = -7
};

struct kdrv_trust_key {
    uint8_t id[KDRV_TRUST_KEY_ID_SIZE];
    uint8_t public_key[32];
    uint64_t not_before;
    uint64_t not_after;
    uint8_t revoked;
};

struct kdrv_trust_record {
    uint32_t format_version;
    uint32_t flags;
    uint8_t key_id[KDRV_TRUST_KEY_ID_SIZE];
    uint64_t catalogue_version;
    uint64_t issued_at;
    uint64_t expires_at;
    uint64_t payload_length;
    uint8_t payload_digest[KDRV_TRUST_DIGEST_SIZE];
    uint8_t signature[KDRV_TRUST_SIGNATURE_SIZE];
};

/* Callers must serialize catalogue verification/parse/acceptance with package
   activation. The monotonic storage callback prevents floor downgrade, while
   serialization prevents a stale concurrent consumer from using an older list. */
struct kdrv_trust_ops {
    void *context;
    int (*ed25519_verify)(void *context, const uint8_t public_key[32],
                          const uint8_t signature[64],
                          const uint8_t *message, size_t message_length);
    int (*sha256)(void *context, const void *data, size_t length,
                  uint8_t digest[32]);
    int (*read_version_floor)(void *context, uint64_t *version);
    /* Must atomically persist max(current, version); never permit concurrent downgrade. */
    int (*write_version_floor)(void *context, uint64_t version);
    uint64_t (*trusted_time)(void *context);
    /* Explicit platform assertion; ordinary writable disk/CRC storage is insufficient. */
    uint32_t storage_flags;
};

struct kdrv_trust_store {
    struct kdrv_trust_key keys[KDRV_TRUST_MAX_KEYS];
    size_t key_count;
};

int kdrv_trust_add_key(struct kdrv_trust_store *store,
                       const struct kdrv_trust_key *key);
int kdrv_trust_revoke_key(struct kdrv_trust_store *store,
                          const uint8_t key_id[KDRV_TRUST_KEY_ID_SIZE]);
int kdrv_trust_verify_catalogue(struct kdrv_trust_store *store,
                                const struct kdrv_trust_record *record,
                                const void *payload, size_t payload_length,
                                const struct kdrv_trust_ops *ops);
int kdrv_trust_advance_floor(const struct kdrv_trust_ops *ops,
                             uint64_t verified_version);

#endif
