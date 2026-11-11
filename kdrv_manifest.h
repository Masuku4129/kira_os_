#ifndef KIRA_KDRV_MANIFEST_H
#define KIRA_KDRV_MANIFEST_H
/* Canonical signed KDRV manifest. The signature covers every byte before signature.
 * This parser is deliberately separate from loader enforcement: unsigned package
 * migration and the production signing pipeline must be completed before fail-closed
 * enforcement is enabled in pkg_load().
 */
#include <stdint.h>

#define KDRV_MANIFEST_MAGIC 0x4D56444Bu /* "KDVM" */
#define KDRV_MANIFEST_FORMAT 1u
#define KDRV_MANIFEST_ABI 1u
#define KDRV_MANIFEST_KEY_ID_SIZE 16u
#define KDRV_MANIFEST_SIGNATURE_SIZE 64u
#define KDRV_MANIFEST_MAX_KEYS 32u

typedef struct {
    uint32_t magic;
    uint16_t format;
    uint16_t size;
    char key_id[KDRV_MANIFEST_KEY_ID_SIZE];
    uint8_t package_sha[32];
    uint32_t version;
    uint32_t abi;
    uint32_t flags;                 /* v1: must be zero */
    uint8_t signature[KDRV_MANIFEST_SIGNATURE_SIZE];
} kdrv_manifest_t;

typedef struct {
    char key_id[KDRV_MANIFEST_KEY_ID_SIZE];
    uint8_t public_key[32];
    uint32_t revoked;               /* nonzero keys are never accepted */
} kdrv_manifest_key_t;

_Static_assert(sizeof(kdrv_manifest_t) == 132, "kdrv_manifest_t");

enum {
    KDRV_MANIFEST_OK = 0,
    KDRV_MANIFEST_E_INPUT = -1,
    KDRV_MANIFEST_E_SIZE = -2,
    KDRV_MANIFEST_E_FORMAT = -3,
    KDRV_MANIFEST_E_BINDING = -4,
    KDRV_MANIFEST_E_KEY = -5,
    KDRV_MANIFEST_E_REVOKED = -6,
    KDRV_MANIFEST_E_SIGNATURE = -7
};

/* Verifies canonical bytes, package hash, version/ABI binding and Ed25519 trust.
 * keys is a kernel-owned trust table; callers must not pass package-controlled keys.
 */
int kdrv_manifest_verify(const void *data, uint32_t len, const uint8_t package_sha[32],
                         uint32_t version, uint32_t abi,
                         const kdrv_manifest_key_t *keys, uint32_t key_count);

#endif
