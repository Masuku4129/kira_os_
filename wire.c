#include "wire.h"
#include "proc.h"
#include "gfxsrv.h"
#include "net.h"
#include "kdrv.h"
#include "life.h"
#include "klog.h"
#include "wm.h"
#include "dock.h"
#include "mind_sys.h"
#include "buildcmd.h"
#include "hwprofile.h"
#include "uheap.h"
#include "blk.h"
#include "kfs.h"
#include "store.h"
#include "usbmsc_fs.h"
#include "e1000.h"
#include "virtio_net.h"
#include "display.h"
#include "syscall.h"
#include "fsrv.h"

static void wire_gone_early(uint32_t pid, uint32_t drv) {
    (void)drv;
    gfx_proc_gone(pid);                    /* окно приложения снимет поток дисплея */
    net_proc_gone(pid);                    /* соединения TCP программы закрываются */
}

static void wire_gone_late(uint32_t pid, uint32_t drv) {
    if (drv) kdrv_proc_gone(drv - 1, pid);
}

_Static_assert(SVC_NET_CONNECT_SA == NET_SYS_CONNECT_SA && SVC_NET_PEER == NET_SYS_PEER, "коды net_sys расходятся с syscall.h");
_Static_assert(SVC_UDP_SENDTO == NET_UDP_SENDTO && SVC_UDP_RECVFROM == NET_UDP_RECVFROM, "коды net_udp_sys расходятся с syscall.h");

static int wire_own_disk(blk_dev_t* d) { return store_detect(d) || kfs_detect(d, KFS_BASE_LBA) == 0; }

static int wire_win_create(const char* title, int w, int h, uint32_t* mem) {
    return wm_create_user(title, WM_AUTO, WM_AUTO, w, h, mem);
}

void wire_init(void) {
    static const gfx_wm_ops_t wm_ops = { wire_win_create, wm_destroy, dock_title, wm_set_title, wm_set_frame_seq, wm_raise,
                                         wm_visible, wm_invalidate, wm_top };
    gfx_set_wm_ops(&wm_ops);
    static const mind_sys_env_t mind_env = { display_get_fx, display_set_fx, display_get_stats, display_compositor_stats, gfx_focus_pid, build_mind_tick };
    mind_sys_set_env(&mind_env);
#if defined(__x86_64__)   /* профиль железа (CPUID) есть только на x86-64; на ARM64 ядро его не строит */
    hw_set_describe_extra(uheap_describe_limits);
#endif
    static const syscall_services_t services = {
        gfx_sys_create, gfx_sys_present, gfx_sys_event, fsrv_syscall, net_sys, net_udp_sys, kdrv_sys };
    syscall_set_services(&services);
    proc_add_gone_hook(wire_gone_early, 0);
    proc_add_gone_hook(wire_gone_late, 1);
    proc_set_note_hook(life_note);
    klog_set_panic_flush(display_panic_flush);
    msc_set_fs_ops(&usbmsc_fs_ops);
    blk_set_own_detect(wire_own_disk);
}

/* Сетевая карта: Intel e1000, иначе virtio-net; найденную регистрирует стек (драйверы ур.2 стек не включают). */
void wire_net_probe(void) {
    if (e1000_probe() == 0) net_register(e1000_netdev());
    else if (virtio_net_probe() == 0) net_register(virtio_net_netdev());
}
