#include <stdio.h>
#include <string.h>
#include "kdrv_manifest.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("kdrv_manifest_test: FAIL line %d: %s\\n", __LINE__, #x); failures++; } } while (0)

static const uint8_t test_signature[64] = {
    0x49,0x15,0x1a,0xa7,0x30,0xdf,0x98,0x14,0xe6,0x27,0x16,0xe9,0x69,0xb6,0xed,0x32,
    0xfe,0xdf,0xb6,0x39,0xf3,0x89,0x68,0xa7,0xde,0x17,0x8e,0xbd,0x81,0x28,0x64,0x8a,
    0xf1,0x9a,0x27,0xe1,0x9a,0xb9,0xa3,0x35,0x83,0xd9,0xcb,0xea,0x9f,0xbc,0x58,0xa5,
    0x51,0x52,0xf9,0xc6,0x96,0x8b,0x6c,0x40,0x04,0x1a,0xfc,0x94,0x92,0x52,0x87,0x01
};
static const uint8_t package_sha[32] = {
    0x7b,0x29,0x6f,0x60,0x4d,0xb8,0x4c,0x9c,0xa2,0x27,0xa1,0x4d,0x11,0xe3,0xe4,0xa0,
    0xe7,0xb9,0x7d,0x2c,0xe9,0x3c,0x10,0x5d,0x89,0x76,0x0e,0xf9,0x9c,0x99,0x0a,0x98
};
static const kdrv_manifest_key_t key = {
    "rfc8032-test-key",
    { 0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
      0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a },
    0
};

int main(void) {
    kdrv_manifest_t m;
    memset(&m, 0, sizeof m);
    m.magic = KDRV_MANIFEST_MAGIC; m.format = KDRV_MANIFEST_FORMAT; m.size = sizeof m;
    memcpy(m.key_id, "rfc8032-test-key", 16);
    memcpy(m.package_sha, package_sha, sizeof package_sha);
    m.version = 7; m.abi = KDRV_MANIFEST_ABI;
    memcpy(m.signature, test_signature, sizeof test_signature);

    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_OK);
    CHECK(kdrv_manifest_verify(0, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_INPUT);
    CHECK(kdrv_manifest_verify(&m, sizeof m - 1, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_SIZE);
    CHECK(kdrv_manifest_verify(&m, sizeof m, 0, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_INPUT);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, 0, 1) == KDRV_MANIFEST_E_INPUT);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &key, 0) == KDRV_MANIFEST_E_INPUT);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &key, KDRV_MANIFEST_MAX_KEYS + 1) == KDRV_MANIFEST_E_INPUT);

    uint8_t other_sha[32]; memcpy(other_sha, package_sha, sizeof other_sha); other_sha[0] ^= 1;
    CHECK(kdrv_manifest_verify(&m, sizeof m, other_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_BINDING);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 8, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_BINDING);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI + 1, &key, 1) == KDRV_MANIFEST_E_BINDING);

    kdrv_manifest_t bad = m; bad.magic ^= 1;
    CHECK(kdrv_manifest_verify(&bad, sizeof bad, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_FORMAT);
    bad = m; bad.flags = 1;
    CHECK(kdrv_manifest_verify(&bad, sizeof bad, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_FORMAT);
    bad = m; bad.signature[12] ^= 1;
    CHECK(kdrv_manifest_verify(&bad, sizeof bad, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_SIGNATURE);
    bad = m; bad.version = 8;
    CHECK(kdrv_manifest_verify(&bad, sizeof bad, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_BINDING);

    kdrv_manifest_key_t revoked = key; revoked.revoked = 1;
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &revoked, 1) == KDRV_MANIFEST_E_REVOKED);
    kdrv_manifest_key_t unknown = key; memcpy(unknown.key_id, "unknown-key-id!!", 16);
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &unknown, 1) == KDRV_MANIFEST_E_KEY);
    kdrv_manifest_key_t duplicate[2] = { key, key };
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, duplicate, 2) == KDRV_MANIFEST_E_KEY);

    if (!failures) puts("kdrv_manifest: canonical format, Ed25519, package/version/ABI binding, revoked/unknown/ambiguous keys and hostile inputs passed");
    return failures != 0;
}
