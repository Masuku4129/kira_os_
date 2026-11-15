#include "wire.h"
#include "proc.h"
#include "gfxsrv.h"
#include "net.h"
#include "kdrv.h"
#include "life.h"
#include "klog.h"
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

void wire_init(void) {
    hw_set_describe_extra(uheap_describe_limits);
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
