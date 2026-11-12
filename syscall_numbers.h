#ifndef KIRA_SYSCALL_NUMBERS_H
#define KIRA_SYSCALL_NUMBERS_H

/* Stable KIRA user syscall numbers. Values are part of the Native ABI. */
#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_TICKS  3
#define SYS_SLEEP  4
#define SYS_YIELD  5
#define SYS_GETPID 6
#define SYS_GFX_CREATE  7
#define SYS_GFX_PRESENT 8
#define SYS_GFX_EVENT   9
#define SYS_FS         10
#define SYS_HEAP       11
#define SYS_THREAD_CREATE 12
#define SYS_THREAD_EXIT   13
#define SYS_GETTID        14
#define SYS_FUTEX_WAIT    15
#define SYS_FUTEX_WAKE    16
#define SYS_PORT_OPEN     17
#define SYS_PORT_SEND     18
#define SYS_PORT_RECV     19
#define SYS_SHM_OPEN      20
#define SYS_SHM_MAP       21
#define SYS_SHM_UNMAP     22
#define SYS_CLOSE         23
#define SYS_CLOCK_NS      24
#define SYS_NET_DNS       25
#define SYS_NET_CONNECT   26
#define SYS_NET_SEND      27
#define SYS_NET_RECV      28
#define SYS_NET_CLOSE     29
#define SYS_ARGS          30
#define SYS_NET_LISTEN    31
#define SYS_NET_ACCEPT    32
#define SYS_SND_OPEN      33
#define SYS_SND_WRITE     34
#define SYS_SND_CLOSE     35
#define SYS_RANDOM        36
#define SYS_TIME          37
#define SYS_UDP_OPEN      38
#define SYS_UDP_SEND      39
#define SYS_UDP_RECV      40
#define SYS_UDP_CLOSE     41
#define SYS_SND_VOLUME    42
#define SYS_SND_RECORD    43
#define SYS_NET_CONNECT_SA 44
#define SYS_UDP_SENDTO    45
#define SYS_UDP_RECVFROM  46
#define SYS_NET_PEER      47
#define SYS_JIT           56

#endif
