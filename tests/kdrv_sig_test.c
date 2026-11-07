#include "kdrv_sig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static const uint8_t package[] = "KIRA KDRV signature test package v1";
static const kdrv_sig_key_t test_key = {
    "TEST-ED25519-001",
    {
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
    },
    0
};

static const kdrv_sig_t valid_signature = {
    .magic = KDRV_SIG_MAGIC,
    .format = KDRV_SIG_FORMAT,
    .size = sizeof(kdrv_sig_t),
    .key_id = "TEST-ED25519-001",
    .package_sha256 = {
        0xcf,0x4d,0x54,0x9e,0x5a,0x1a,0x3c,0x6e,
        0x27,0x97,0x31,0xeb,0x35,0x46,0x37,0xc7,
        0xc1,0xc3,0x4a,0x4f,0xac,0x58,0xae,0xb0,
        0x0c,0xa8,0x95,0x12,0x92,0x82,0x1c,0x80
    },
    .package_size = 35,
    .not_before = 1700000000ULL,
    .not_after = 4102444800ULL,
    .signature = {
        0x4c,0x6c,0xfc,0xfa,0x69,0x9f,0x6e,0x52,
        0x84,0x43,0x48,0xbb,0x32,0x6f,0x8c,0x48,
        0x90,0xf9,0x0d,0xb2,0x98,0xd7,0xdb,0x86,
        0xf5,0x0a,0x34,0xd0,0xdf,0x27,0x29,0x76,
        0x06,0xb2,0xce,0xa3,0xc3,0x35,0x66,0x2d,
        0x52,0x5c,0x0b,0xee,0x03,0xad,0x7d,0x46,
        0x54,0x98,0x54,0x6b,0x1b,0x16,0x8d,0x31,
        0xb7,0x7a,0x2d,0x7f,0x46,0x0c,0xb9,0x0f
    }
};

int main(void) {
    CHECK(sizeof(kdrv_sig_t) == 144);
    CHECK(kdrv_sig_header_check(&valid_signature, sizeof valid_signature) == KDRV_SIG_OK);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &valid_signature, sizeof valid_signature,
                          &test_key, 1, 1800000000LL) == KDRV_SIG_OK);

    uint8_t altered[sizeof package];
    memcpy(altered, package, sizeof package);
    altered[0] ^= 1;
    CHECK(kdrv_sig_verify(altered, sizeof altered - 1, &valid_signature, sizeof valid_signature,
                          &test_key, 1, 1800000000LL) == KDRV_SIG_E_HASH);

    kdrv_sig_t s = valid_signature;
    s.signature[0] ^= 1;
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, 1800000000LL) == KDRV_SIG_E_SIGNATURE);
    s = valid_signature; s.key_id[0] ^= 1;
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, 1800000000LL) == KDRV_SIG_E_UNKNOWN_KEY);
    s = valid_signature;
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, 1600000000LL) == KDRV_SIG_E_NOT_YET);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, 4102444800LL) == KDRV_SIG_E_EXPIRED);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, 0) == KDRV_SIG_E_TIME);
    CHECK(kdrv_sig_verify(package, sizeof package - 2, &s, sizeof s, &test_key, 1, 1800000000LL) == KDRV_SIG_E_PACKAGE_SIZE);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s - 1, &test_key, 1, 1800000000LL) == KDRV_SIG_E_SIZE);

    s = valid_signature; s.not_after = s.not_before;
    CHECK(kdrv_sig_header_check(&s, sizeof s) == KDRV_SIG_E_TIME);
    s = valid_signature; s.magic = 0;
    CHECK(kdrv_sig_header_check(&s, sizeof s) == KDRV_SIG_E_FORMAT);

    kdrv_sig_key_t invalid_flags = test_key;
    invalid_flags.flags = 2u;
    s = valid_signature;
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &invalid_flags, 1, 1800000000LL) == KDRV_SIG_E_TRUST_STORE);
    kdrv_sig_key_t zero_public = test_key;
    memset(zero_public.public_key, 0, sizeof zero_public.public_key);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &zero_public, 1, 1800000000LL) == KDRV_SIG_E_TRUST_STORE);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 1, -1) == KDRV_SIG_E_TIME);

    kdrv_sig_key_t revoked = test_key;
    revoked.flags = KDRV_SIG_KEY_REVOKED;
    s = valid_signature;
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &revoked, 1, 1800000000LL) == KDRV_SIG_E_REVOKED);

    kdrv_sig_key_t duplicate[2] = { test_key, test_key };
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, duplicate, 2, 1800000000LL) == KDRV_SIG_E_TRUST_STORE);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, duplicate, KDRV_SIG_MAX_KEYS + 1, 1800000000LL) == KDRV_SIG_E_ARGUMENT);
    CHECK(kdrv_sig_verify(0, sizeof package - 1, &s, sizeof s, &test_key, 1, 1800000000LL) == KDRV_SIG_E_ARGUMENT);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, 0, 1, 1800000000LL) == KDRV_SIG_E_ARGUMENT);
    CHECK(kdrv_sig_verify(package, sizeof package - 1, &s, sizeof s, &test_key, 0, 1800000000LL) == KDRV_SIG_E_ARGUMENT);
    s = valid_signature;
    memset(s.key_id, 0, sizeof s.key_id);
    CHECK(kdrv_sig_header_check(&s, sizeof s) == KDRV_SIG_E_FORMAT);

    puts("kdrv_sig: Ed25519 package authentication, validity window, key lookup and revocation passed");
    return 0;
}
