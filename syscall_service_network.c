#include "syscall_services.h"
#include "syscall_numbers.h"
#include "net.h"

int syscall_service_network(uint64_t nr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t* result) {
    if (!result) return 0;
    switch (nr) {
    case SYS_NET_DNS: case SYS_NET_CONNECT: case SYS_NET_SEND:
    case SYS_NET_RECV: case SYS_NET_CLOSE: case SYS_NET_LISTEN:
    case SYS_NET_ACCEPT:
        *result = (uint64_t)net_sys((unsigned)(nr - SYS_NET_DNS), a1, a2, a3);
        return 1;
    case SYS_UDP_OPEN: case SYS_UDP_SEND: case SYS_UDP_RECV:
    case SYS_UDP_CLOSE:
        *result = (uint64_t)net_udp_sys((unsigned)(nr - SYS_UDP_OPEN), a1, a2, a3);
        return 1;
    case SYS_NET_CONNECT_SA:
        *result = (uint64_t)net_sys(NET_SYS_CONNECT_SA, a1, a2, a3);
        return 1;
    case SYS_NET_PEER:
        *result = (uint64_t)net_sys(NET_SYS_PEER, a1, a2, a3);
        return 1;
    case SYS_UDP_SENDTO:
        *result = (uint64_t)net_udp_sys(NET_UDP_SENDTO, a1, a2, a3);
        return 1;
    case SYS_UDP_RECVFROM:
        *result = (uint64_t)net_udp_sys(NET_UDP_RECVFROM, a1, a2, a3);
        return 1;
    default:
        return 0;
    }
}
