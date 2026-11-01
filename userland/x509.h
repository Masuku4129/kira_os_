#ifndef KIRA_X509_H
#define KIRA_X509_H
/* Сертификаты X.509 (DER, RFC 5280) для TLS: разбор, подписи (RSA PKCS#1 v1.5, ECDSA P-256/P-384 с SHA-256/384/512),
   цепочка до корневого сертификата из хранилища, имя хоста (subjectAltName: DNS с * в левой метке, IPv4), срок.
   Без памяти из кучи: поля - указатели внутрь переданных байтов. */
#include <stdint.h>
#include <stddef.h>

enum { X509_KEY_NONE = 0, X509_KEY_RSA = 1, X509_KEY_EC256 = 2, X509_KEY_EC384 = 3 };
enum { X509_SIG_NONE = 0, X509_SIG_RSA_SHA256, X509_SIG_RSA_SHA384, X509_SIG_RSA_SHA512,
       X509_SIG_EC_SHA256, X509_SIG_EC_SHA384, X509_SIG_EC_SHA512 };

typedef struct {
    const uint8_t *tbs, *sig, *issuer, *subject, *rsa_n, *rsa_e, *ec_pub, *san;
    size_t tbs_len, sig_len, issuer_len, subject_len, rsa_n_len, rsa_e_len, ec_pub_len, san_len;
    int sig_alg, key_type, is_ca, has_bc;
    int64_t not_before, not_after;          /* секунды UTC от 1970 */
} x509_t;

int x509_parse(const uint8_t* der, size_t len, x509_t* c);           /* 0 - разобран; <0 - не DER/не X.509 v3 */
/* Подпись child ключом parent: 0 - верна; -1 - нет; -2 - алгоритм не поддерживается. */
int x509_check_sig(const x509_t* child, const x509_t* parent);
int x509_match_host(const x509_t* c, const char* host);               /* 1 - имя (или IPv4) есть в subjectAltName */

/* Цепочка: certs[0] - сертификат сервера, дальше - промежуточные (в любом порядке). roots - подряд идущие DER
   корневых сертификатов. now - секунды UTC (0 - время неизвестно: срок не проверяется, *why говорит об этом).
   0 - доверено; <0 - отказ, *why - причина по-английски. */
int x509_verify_chain(const uint8_t* const* certs, const size_t* lens, int n, const uint8_t* roots, size_t roots_len,
                      const char* host, int64_t now, const char** why);
int x509_count(const uint8_t* roots, size_t len);                     /* сертификатов в хранилище (разобранных) */

/* Подпись данных ключом сертификата (CertificateVerify TLS 1.3): scheme - код TLS (0x0403, 0x0503, 0x0804-0x0806). */
int x509_verify_scheme(const x509_t* c, unsigned scheme, const uint8_t* data, size_t n, const uint8_t* sig, size_t slen);
#endif
