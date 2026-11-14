#ifndef KIRA_NET_H
#define KIRA_NET_H
#include <stdint.h>

/* TCP handles are opaque generation-tagged values; the low bits select a table slot.
   Listener handles use a separate positive tag so they cannot alias socket handles. */
enum { NET_TCP_SLOT_BITS = 3, NET_TCP_SLOT_MASK = 7, NET_TCP_GENERATION_MAX = 0x1FFF,
       NET_LISTEN_SLOT_BITS = 2, NET_LISTEN_SLOT_MASK = 3, NET_LISTEN_GENERATION_MAX = 0x3FFF };
#define NET_LISTEN_HANDLE_TAG 0x40000000u
static inline int net_tcp_handle_make(unsigned slot, unsigned generation) {
    return slot <= NET_TCP_SLOT_MASK && generation && generation <= NET_TCP_GENERATION_MAX
        ? (int)((generation << NET_TCP_SLOT_BITS) | slot) : -1;
}
static inline int net_tcp_handle_decode(int64_t handle, unsigned* slot, unsigned* generation) {
    if (handle <= 0 || handle > 0xFFFF) return -1;
    *slot = (unsigned)handle & NET_TCP_SLOT_MASK;
    *generation = (unsigned)handle >> NET_TCP_SLOT_BITS;
    return *generation ? 0 : -1;
}
static inline int64_t net_listen_handle_make(unsigned slot, unsigned generation) {
    return slot <= NET_LISTEN_SLOT_MASK && generation && generation <= NET_LISTEN_GENERATION_MAX
        ? (int64_t)(NET_LISTEN_HANDLE_TAG | (generation << NET_LISTEN_SLOT_BITS) | slot) : -1;
}
static inline int net_listen_handle_decode(int64_t handle, unsigned* slot, unsigned* generation) {
    if (handle < NET_LISTEN_HANDLE_TAG ||
        ((uint64_t)handle & ~(uint64_t)(NET_LISTEN_HANDLE_TAG | 0xFFFFu))) return -1;
    unsigned value = (unsigned)handle & 0xFFFFu;
    *slot = value & NET_LISTEN_SLOT_MASK;
    *generation = value >> NET_LISTEN_SLOT_BITS;
    return *generation ? 0 : -1;
}

/* Сеть: драйвер карты (e1000 и др.) даёт кадры Ethernet, стек ядра - ARP, IPv4, ICMP (ping), UDP, клиент DHCP.
   Обработка входящих кадров и DHCP - в потоке «net»; консоль только спрашивает. */

typedef struct netdev {
    char name[12];
    uint8_t mac[6];
    int  (*send)(struct netdev* d, const void* frame, uint32_t len);   /* 0 - кадр отдан карте */
    int  (*recv)(struct netdev* d, void* buf, uint32_t max);           /* длина кадра или 0 - пусто */
    int  (*link)(struct netdev* d);                                     /* 1 - кабель/канал есть */
    void* priv;
    struct irqwait* iw;                                                 /* прерывание приёма (0 - опрос по тику) */
    int  (*mcast_add)(struct netdev* d, const uint8_t mac[6]);          /* принимать и этот групповой MAC; 0 у карт, берущих всё */
} netdev_t;

void net_register(netdev_t* d);       /* драйвер нашёл карту; первая карта становится основной */
netdev_t* net_dev(void);              /* основная карта (0 - нет): драйвер Ring 3 не регистрирует вторую */
int  net_start(void);                 /* поток «net» и DHCP, если карта есть; число карт */

/* Чистые функции (проверяются на хосте) */
uint16_t net_csum(const void* data, uint32_t len);                 /* интернет-контрольная сумма (RFC 1071) */
int      net_parse_ip(const char* s, uint32_t* ip);                /* «10.0.2.2» -> адрес в порядке хоста; 0 - ok */

/* Команды консоли */
void netcmd_net(const char* arg);     /* состояние: карта, канал, адрес, шлюз, счётчики */
void netcmd_ping(const char* arg);    /* ping адрес [число] */
void netcmd_dns(const char* arg);     /* dns имя */
void netcmd_http(const char* arg);    /* http адрес[:порт] [путь] */
void netcmd_ping6(const char* arg);   /* ping6 адрес [число] */

/* Для ядра (и будущих сокетов программ): DNS и соединения TCP. Блокирующие, зовутся из потоков (не из «net»). */
int  net_dns(const char* name, uint32_t* ip);                     /* 0 - адрес найден (или name - уже адрес) */
int  tcp_connect(uint32_t ip, uint16_t port);                     /* номер соединения или <0 */
int  tcp_connect6(const uint8_t ip[16], uint16_t port);           /* то же по IPv6 (адрес - 16 байт) */
int  tcp_listen6(uint16_t port);                                  /* слушатель IPv6 (tcp_listen - IPv4) */
int  tcp_peer(int h, uint8_t addr[16], uint16_t* port);           /* собеседник: семейство 4/6 (адрес IPv4 - в первых 4 байтах) */
int  tcp_send(int h, const void* data, uint32_t len);             /* отправлено байт (подтверждено) или <0 */
int  tcp_recv(int h, void* buf, uint32_t max, uint32_t timeout_ticks);   /* байт; 0 - собеседник закрыл; <0 - сброс/срок */
void tcp_close(int h);
int  tcp_listen(uint16_t port);                                   /* номер слушателя или <0 (-3 порт занят) */
int  tcp_accept(int l, uint32_t timeout_ticks);                   /* номер соединения или <0 (-5 срок) */
void tcp_unlisten(int l);

/* Системные вызовы сети для программ (syscall.c): соединение принадлежит открывшему процессу. */
enum { NET_SYS_DNS = 0, NET_SYS_CONNECT = 1, NET_SYS_SEND = 2, NET_SYS_RECV = 3, NET_SYS_CLOSE = 4,
       NET_SYS_LISTEN = 6, NET_SYS_ACCEPT = 7,            /* 5 - вызов ARGS (не сеть) */
       NET_SYS_CONNECT_SA = 8, NET_SYS_PEER = 9 };        /* вызовы 44 и 47: адрес сокета любого семейства */
int64_t net_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
void net_proc_gone(uint32_t pid);       /* процесс завершён: его соединения закрыть */
/* UDP для программ: вызовы 38-41 (open, send, recv, close); получатель/отправитель - в аргументах и заголовке
   из 8 байт перед данными при приёме (адрес 4, порт 2, длина 2 - большой порядок). */
enum { NET_UDP_OPEN = 0, NET_UDP_SEND = 1, NET_UDP_RECV = 2, NET_UDP_CLOSE = 3,
       NET_UDP_SENDTO = 4, NET_UDP_RECVFROM = 5 };       /* вызовы 45 и 46: адрес сокета любого семейства */
/* Адрес сокета для вызовов 44-47: family 4 (адрес - первые 4 байта) или 6; порт - в порядке хоста, адрес - сетевой порядок. */
typedef struct { uint16_t family, port; uint8_t addr[16]; } ksockaddr_t;
/* Вызов 46 кладёт в буфер перед данными: ksockaddr_t отправителя (20 байт) и uint32 полной длины датаграммы (итого 24).
   Семейство сокета задаёт открытие: порт | семейство << 16 (0 или 4 - IPv4, 6 - IPv6); старые вызовы 39-40 - только IPv4. */
int64_t net_udp_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
#endif
