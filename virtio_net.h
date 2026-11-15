#ifndef KIRA_VIRTIO_NET_H
#define KIRA_VIRTIO_NET_H
/* Сетевая карта virtio-net по PCI: 0 - найдена; карту отдаёт virtio_net_netdev, в стеке её регистрирует корень сборки (wire_net_probe). */
#include "netdev.h"
int virtio_net_probe(void);
netdev_t* virtio_net_netdev(void);       /* карта после успешного probe */
#endif
