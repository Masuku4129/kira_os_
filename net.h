#ifndef KIRA_NET_H
#define KIRA_NET_H
#include <stdint.h>

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
} netdev_t;

void net_register(netdev_t* d);       /* драйвер нашёл карту; первая карта становится основной */
int  net_start(void);                 /* поток «net» и DHCP, если карта есть; число карт */

/* Чистые функции (проверяются на хосте) */
uint16_t net_csum(const void* data, uint32_t len);                 /* интернет-контрольная сумма (RFC 1071) */
int      net_parse_ip(const char* s, uint32_t* ip);                /* «10.0.2.2» -> адрес в порядке хоста; 0 - ok */

/* Команды консоли */
void netcmd_net(const char* arg);     /* состояние: карта, канал, адрес, шлюз, счётчики */
void netcmd_ping(const char* arg);    /* ping адрес [число] */
void netcmd_dns(const char* arg);     /* dns имя */
void netcmd_http(const char* arg);    /* http адрес[:порт] [путь] */

/* Для ядра (и будущих сокетов программ): DNS и соединения TCP. Блокирующие, зовутся из потоков (не из «net»). */
int  net_dns(const char* name, uint32_t* ip);                     /* 0 - адрес найден (или name - уже адрес) */
int  tcp_connect(uint32_t ip, uint16_t port);                     /* номер соединения или <0 */
int  tcp_send(int h, const void* data, uint32_t len);             /* отправлено байт (подтверждено) или <0 */
int  tcp_recv(int h, void* buf, uint32_t max, uint32_t timeout_ticks);   /* байт; 0 - собеседник закрыл; <0 - сброс/срок */
void tcp_close(int h);
int  tcp_listen(uint16_t port);                                   /* номер слушателя или <0 (-3 порт занят) */
int  tcp_accept(int l, uint32_t timeout_ticks);                   /* номер соединения или <0 (-5 срок) */
void tcp_unlisten(int l);

/* Системные вызовы сети для программ (syscall.c): соединение принадлежит открывшему процессу. */
enum { NET_SYS_DNS = 0, NET_SYS_CONNECT = 1, NET_SYS_SEND = 2, NET_SYS_RECV = 3, NET_SYS_CLOSE = 4,
       NET_SYS_LISTEN = 6, NET_SYS_ACCEPT = 7 };          /* 5 - вызов ARGS (не сеть) */
int64_t net_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
void net_proc_gone(uint32_t pid);       /* процесс завершён: его соединения закрыть */
#endif
