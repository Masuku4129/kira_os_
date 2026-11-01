#ifndef KIRA_IP6_H
#define KIRA_IP6_H
#include <stdint.h>

/* IPv6: чистые функции без зависимостей от ядра (проверяются на хосте; ip6.c собирается и в программы Ring 3).
   Адреса - 16 байт в сетевом порядке. Сообщения ND - RFC 4861, SLAAC - RFC 4862, адреса - RFC 4291, текст - RFC 4291/5952,
   контрольные суммы - RFC 1071 и псевдозаголовок RFC 8200 п. 8.1. */

/* ---- адреса ---- */
int  ip6_parse(const char* s, uint8_t out[16]);       /* «fe80::1», «::ffff:1.2.3.4»; до пробела или конца; 0 - ок, -1 - ошибка */
void ip6_format(const uint8_t a[16], char out[40]);   /* RFC 5952: строчные, самая длинная серия нулей (>= 2 групп) как «::» */
int  ip6_eq(const uint8_t* a, const uint8_t* b);
int  ip6_prefix_eq(const uint8_t* a, const uint8_t* b, unsigned bits);
int  ip6_is_unspec(const uint8_t* a);
int  ip6_is_mcast(const uint8_t* a);
int  ip6_is_linklocal(const uint8_t* a);              /* fe80::/10 */
void ip6_eui64(const uint8_t mac[6], uint8_t iid[8]);                         /* RFC 4291, приложение A */
void ip6_linklocal(const uint8_t mac[6], uint8_t a[16]);                      /* fe80::/64 + EUI-64 */
int  ip6_slaac(const uint8_t prefix[16], unsigned plen, const uint8_t mac[6], uint8_t a[16]);   /* RFC 4862: только /64; -1 иначе */
void ip6_solicited(const uint8_t a[16], uint8_t out[16]);                     /* ff02::1:ff + младшие 24 бита адреса */
void ip6_mcast_mac(const uint8_t a[16], uint8_t mac[6]);                      /* 33:33 + последние 4 байта (RFC 2464) */

/* ---- контрольные суммы ---- */
uint32_t net_sum(uint32_t acc, const void* d, uint32_t n);   /* добавить слова; нечётный хвост - только в последнем вызове */
uint16_t net_sum_end(uint32_t acc);                          /* свернуть и дополнить */
uint16_t net_csum4(uint32_t src, uint32_t dst, uint8_t proto, const void* d, uint32_t n);   /* TCP/UDP по IPv4; 0 - сумма верна */
uint16_t net_csum6(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, uint32_t n);   /* ICMPv6/TCP/UDP */

/* ---- заголовки расширения ---- */
/* b - байты после основного заголовка IPv6 (или собранная часть), nh - тип первого заголовка. Возвращает 0:
   *proto - протокол верхнего уровня, *off - начало его данных; 1: найден заголовок фрагмента, *off - его место (8 байт:
   следующий, резерв, смещение и M, идентификатор 32 бита); -1: испорчено или не по силам (маршрутизация с остатком). */
int  ip6_walk(const uint8_t* b, uint32_t len, uint8_t nh, uint8_t* proto, uint32_t* off);

/* ---- сообщения Neighbor Discovery (ICMPv6, RFC 4861) ----
   Строители пишут сообщение вместе с контрольной суммой, возвращают длину. slla/tlla - MAC или 0 (без опции). */
uint32_t nd_build_ns(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* target, const uint8_t* slla);
#define ND_NA_R 0x80
#define ND_NA_S 0x40
#define ND_NA_O 0x20
uint32_t nd_build_na(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* target, uint8_t flags, const uint8_t* tlla);
uint32_t nd_build_rs(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* slla);
uint32_t icmp6_build_echo(uint8_t* o, const uint8_t* src, const uint8_t* dst, uint8_t type, uint16_t id, uint16_t seq,
                          const uint8_t* data, uint32_t n);

/* Опция адреса канального уровня (тип 1 - источник, 2 - получатель) среди опций o[0..n): 1 - есть (mac), 0 - нет, -1 - опции испорчены
   (длина 0 или за пределом: сообщение отбрасывается целиком, RFC 4861 п. 6.1.1). */
int  nd_lladdr(const uint8_t* o, uint32_t n, uint8_t type, uint8_t mac[6]);

#define RA_PI_MAX 4
#define RA_PI_ONLINK 0x80
#define RA_PI_AUTO   0x40
typedef struct { uint8_t plen, flags; uint32_t valid, pref; uint8_t prefix[16]; } ra_pi_t;
typedef struct {
    uint8_t hop, flags;                       /* flags: 0x80 M, 0x40 O */
    uint16_t lifetime;                        /* срок маршрутизатора по умолчанию, с (0 - не маршрутизатор по умолчанию) */
    uint32_t reachable, retrans;
    int have_slla; uint8_t slla[6];
    uint16_t mtu;                             /* 0 - опции нет */
    int npi; ra_pi_t pi[RA_PI_MAX];           /* префиксы (лишние сверх RA_PI_MAX пропускаются) */
    int have_dns; uint8_t dns[16]; uint32_t dns_life;   /* первый адрес из RDNSS (RFC 8106) */
} ra_t;
int  nd_parse_ra(const uint8_t* m, uint32_t n, ra_t* r);   /* m - сообщение ICMPv6 с байта «тип»; 0 - ок, -1 - испорчено */
#endif
