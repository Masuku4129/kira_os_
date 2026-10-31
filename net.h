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
} netdev_t;

void net_register(netdev_t* d);       /* драйвер нашёл карту; первая карта становится основной */
int  net_start(void);                 /* поток «net» и DHCP, если карта есть; число карт */

/* Чистые функции (проверяются на хосте) */
uint16_t net_csum(const void* data, uint32_t len);                 /* интернет-контрольная сумма (RFC 1071) */
int      net_parse_ip(const char* s, uint32_t* ip);                /* «10.0.2.2» -> адрес в порядке хоста; 0 - ok */

/* Команды консоли */
void netcmd_net(const char* arg);     /* состояние: карта, канал, адрес, шлюз, счётчики */
void netcmd_ping(const char* arg);    /* ping адрес [число] */
#endif
