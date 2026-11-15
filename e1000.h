#ifndef KIRA_E1000_H
#define KIRA_E1000_H
/* Сетевая карта Intel 8254x по PCI: 0 - найдена; карту отдаёт e1000_netdev, в стеке её регистрирует корень сборки (wire_net_probe). */
#include "netdev.h"
int e1000_probe(void);
netdev_t* e1000_netdev(void);       /* карта после успешного probe */
#endif
