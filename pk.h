#ifndef KIRA_PK_H
#define KIRA_PK_H
/* Проверка подписей с открытым ключом (сертификаты и CertificateVerify TLS): RSA (PKCS#1 v1.5 и PSS, модуль до
   4096 бит), ECDSA (P-256, P-384) и Ed25519. Проверка: ключи и подписи открыты, секретов нет - постоянное время не нужно. Подпись Ed25519 (секретное семя) - в конце файла.
   Арифметика Монтгомери на 32-битных частях, без кучи. 0 - подпись верна, -1 - нет (или неподдерживаемые размеры). */
#include <stdint.h>
#include <stddef.h>

enum { PK_SHA256 = 32, PK_SHA384 = 48, PK_SHA512 = 64 };     /* алгоритм хэша = длина его значения */
void pk_hash(int alg, const void* data, size_t n, uint8_t* out);

int rsa_verify_pkcs1(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                     const uint8_t* sig, size_t slen);
int rsa_verify_pss(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                   const uint8_t* sig, size_t slen);
/* PSS с отдельными параметрами (сертификаты id-RSASSA-PSS): хэш сообщения, хэш MGF1, длина соли. */
int rsa_verify_pss_ex(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, int mgf_alg, size_t salt,
                      const uint8_t* digest, const uint8_t* sig, size_t slen);
/* pub - точка без сжатия (04 || X || Y); curve - 256 или 384; digest любой длины (усекается до размера порядка). */
int ecdsa_verify(int curve, const uint8_t* pub, size_t publen, const uint8_t* digest, size_t dlen,
                 const uint8_t* r, size_t rlen, const uint8_t* s, size_t slen);
/* Ed25519 (RFC 8032): pub - 32 байта открытого ключа, sig - 64 байта (R || S), сообщение целиком (PureEdDSA). */
int ed25519_verify(const uint8_t pub[32], const uint8_t* msg, size_t mlen, const uint8_t sig[64]);
/* Подпись Ed25519 (RFC 8032): seed - 32 секретных байта; pub получается из семени; sig - 64 байта. 0 - готово. Не для ключей, которые нужно беречь от
   наблюдателя по времени (см. pk.c). Нужна журналу аудита: ключ устройства создаётся и остаётся на самом устройстве. */
/* Считает константы Ed25519 заранее (один раз): ядро зовёт это при запуске, пока работает один поток, и дальше проверка и подпись из разных потоков безопасны. */
int ed25519_prepare(void);
int ed25519_pubkey(const uint8_t seed[32], uint8_t pub[32]);
int ed25519_sign(const uint8_t seed[32], const uint8_t* msg, size_t mlen, uint8_t sig[64]);
#endif
