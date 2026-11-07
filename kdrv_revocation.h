#ifndef KIRA_KDRV_REVOCATION_H
#define KIRA_KDRV_REVOCATION_H
#include <stdint.h>
#include "kdrv_sig.h"

#define KDRV_REVOCATION_MAGIC 0x3156524bu /* KRV1 */
#define KDRV_REVOCATION_FORMAT 1u
#define KDRV_REVOCATION_MAX_IDS 32u
#define KDRV_REVOCATION_MAX_CHAIN 32u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format, size;
    uint64_t generation;
    uint64_t not_before, not_after;
    uint8_t issuer_key_id[KDRV_SIG_KEY_ID_SIZE];
    uint16_t revoked_count, reserved;
    uint8_t revoked_ids[KDRV_REVOCATION_MAX_IDS][KDRV_SIG_KEY_ID_SIZE];
    uint8_t signature[64];
} kdrv_revocation_t;

/* Verifies a signed revocation statement and returns a modified trust snapshot.
 * Persist the returned keys and generation atomically before accepting packages. */
int kdrv_revocation_apply(const kdrv_revocation_t *statement,
                          const kdrv_sig_key_t *trusted, uint32_t trusted_count,
                          int64_t now_utc, uint64_t highest_generation,
                          kdrv_sig_key_t *out_keys, uint32_t out_capacity,
                          uint32_t *out_count, uint64_t *out_generation);
#endif
