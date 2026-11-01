#ifndef KIRA_TLS_H
#define KIRA_TLS_H
/* TLS 1.3 (RFC 8446) - клиент для программ: X25519, TLS_CHACHA20_POLY1305_SHA256. Работает поверх соединения TCP
   ядра (k_net_*), вся криптография - в программе (кольцо 3 / EL0), ядро видит только шифротекст.
   Сертификат сервера: цепочка принимается и хранится, но ПОДПИСИ X.509 и CertificateVerify пока не проверяются
   (нужны ECDSA P-256/RSA и корневые сертификаты - следующий шаг). tls_t.verified = 0 - так и сообщается. */
#ifdef TLS_HOST
#include "tls_host_api.h"      /* хост-тест: те же функции поверх сокетов Linux */
#else
#include "kira.h"
#endif

typedef struct {
    i64 sock;
    int state;                       /* 0 - нет, 1 - рукопожатие, 2 - данные, 3 - закрыто собеседником, -1 - ошибка */
    const char* err;
    int verified;                    /* 1 - сертификат проверен (пока всегда 0) */
    int weak_random;                 /* ключ сделан без аппаратного источника случайности */
    u16 cipher;
    u32 cert_count, leaf_len;
    u8 rkey[32], riv[12], wkey[32], wiv[12], rsec[32];
    u64 rseq, wseq;
    u8 in[16384 + 512]; u32 in_len;  /* сырой входной поток (записи) */
    u8 pt[16384 + 256]; u32 pt_off, pt_len;   /* расшифрованные данные приложения */
} tls_t;

int tls_connect(tls_t* t, i64 sock, const char* host, u32 timeout_ms);   /* 0 - рукопожатие завершено */
i64 tls_send(tls_t* t, const void* p, u32 n);                             /* отправлено байт или <0 */
i64 tls_recv(tls_t* t, void* p, u32 n, u32 timeout_ms);                   /* байт; 0 - сервер закрыл; <0 - ошибка */
void tls_close(tls_t* t);                                                 /* close_notify (сокет не закрывает) */
#endif
