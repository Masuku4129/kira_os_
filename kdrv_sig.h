#ifndef KIRA_KDRV_SIG_H
#define KIRA_KDRV_SIG_H
#include <stddef.h>
#include <stdint.h>

#define KDRV_SIG_MAGIC 0x31474953u /* "SIG1" */
#define KDRV_SIG_FORMAT 1u
#define KDRV_SIG_KEY_ID_SIZE 16u
#define KDRV_SIG_MAX_KEYS 32u
#define KDRV_SIG_KEY_REVOKED 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format;
    uint16_t size;
    uint8_t key_id[KDRV_SIG_KEY_ID_SIZE];
    uint8_t package_sha256[32];
    uint64_t package_size;
    uint64_t not_before;   /* Unix UTC seconds, inclusive */
    uint64_t not_after;    /* Unix UTC seconds, exclusive */
    uint8_t signature[64];
} kdrv_sig_t;

_Static_assert(sizeof(kdrv_sig_t) == 144, "kdrv_sig_t");

typedef struct {
    uint8_t id[KDRV_SIG_KEY_ID_SIZE];
    uint8_t public_key[32];
    uint32_t flags;
} kdrv_sig_key_t;

enum {
    KDRV_SIG_OK = 0,
    KDRV_SIG_E_ARGUMENT = -1,
    KDRV_SIG_E_SIZE = -2,
    KDRV_SIG_E_FORMAT = -3,
    KDRV_SIG_E_TIME = -4,
    KDRV_SIG_E_NOT_YET = -5,
    KDRV_SIG_E_EXPIRED = -6,
    KDRV_SIG_E_PACKAGE_SIZE = -7,
    KDRV_SIG_E_HASH = -8,
    KDRV_SIG_E_UNKNOWN_KEY = -9,
    KDRV_SIG_E_REVOKED = -10,
    KDRV_SIG_E_TRUST_STORE = -11,
    KDRV_SIG_E_SIGNATURE = -12,
    KDRV_SIG_E_IO = -13
};

enum { KDRV_AUTH_LOCAL = 0, KDRV_AUTH_PUBLISHER = 1 };

int kdrv_sig_header_check(const void* signature, size_t signature_len);
int kdrv_sig_verify(const void* package, size_t package_len,
                    const void* signature, size_t signature_len,
                    const kdrv_sig_key_t* keys, uint32_t key_count,
                    int64_t now_utc);
const char* kdrv_sig_strerror(int error);

#endif
