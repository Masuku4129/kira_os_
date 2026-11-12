#include "syscall_services.h"
#include "syscall_numbers.h"

#include "audio.h"
#include "fsrv.h"
#include "gfxsrv.h"
#include "net.h"
#include "proc.h"
#include "rtc.h"

/*
 * Transitional service-facing syscall adapters.
 *
 * This file deliberately isolates service API knowledge from the core syscall
 * mechanism. Calls still execute in kernel context today; a future Service
 * Manager/IPC migration must preserve these Native ABI numbers and semantics.
 */
int syscall_service_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result) {
    if (!result) return 0;
    switch (nr) {
    case SYS_GFX_CREATE: {
        proc_space_lock();
        int64_t r = gfx_sys_create(proc_current_pid(), proc_current_name(),
                                   (uint32_t)a1, (uint32_t)a2);
        proc_space_unlock();
        *result = (uint64_t)r;
        return 1;
    }
    case SYS_GFX_PRESENT:
        *result = (uint64_t)gfx_sys_present(proc_current_pid());
        return 1;
    case SYS_GFX_EVENT:
        *result = (uint64_t)gfx_sys_event(proc_current_pid(), a1);
        return 1;
    case SYS_FS:
        *result = (uint64_t)fsrv_syscall(a1);
        return 1;
    case SYS_NET_DNS: case SYS_NET_CONNECT: case SYS_NET_SEND:
    case SYS_NET_RECV: case SYS_NET_CLOSE: case SYS_NET_LISTEN:
    case SYS_NET_ACCEPT:
        *result = (uint64_t)net_sys((unsigned)(nr - SYS_NET_DNS), a1, a2, a3);
        return 1;
    case SYS_SND_OPEN: case SYS_SND_WRITE: case SYS_SND_CLOSE:
        *result = (uint64_t)audio_sys((unsigned)(nr - SYS_SND_OPEN), a1, a2, a3);
        return 1;
    case SYS_SND_VOLUME:
        *result = (uint64_t)audio_sys(3, a1, a2, 0);
        return 1;
    case SYS_SND_RECORD:
        *result = (uint64_t)audio_record_sys(a1, a2, a3);
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
    case SYS_TIME: {
        rtc_time_t tm;
        *result = rtc_now(0, &tm) == 0
            ? (uint64_t)rtc_to_epoch(&tm) : (uint64_t)-1;
        return 1;
    }
    default:
        return 0;
    }
}
