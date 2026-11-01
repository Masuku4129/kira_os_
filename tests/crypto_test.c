// Хост-тест криптографии TLS 1.3 по векторам стандартов: RFC 8439 (ChaCha20, Poly1305, AEAD), RFC 7748 (X25519),
// RFC 4231 (HMAC-SHA256), RFC 5869 (HKDF), RFC 8448 (HKDF-Expand-Label TLS 1.3).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"

/* Эталон: прежняя реализация AES-128 с таблицей S-блока (до перехода на вычисление без таблиц), только для сверки. */
static const uint8_t RSBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint32_t rsubw(uint32_t w) { return (uint32_t)RSBOX[w >> 24] << 24 | (uint32_t)RSBOX[(w >> 16) & 255] << 16 | (uint32_t)RSBOX[(w >> 8) & 255] << 8 | RSBOX[w & 255]; }
static uint8_t rxt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void raes_expand(uint32_t rk[44], const uint8_t key[16]) {
    static const uint8_t rcon[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36 };
    for (int i = 0; i < 4; i++) rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) t = rsubw(t << 8 | t >> 24) ^ ((uint32_t)rcon[i / 4 - 1] << 24);
        rk[i] = rk[i - 4] ^ t;
    }
}

void raes_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (uint8_t)(rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = RSBOX[s[(i + 4 * (i % 4)) % 16]];          /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                              /* MixColumns */
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[4 * c] ^= x ^ rxt(a0 ^ a1); t[4 * c + 1] ^= x ^ rxt(a1 ^ a2); t[4 * c + 2] ^= x ^ rxt(a2 ^ a3); t[4 * c + 3] ^= x ^ rxt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ (uint8_t)(rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
    
}


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

    sha512_ctx_t q5; sha512_init(&q5); sha512_update(&q5, "abc", 3); sha512_final(&q5, out);
    CHECK(eqhex(out, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    sha384_init(&q5);
    for (int i = 0; i < 1000; i++) sha512_update(&q5, "a", 1);           /* через границу блока */
    sha512_final(&q5, out);
    CHECK(eqhex(out, "f54480689c6b0b11d0303285d9a81b21a93bca6ba5a1b4472765dca4da45ee328082d469c650cd3b61b16d3266ab8ced"));

    { uint8_t k[16], blk[16]; aes_gcm_t g;                                   /* FIPS-197 C.1; GCM: McGrew-Viega 3 и 4 */
      for (int i = 0; i < 16; i++) k[i] = (uint8_t)i;
      hex("00112233445566778899aabbccddeeff", blk); aes128_gcm_init(&g, k); aes128_encrypt_block(g.rk, blk, out);
      CHECK(eqhex(out, "69c4e0d86a7b0430d8cdb78070b4c55a"));
      uint8_t P[64], A[20], N[12], C[64], back2[64];
      hex("feffe9928665731c6d6a8f9467308308", k); hex("cafebabefacedbaddecaf888", N);
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", P);
      hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", A);
      aes128_gcm_init(&g, k);
      aes128_gcm_seal(&g, N, 0, 0, P, 64, C, tag);
      CHECK(eqhex(C, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985"));
      CHECK(eqhex(tag, "4d5c2af327cd64a62cf35abd2ba6fab4"));
      aes128_gcm_seal(&g, N, A, 20, P, 60, C, tag);
      CHECK(eqhex(tag, "5bc94fbc3221a5db94fae95ae7121a47"));
      CHECK(aes128_gcm_open(&g, N, A, 20, C, 60, tag, back2) == 0 && !memcmp(back2, P, 60));
      C[7] ^= 1; CHECK(aes128_gcm_open(&g, N, A, 20, C, 60, tag, back2) == -1); }

    { uint8_t d1[20]; char* big = malloc(1000000);                           /* FIPS 180-4: SHA-1 */
      sha1("abc", 3, d1); CHECK(eqhex(d1, "a9993e364706816aba3e25717850c26c9cd0d89d"));
      sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d1); CHECK(eqhex(d1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
      sha1("", 0, d1); CHECK(eqhex(d1, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
      memset(big, 'a', 1000000); sha1(big, 1000000, d1); CHECK(eqhex(d1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f")); free(big); }

    { /* сверка AES без таблиц с прежней табличной реализацией на случайных ключах и блоках */
      FILE* ur = fopen("/dev/urandom", "rb"); int bad = 0;
      for (int n = 0; n < 2000 && ur; n++) {
          uint8_t k[16], blk[16], o1[16], o2[16]; uint32_t rrk[44]; aes_gcm_t gg;
          if (fread(k, 1, 16, ur) != 16 || fread(blk, 1, 16, ur) != 16) { bad = 1; break; }
          raes_expand(rrk, k); aes128_gcm_init(&gg, k);
          for (int i = 0; i < 44; i++) if (rrk[i] != gg.rk[i]) bad = 1;
          raes_encrypt_block(rrk, blk, o1); aes128_encrypt_block(gg.rk, blk, o2);
          if (memcmp(o1, o2, 16)) bad = 1;
      }
      if (ur) fclose(ur);
      CHECK(ur && !bad);
      uint8_t k0[16] = { 0 }, z[16] = { 0 }, o1[16], o2[16]; uint32_t rrk[44]; aes_gcm_t gg;      /* все 256 значений S-блока */
      raes_expand(rrk, k0); aes128_gcm_init(&gg, k0);
      for (int v = 0; v < 256; v++) { z[0] = (uint8_t)v; z[5] = (uint8_t)(255 - v); raes_encrypt_block(rrk, z, o1); aes128_encrypt_block(gg.rk, z, o2); CHECK(!memcmp(o1, o2, 16)); }
    }

    printf(fails ? "CRYPTO TESTS FAILED\n" : "crypto: ChaCha20, Poly1305, AEAD (+forgery), AES-128 without tables (FIPS 197, + 2000 random keys/blocks equal to the old table version), AES-128-GCM (+forgery), SHA-1, SHA-384/512, X25519 (+low order), HMAC-SHA256, HKDF, TLS 1.3 labels - RFC vectors passed\n");
    return fails != 0;
}
