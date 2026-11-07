// Хост-тест проверки подписей: RSA 2048/3072/4096 (PKCS#1 v1.5 и PSS, SHA-256/384/512), ECDSA P-256 и P-384
// (в т.ч. P-256 с SHA-384 - усечение хэша). Подписи сделаны OpenSSL (tests/pk_vectors.h); испорченные - отвергаются.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pk.h"
#include "pk_vectors.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static const uint8_t e65537[] = { 1, 0, 1 };

static size_t unhex(const char* s, uint8_t* o) { size_t n = 0; while (s[0] && s[1] && s[0] != '\n' && s[0] != ' ') { unsigned v; sscanf(s, "%2x", &v); o[n++] = (uint8_t)v; s += 2; } return n; }
static const struct { const char *pk, *msg, *sig; } ED[] = {            /* RFC 8032 §7.1, TEST 1, 2, 3 */
    { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" } };

/* Сверка с python cryptography (OpenSSL): файл строк «ed pub msg sig ожидание» и «pss n e хэш mgf соль digest sig ожидание». */
static int cross(const char* path, int* ned, int* npss) {
    FILE* f = fopen(path, "r"); static char line[16384]; int bad = 0;
    if (!f) { printf("FAIL: no cross-check file %s\n", path); return 1; }
    while (fgets(line, sizeof line, f)) {
        static uint8_t a[4096], b[4096], c[4096], d[4096]; int alg, mgf, salt, want;
        if (!strncmp(line, "ed ", 3)) {
            char *p = line + 3; size_t pl = unhex(p, a); p += 2 * pl + 1;
            size_t ml = *p == '-' ? 0 : unhex(p, b); p += (*p == '-' ? 1 : 2 * ml) + 1;
            size_t sl = unhex(p, c); p += 2 * sl + 1; want = atoi(p);
            if (pl != 32 || sl != 64) { bad++; continue; }
            if ((ed25519_verify(a, b, ml, c) == 0) != want) { printf("FAIL ed25519 cross-check: %.20s...\n", line + 3); bad++; }
            (*ned)++;
        } else if (!strncmp(line, "pss ", 4)) {
            char *p = line + 4; size_t nl = unhex(p, a); p += 2 * nl + 1;
            size_t el = unhex(p, b); p += 2 * el + 1;
            if (sscanf(p, "%d %d %d", &alg, &mgf, &salt) != 3) { bad++; continue; }
            for (int i = 0; i < 3; i++) { while (*p && *p != ' ') p++; p++; }
            size_t dl = unhex(p, c); p += 2 * dl + 1; size_t sl = unhex(p, d); p += 2 * sl + 1; want = atoi(p);
            if ((rsa_verify_pss_ex(a, nl, b, el, alg, mgf, (size_t)salt, c, d, sl) == 0) != want) { printf("FAIL PSS cross-check: hash %d mgf %d salt %d\n", alg, mgf, salt); bad++; }
            (*npss)++;
        }
    }
    fclose(f); return bad;
}

int main(int argc, char** argv) {
    uint8_t bad[512], h[64];
    for (unsigned i = 0; i < sizeof ED / sizeof ED[0]; i++) {
        uint8_t pk[32], m[8], sg[64]; size_t ml = unhex(ED[i].msg, m);
        unhex(ED[i].pk, pk); unhex(ED[i].sig, sg);
        CHECK(ed25519_verify(pk, m, ml, sg) == 0);
        sg[10] ^= 4; CHECK(ed25519_verify(pk, m, ml, sg) == -1);                         /* R испорчено */
        sg[10] ^= 4; sg[40] ^= 1; CHECK(ed25519_verify(pk, m, ml, sg) == -1);            /* S испорчено */
        sg[40] ^= 1; sg[63] |= 0x10; CHECK(ed25519_verify(pk, m, ml, sg) == -1);         /* S >= L */
        sg[63] &= 0xef; m[0] ^= 1; CHECK(ed25519_verify(pk, m, ml ? ml : 1, sg) == -1);  /* сообщение другое */
        pk[5] ^= 1; unhex(ED[i].msg, m); CHECK(ed25519_verify(pk, m, ml, sg) == -1);      /* ключ другой */
    }
    { uint8_t pk[32], m[1], sg[64]; unhex(ED[1].pk, pk); unhex(ED[1].msg, m); unhex(ED[1].sig, sg);
      pk[31] = 0xff; memset(pk, 0xff, 31); CHECK(ed25519_verify(pk, m, 1, sg) == -1); }                        /* y >= p */
    int ned = 0, npss = 0;
    if (argc > 1) fails += cross(argv[1], &ned, &npss);
    CHECK(rsa_verify_pkcs1(r1_n, sizeof r1_n, e65537, 3, PK_SHA256, r1_hash, r1_sig, sizeof r1_sig) == 0);
    CHECK(rsa_verify_pss(r2_n, sizeof r2_n, e65537, 3, PK_SHA384, r2_hash, r2_sig, sizeof r2_sig) == 0);
    CHECK(rsa_verify_pkcs1(r3_n, sizeof r3_n, e65537, 3, PK_SHA512, r3_hash, r3_sig, sizeof r3_sig) == 0);
    CHECK(rsa_verify_pss(r4_n, sizeof r4_n, e65537, 3, PK_SHA256, r4_hash, r4_sig, sizeof r4_sig) == 0);
    memcpy(bad, r1_sig, sizeof r1_sig); bad[100] ^= 1;
    CHECK(rsa_verify_pkcs1(r1_n, sizeof r1_n, e65537, 3, PK_SHA256, r1_hash, bad, sizeof r1_sig) == -1);
    memcpy(h, r2_hash, 48); h[0] ^= 1;
    CHECK(rsa_verify_pss(r2_n, sizeof r2_n, e65537, 3, PK_SHA384, h, r2_sig, sizeof r2_sig) == -1);
    CHECK(rsa_verify_pss(r1_n, sizeof r1_n, e65537, 3, PK_SHA256, r1_hash, r1_sig, sizeof r1_sig) == -1);   /* не та схема */

    CHECK(ecdsa_verify(256, e1_pub, sizeof e1_pub, e1_hash, sizeof e1_hash, e1_r, sizeof e1_r, e1_s, sizeof e1_s) == 0);
    CHECK(ecdsa_verify(384, e2_pub, sizeof e2_pub, e2_hash, sizeof e2_hash, e2_r, sizeof e2_r, e2_s, sizeof e2_s) == 0);
    CHECK(ecdsa_verify(256, e3_pub, sizeof e3_pub, e3_hash, sizeof e3_hash, e3_r, sizeof e3_r, e3_s, sizeof e3_s) == 0);
    memcpy(h, e1_hash, 32); h[31] ^= 1;
    CHECK(ecdsa_verify(256, e1_pub, sizeof e1_pub, h, 32, e1_r, sizeof e1_r, e1_s, sizeof e1_s) == -1);
    memcpy(bad, e2_s, sizeof e2_s); bad[5] ^= 0x40;
    CHECK(ecdsa_verify(384, e2_pub, sizeof e2_pub, e2_hash, sizeof e2_hash, e2_r, sizeof e2_r, bad, sizeof e2_s) == -1);
    memcpy(bad, e1_pub, sizeof e1_pub); bad[40] ^= 1;                                   /* точка не на кривой */
    CHECK(ecdsa_verify(256, bad, sizeof e1_pub, e1_hash, 32, e1_r, sizeof e1_r, e1_s, sizeof e1_s) == -1);
    CHECK(ecdsa_verify(256, e1_pub, sizeof e1_pub, e1_hash, 32, e1_s, sizeof e1_s, e1_r, sizeof e1_r) == -1);   /* r и s местами */

    {
        uint8_t valid_pk[32] = {0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
            0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,0x0e,0xe1,0x72,0xf3,
            0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a};
        uint8_t valid_sig[64] = {0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,
            0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,0x84,0x87,0x7f,0x1e,
            0xb8,0xe5,0xd9,0x74,0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
            0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,0xc6,0x1e,0x39,0x70,
            0x1c,0xf9,0xb4,0x6b,0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,
            0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b};
        uint8_t message = 0;
        CHECK(ed25519_verify(0, &message, 1, valid_sig) == -1);
        CHECK(ed25519_verify(valid_pk, 0, 1, valid_sig) == -1);
        CHECK(ed25519_verify(valid_pk, &message, 1, 0) == -1);
    }
    {
        uint8_t identity_pk[32] = {1};
        uint8_t identity_sig[64] = {1}; /* R=identity, S=0 */
        uint8_t message = 0x42;
        CHECK(ed25519_verify(identity_pk, &message, 1, identity_sig) == -1);
    }
    {
        uint8_t torsion_pk[32], noncanonical_pk[32], sig[64] = {1}, message = 0x42;
        memset(torsion_pk, 0xff, sizeof torsion_pk);
        torsion_pk[0] = 0xec; torsion_pk[31] = 0x7f; /* order-2 point (0,-1) */
        memset(noncanonical_pk, 0xff, sizeof noncanonical_pk);
        noncanonical_pk[0] = 0xed; noncanonical_pk[31] = 0x7f; /* y = p, not canonical */
        CHECK(ed25519_verify(torsion_pk, &message, 1, sig) == -1);
        CHECK(ed25519_verify(noncanonical_pk, &message, 1, sig) == -1);
        /* S must be strictly smaller than the Ed25519 subgroup order L. */
        static const uint8_t order_l[32] = {
            0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
            0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10
        };
        memcpy(sig + 32, order_l, sizeof order_l);
        CHECK(ed25519_verify(valid_pk, &message, 1, sig) == -1);
    }
    if (!fails) printf("pk: RSA 2048/3072/4096 PKCS#1 v1.5 + PSS (SHA-256/384/512), ECDSA P-256/P-384 (+truncated hash), Ed25519 (RFC 8032 TEST 1-3), forgeries rejected - passed\n");
    else printf("PK TESTS FAILED\n");
    if (!fails && argc > 1) printf("pk: cross-check with python cryptography/OpenSSL: %d Ed25519 and %d RSA-PSS signatures (valid and corrupted; hash, MGF1 hash, salt length vary) - passed\n", ned, npss);
    return fails != 0;
}
