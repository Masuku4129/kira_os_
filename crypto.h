#ifndef KIRA_CRYPTO_H
#define KIRA_CRYPTO_H
/* Криптография для TLS 1.3 и генератора случайных чисел ядра. Чистый C без библиотек, без памяти из кучи;
   одинаково собирается в ядро, в программы (кольцо 3 / EL0) и в хост-тесты (векторы RFC 8439, 7748, 4231, 5869).
   SHA-256 - из sha256.c (общий с иммунной системой). */
#include <stdint.h>
#include <stddef.h>
#include "immune.h"          /* sha256_ctx_t, sha256_* */

/* ChaCha20 (RFC 8439): ключ 32 байта, nonce 12 байт, счётчик блоков. out = in XOR поток; out может совпадать с in. */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, uint8_t* out, size_t len);
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);

/* Poly1305 (RFC 8439): ключ 32 байта (одноразовый), метка 16 байт. */
typedef struct { uint32_t r[5], h[5], pad[4]; uint8_t buf[16]; size_t left; } poly1305_ctx_t;
void poly1305_init(poly1305_ctx_t* c, const uint8_t key[32]);
void poly1305_update(poly1305_ctx_t* c, const uint8_t* m, size_t n);
void poly1305_final(poly1305_ctx_t* c, uint8_t tag[16]);

/* AEAD ChaCha20-Poly1305 (RFC 8439 §2.8). Шифрование: out = шифротекст, tag - метка.
   Расшифровка: 0 - метка верна (out заполнен), -1 - подделка (out не заполняется). */
void aead_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                                 const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]);
int  aead_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                                 const uint8_t* in, size_t len, const uint8_t tag[16], uint8_t* out);

/* X25519 (RFC 7748): out = scalar * point; базовая точка 9 - x25519_base. 0 - успех, -1 - общий секрет нулевой. */
int  x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* HMAC-SHA256 (RFC 2104/4231), HKDF (RFC 5869), HKDF-Expand-Label TLS 1.3 (RFC 8446 §7.1). */
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]);
void hkdf_extract(const uint8_t* salt, size_t slen, const uint8_t* ikm, size_t ilen, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t ilen, uint8_t* out, size_t olen);
void hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, size_t clen, uint8_t* out, size_t olen);

/* AES-128-GCM (FIPS 197, SP 800-38D): обязательный набор TLS 1.3 (TLS_AES_128_GCM_SHA256), nonce 12 байт.
   AES - без таблиц: S-блок вычисляется в GF(2^8) в битовой нарезке (без индексирования по секрету и ветвей),
   GHASH - побитовое умножение без таблиц. */
typedef struct { uint32_t rk[44]; uint8_t h[16]; } aes_gcm_t;
void aes128_gcm_init(aes_gcm_t* g, const uint8_t key[16]);
void aes128_gcm_seal(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                     const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]);
int  aes128_gcm_open(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                     const uint8_t* in, size_t len, const uint8_t tag[16], uint8_t* out);   /* 0 - метка верна */
void aes128_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);

/* SHA-512 и SHA-384 (FIPS 180-4): подписи сертификатов (ECDSA P-384, RSA с SHA-384/512). */
typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; uint32_t n; int out; } sha512_ctx_t;
void sha512_init(sha512_ctx_t* c);
void sha384_init(sha512_ctx_t* c);
void sha512_update(sha512_ctx_t* c, const void* d, size_t n);
void sha512_final(sha512_ctx_t* c, uint8_t* out);          /* 64 байта (SHA-512) или 48 (SHA-384) */

/* SHA-1 (FIPS 180-4): только для идентификаторов CertID в OCSP (RFC 6960), не для защиты. */
void sha1(const void* data, size_t len, uint8_t out[20]);

int  crypto_eq(const void* a, const void* b, size_t n);   /* сравнение за постоянное время: 1 - равны */
void crypto_wipe(void* p, size_t n);                      /* стереть секрет (компилятор не выбросит) */
#endif
