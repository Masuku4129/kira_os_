#ifndef KIRA_TLS_H
#define KIRA_TLS_H
/* TLS 1.3 (RFC 8446) - клиент для программ: X25519, TLS_CHACHA20_POLY1305_SHA256 и TLS_AES_128_GCM_SHA256. Работает поверх соединения TCP
   ядра (k_net_*), вся криптография - в программе (кольцо 3 / EL0), ядро видит только шифротекст.
   Сервер доказывает владение ключом сертификата (CertificateVerify проверяется всегда). С хранилищем корневых
   (roots) - ещё цепочка X.509 до корневого, имя хоста и срок (now - секунды UTC, 0 - часы неизвестны): иначе отказ.
   Без хранилища (roots = 0) личность сервера не проверена: verified = 0, и программа обязана так и сказать.
   OCSP stapling: клиент просит status_request; прикреплённый ответ для листа проверяется (x509_ocsp): ocsp = 1 - ответ
   подписан, про этот сертификат, свежий, статус good; ответа нет (ocsp_stapled = 0) - не ошибка, ocsp = 0; отозван
   (revoked), ответ подделан или про другой сертификат - отказ.
   Повторное использование сессии: после рукопожатия сервер присылает NewSessionTicket (приходит с первыми данными
   tls_recv) - билет с PSK лежит в t->ticket (t->ticket.valid) в памяти программы; передав его в tls_connect, следующее
   соединение с тем же именем идёт по psk_dhe_ke (резюмирование: без сертификатов, resumed = 1). Отказ сервера от билета -
   обычное полное рукопожатие. */
#ifdef TLS_HOST
#include "tls_host_api.h"      /* хост-тест: те же функции поверх сокетов Linux */
#else
#include "kira.h"
#endif
#include "../crypto.h"

#define TLS_TICKET_MAX 1024
typedef struct {
    int valid;                       /* 1 - билет и PSK получены */
    u8 ticket[TLS_TICKET_MAX]; u32 ticket_len;
    u8 psk[32];                      /* resumption PSK (секрет: хранить как ключ) */
    u32 lifetime_s, age_add;
    i64 issued_s;                    /* UTC секунды получения (0 - часы неизвестны) */
    u16 cipher;
    char host[64];                   /* билет годен только для этого имени */
    int verified;                    /* исходное соединение прошло проверку цепочки, имени и срока */
} tls_ticket_t;

typedef struct {
    i64 sock;
    int state;                       /* 0 - нет, 1 - рукопожатие, 2 - данные, 3 - закрыто собеседником, -1 - ошибка */
    const char* err;
    int verified;                    /* 1 - цепочка до корневого, имя и срок проверены */
    int cv_ok;                       /* CertificateVerify верна */
    u16 cv_scheme;                   /* схема подписи сервера (0x0403 ECDSA P-256, 0x0807 Ed25519, 0x0809 RSA-PSS ...) */
    const char* why;                 /* оговорка проверки (например, часы неизвестны) или 0 */
    int weak_random;                 /* ключ сделан без аппаратного источника случайности */
    int ocsp_stapled;                /* сервер прислал ответ OCSP */
    int ocsp;                        /* 1 - ответ OCSP проверен и говорит good */
    const char* ocsp_why;            /* почему ответ не даёт гарантии (устарел, unknown, ...) или 0 */
    int resumed;                     /* 1 - соединение по билету сессии (PSK), без сертификатов */
    tls_ticket_t ticket;             /* последний полученный билет для следующего tls_connect */
    char host[64];
    i64 now0;
    u8 resm[32];                     /* resumption_master_secret */
    u16 cipher;
    u32 cert_count, leaf_len;
    u8 rkey[32], riv[12], wkey[32], wiv[12], rsec[32];
    aes_gcm_t rg, wg;                /* развёрнутые ключи AES-128-GCM (если сервер выбрал его) */
    u64 rseq, wseq;
    u8 in[16384 + 512]; u32 in_len;  /* сырой входной поток (записи) */
    u8 pt[16384 + 256]; u32 pt_off, pt_len;   /* расшифрованные данные приложения */
} tls_t;

int tls_connect(tls_t* t, i64 sock, const char* host, u32 timeout_ms, const u8* roots, u32 roots_len, i64 now,
                const tls_ticket_t* resume);   /* 0 - готово; resume - билет прошлого соединения или 0 */
i64 tls_send(tls_t* t, const void* p, u32 n);                             /* отправлено байт или <0 */
i64 tls_recv(tls_t* t, void* p, u32 n, u32 timeout_ms);                   /* байт; 0 - сервер закрыл; <0 - ошибка */
void tls_close(tls_t* t);                                                 /* close_notify (сокет не закрывает) */
#endif
