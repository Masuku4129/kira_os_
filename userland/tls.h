#ifndef KIRA_TLS_H
#define KIRA_TLS_H
/* TLS 1.3 (RFC 8446) - клиент для программ: X25519, TLS_CHACHA20_POLY1305_SHA256. Работает поверх соединения TCP
   ядра (k_net_*), вся криптография - в программе (кольцо 3 / EL0), ядро видит только шифротекст.
   Сервер доказывает владение ключом сертификата (CertificateVerify проверяется всегда). С хранилищем корневых
   (roots) - ещё цепочка X.509 до корневого, имя хоста и срок (now - секунды UTC, 0 - часы неизвестны): иначе отказ.
   Без хранилища (roots = 0) личность сервера не проверена: verified = 0, и программа обязана так и сказать. */
#ifdef TLS_HOST
#include "tls_host_api.h"      /* хост-тест: те же функции поверх сокетов Linux */
#else
#include "kira.h"
#endif

typedef struct {
    i64 sock;
    int state;                       /* 0 - нет, 1 - рукопожатие, 2 - данные, 3 - закрыто собеседником, -1 - ошибка */
    const char* err;
    int verified;                    /* 1 - цепочка до корневого, имя и срок проверены */
    int cv_ok;                       /* CertificateVerify верна */
    const char* why;                 /* оговорка проверки (например, часы неизвестны) или 0 */
    int weak_random;                 /* ключ сделан без аппаратного источника случайности */
    u16 cipher;
    u32 cert_count, leaf_len;
    u8 rkey[32], riv[12], wkey[32], wiv[12], rsec[32];
    u64 rseq, wseq;
    u8 in[16384 + 512]; u32 in_len;  /* сырой входной поток (записи) */
    u8 pt[16384 + 256]; u32 pt_off, pt_len;   /* расшифрованные данные приложения */
} tls_t;

int tls_connect(tls_t* t, i64 sock, const char* host, u32 timeout_ms, const u8* roots, u32 roots_len, i64 now);   /* 0 - готово */
i64 tls_send(tls_t* t, const void* p, u32 n);                             /* отправлено байт или <0 */
i64 tls_recv(tls_t* t, void* p, u32 n, u32 timeout_ms);                   /* байт; 0 - сервер закрыл; <0 - ошибка */
void tls_close(tls_t* t);                                                 /* close_notify (сокет не закрывает) */
#endif
