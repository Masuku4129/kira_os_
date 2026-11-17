#ifndef KIRA_NETDEV_H
#define KIRA_NETDEV_H
#include <stdint.h>

/* Интерфейс сетевой карты для драйверов (ур.2): драйвер заполняет netdev_t и отдаёт его наружу (e1000_netdev, virtio_net_netdev);
   в стеке (net.h, ур.6) карту регистрирует корень сборки через net_register. Драйверы стек не включают. */

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

#endif
