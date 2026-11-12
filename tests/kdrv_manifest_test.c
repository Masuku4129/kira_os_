#include <stdio.h>
#include <string.h>
#include "kdrv_manifest.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("kdrv_manifest_test: FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static const uint8_t package_sha[32] = {
    0x63,0x0d,0xcd,0x29,0x66,0xc4,0x33,0x66,0x91,0x12,0x54,0x48,0xbb,0xb2,0x5b,0x4f,
    0xf4,0x12,0xa4,0x9c,0x73,0x2d,0xb2,0xc8,0xab,0xc1,0xb8,0x58,0x1b,0xd7,0x10,0xdd
};
static const kdrv_manifest_key_t key = {
    { 0x21,0xfe,0x31,0xdf,0xa1,0x54,0xa2,0x61,0x62,0x6b,0xf8,0x54,0x04,0x6f,0xd2,0x27 },
    { 0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
      0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a },
    0
};

int main(int argc, char** argv) {
    kdrv_manifest_t m;
    if (argc != 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    int read_ok = fread(&m, 1, sizeof m, f) == sizeof m && fgetc(f) == EOF;
    fclose(f);
    if (!read_ok) return 2;

    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_OK);
    CHECK(kdrv_manifest_verify_owner(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI) == KDRV_MANIFEST_E_KEY);
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
    bad = m; bad.key_id[0] ^= 1;
    CHECK(kdrv_manifest_verify(&bad, sizeof bad, package_sha, 7, KDRV_MANIFEST_ABI, &key, 1) == KDRV_MANIFEST_E_KEY);

    kdrv_manifest_key_t revoked = key; revoked.revoked = 1;
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &revoked, 1) == KDRV_MANIFEST_E_REVOKED);
    kdrv_manifest_key_t unknown = key; unknown.key_id[0] ^= 1;
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &unknown, 1) == KDRV_MANIFEST_E_KEY);
    kdrv_manifest_key_t wrong_pub = key; wrong_pub.public_key[0] ^= 1;
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, &wrong_pub, 1) == KDRV_MANIFEST_E_KEY);
    kdrv_manifest_key_t duplicate[2] = { key, key };
    CHECK(kdrv_manifest_verify(&m, sizeof m, package_sha, 7, KDRV_MANIFEST_ABI, duplicate, 2) == KDRV_MANIFEST_E_KEY);

    if (!failures) puts("kdrv_manifest: signer/verifier interoperability, Ed25519, fingerprint, package/version/ABI binding, revoked/unknown/ambiguous keys and hostile inputs passed");
    return failures != 0;
}
