// Хост-тест криптографии TLS 1.3 по векторам стандартов: RFC 8439 (ChaCha20, Poly1305, AEAD), RFC 7748 (X25519),
// RFC 4231 (HMAC-SHA256), RFC 5869 (HKDF), RFC 8448 (HKDF-Expand-Label TLS 1.3).
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void hex(const char* s, uint8_t* o) { while (*s) { unsigned v; sscanf(s, "%2x", &v); *o++ = (uint8_t)v; s += 2; } }
static int eqhex(const uint8_t* b, const char* s) { uint8_t t[256]; size_t n = strlen(s) / 2; hex(s, t); return memcmp(b, t, n) == 0; }

int main(void) {
    uint8_t key[32], nonce[12], out[256], tag[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    hex("000000090000004a00000000", nonce);
    chacha20_block(key, 1, nonce, out);
    CHECK(eqhex(out, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"));

    uint8_t pk[32]; hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", pk);
    const char* msg = "Cryptographic Forum Research Group";
    poly1305_ctx_t p; poly1305_init(&p, pk); poly1305_update(&p, (const uint8_t*)msg, 5); poly1305_update(&p, (const uint8_t*)msg + 5, strlen(msg) - 5);
    poly1305_final(&p, tag);
    CHECK(eqhex(tag, "a8061dc1305136c6c22b8baf0c0127a9"));

    const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t n = strlen(pt);
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    hex("070000004041424344454647", nonce);
    uint8_t aad[12]; hex("50515253c0c1c2c3c4c5c6c7", aad);
    aead_chacha20_poly1305_seal(key, nonce, aad, 12, (const uint8_t*)pt, n, out, tag);
    CHECK(eqhex(out, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"));
    CHECK(eqhex(tag, "1ae10b594f09e26a7e902ecbd0600691"));
    uint8_t back[256];
    CHECK(aead_chacha20_poly1305_open(key, nonce, aad, 12, out, n, tag, back) == 0 && memcmp(back, pt, n) == 0);
    out[3] ^= 1; CHECK(aead_chacha20_poly1305_open(key, nonce, aad, 12, out, n, tag, back) == -1);   /* подделка */

    uint8_t a[32], b[32], A[32], B[32], s1[32], s2[32];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    x25519_base(A, a); x25519_base(B, b);
    CHECK(eqhex(A, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(eqhex(B, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    CHECK(x25519(s1, a, B) == 0 && x25519(s2, b, A) == 0);
    CHECK(eqhex(s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742") && !memcmp(s1, s2, 32));
    uint8_t zero[32] = { 0 }; CHECK(x25519(s1, a, zero) == -1);                     /* точка малого порядка */

    hmac_sha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, out);
    CHECK(eqhex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    uint8_t ikm[22], salt[13], info[10], prk[32];
    memset(ikm, 0x0b, 22); for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i; for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hkdf_extract(salt, 13, ikm, 22, prk);
    CHECK(eqhex(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
    hkdf_expand(prk, info, 10, out, 42);
    CHECK(eqhex(out, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));

    uint8_t early[32], empty[32], derived[32];
    hkdf_extract(0, 0, zero, 32, early);
    CHECK(eqhex(early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a"));
    sha256("", 0, empty);
    hkdf_expand_label(early, "derived", empty, 32, derived, 32);
    CHECK(eqhex(derived, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba"));

    printf(fails ? "CRYPTO TESTS FAILED\n" : "crypto: ChaCha20, Poly1305, AEAD (+forgery), X25519 (+low order), HMAC-SHA256, HKDF, TLS 1.3 labels - RFC vectors passed\n");
    return fails != 0;
}
