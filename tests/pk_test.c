// Хост-тест проверки подписей: RSA 2048/3072/4096 (PKCS#1 v1.5 и PSS, SHA-256/384/512), ECDSA P-256 и P-384
// (в т.ч. P-256 с SHA-384 - усечение хэша). Подписи сделаны OpenSSL (tests/pk_vectors.h); испорченные - отвергаются.
#include <stdio.h>
#include <string.h>
#include "pk.h"
#include "pk_vectors.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static const uint8_t e65537[] = { 1, 0, 1 };

int main(void) {
    uint8_t bad[512], h[64];
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

    printf(fails ? "PK TESTS FAILED\n" : "pk: RSA 2048/3072/4096 PKCS#1 v1.5 + PSS (SHA-256/384/512), ECDSA P-256/P-384 (+truncated hash), forgeries rejected - passed\n");
    return fails != 0;
}
