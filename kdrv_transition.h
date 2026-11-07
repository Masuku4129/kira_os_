#ifndef KIRA_KDRV_TRANSITION_H
#define KIRA_KDRV_TRANSITION_H
#include <stddef.h>
#include <stdint.h>
#include "kdrv_sig.h"

#define KDRV_TRANSITION_MAGIC 0x3152544bu /* KTR1 */
#define KDRV_TRANSITION_FORMAT 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format, size;
    uint8_t old_key_id[KDRV_SIG_KEY_ID_SIZE];
    uint8_t new_key_id[KDRV_SIG_KEY_ID_SIZE];
    uint8_t new_public_key[32];
    uint64_t generation;
    uint64_t not_before, not_after;
    uint8_t signature[64];
} kdrv_transition_t;

_Static_assert(sizeof(kdrv_transition_t) == 160, "KDRV transition ABI");

/* Verifies a transition signed by an existing active publisher key.
 * The caller must persist generation atomically before trusting new_key. */
int kdrv_transition_verify(const kdrv_transition_t *transition,
                           const kdrv_sig_key_t *trusted, uint32_t trusted_count,
                           int64_t now_utc, uint64_t highest_generation,
                           kdrv_sig_key_t *new_key);
#endif
