#ifndef KIRA_PK_H
#define KIRA_PK_H
/* Проверка подписей с открытым ключом (сертификаты и CertificateVerify TLS): RSA (PKCS#1 v1.5 и PSS, модуль до
   4096 бит) и ECDSA (P-256, P-384). Только проверка: ключи и подписи открыты, секретов нет - постоянное время не нужно.
   Арифметика Монтгомери на 32-битных частях, без кучи. 0 - подпись верна, -1 - нет (или неподдерживаемые размеры). */
#include <stdint.h>
#include <stddef.h>

enum { PK_SHA256 = 32, PK_SHA384 = 48, PK_SHA512 = 64 };     /* алгоритм хэша = длина его значения */
void pk_hash(int alg, const void* data, size_t n, uint8_t* out);

int rsa_verify_pkcs1(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                     const uint8_t* sig, size_t slen);
int rsa_verify_pss(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                   const uint8_t* sig, size_t slen);
/* pub - точка без сжатия (04 || X || Y); curve - 256 или 384; digest любой длины (усекается до размера порядка). */
int ecdsa_verify(int curve, const uint8_t* pub, size_t publen, const uint8_t* digest, size_t dlen,
                 const uint8_t* r, size_t rlen, const uint8_t* s, size_t slen);
#endif
