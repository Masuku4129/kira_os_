/* virtio-net (PCI): сетевая карта QEMU/Limbo (NIC: virtio). Очереди: 0 - приём (буферы отданы устройству заранее),
   1 - передача. Перед каждым кадром - заголовок virtio_net_hdr (12 байт, без разгрузок). Прерывание MSI-X очередей
   будит поток «net» (без него - опрос каждый тик, как у e1000). */
#include "virtio_net.h"
#include "virtio.h"
#include "net.h"
#include "msi.h"
#include "uart.h"
#include "spinlock.h"

#define NRX 64
#define NTX 32
#define BUF 2048
#define HDR 12
#define F_MAC (1ull << 5)

static struct {
    virtio_dev_t dev;
    virtq_t rx, tx;
    uint8_t* rxb; uint64_t rxb_p;
    uint8_t* txb; uint64_t txb_p;
    uint8_t tx_busy[NTX];
    unsigned tx_next;
    spinlock_t lk;
    irqwait_t iw;
    netdev_t nd;
} v = { .lk = SPINLOCK_INIT, .iw = IRQWAIT_INIT };

static void net_abort(void) {
    if (v.dev.common) {
        virtio_fail(&v.dev);
        if (virtio_reset_device(&v.dev) != 0) return;
    }
    if (v.rxb && virtio_dma_free(v.rxb, v.rxb_p, NRX * BUF) == 0) {
        v.rxb = 0; v.rxb_p = 0;
    }
    if (v.txb && virtio_dma_free(v.txb, v.txb_p, NTX * BUF) == 0) {
        v.txb = 0; v.txb_p = 0;
    }
    if (v.rx.mem) (void)virtio_queue_release(&v.rx);
    if (v.tx.mem) (void)virtio_queue_release(&v.tx);
    if (!v.rxb && !v.txb && !v.rx.mem && !v.tx.mem) {
        v.dev = (virtio_dev_t){0};
        v.rx = (virtq_t){0};
        v.tx = (virtq_t){0};
    }
}

static void reclaim(void) {                                       /* отправленные кадры: буферы свободны */
    uint16_t id; uint32_t len;
    while (virtq_take(&v.tx, &id, &len)) if (id < NTX) v.tx_busy[id] = 0;
}

static int send(netdev_t* d, const void* frame, uint32_t len) {
    (void)d;
    if (len > BUF - HDR || len < 14) return -1;
    uint64_t f = spin_lock_irqsave(&v.lk);
    reclaim();
    unsigned id = v.tx_next;
    if (v.tx_busy[id]) { spin_unlock_irqrestore(&v.lk, f); return -1; }   /* все буферы в пути */
    uint8_t* b = v.txb + id * BUF;
    for (unsigned i = 0; i < HDR; i++) b[i] = 0;
    const uint8_t* s = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) b[HDR + i] = s[i];
    if (virtio_dma_sync_for_device(v.txb + id * BUF, v.txb_p + id * BUF,
                                    HDR + len) != 0) {
        spin_unlock_irqrestore(&v.lk, f);
        return -1;
    }
    v.tx_busy[id] = 1;
    v.tx_next = (id + 1) % NTX;
    virtq_post(&v.tx, (uint16_t)id, v.txb_p + id * BUF, HDR + len, 0);
    virtq_kick(&v.tx);
    spin_unlock_irqrestore(&v.lk, f);
    return 0;
}

static int recv(netdev_t* d, void* buf, uint32_t max) {
    (void)d;
    uint16_t id; uint32_t len;
    if (!virtq_take(&v.rx, &id, &len)) return 0;
    if (id >= NRX || virtio_dma_sync_for_cpu(v.rxb + id * BUF,
                                              v.rxb_p + id * BUF, BUF) != 0)
        return 0;
    int n = 0;
    if (len > HDR && len - HDR <= max) {
        const uint8_t* s = v.rxb + id * BUF + HDR; uint8_t* o = (uint8_t*)buf;
        n = (int)(len - HDR);
        for (int i = 0; i < n; i++) o[i] = s[i];
    }
    if (virtio_dma_sync_for_device(v.rxb + id * BUF, v.rxb_p + id * BUF, BUF) == 0) {
        virtq_post(&v.rx, id, v.rxb_p + id * BUF, BUF, 1);
        virtq_kick(&v.rx);
    }
    return n;
}

static int link(netdev_t* d) {
    (void)d;
    if (!(v.dev.features & (1ull << 16))) return 1;               /* без STATUS канал считается есть */
    return MMIO_R16(v.dev.devcfg + 6) & 1;
}

static void irq(void* ctx) { (void)ctx; irqwait_fire(&v.iw); }

int virtio_net_probe(void) {
    if (v.dev.common || v.rx.mem || v.tx.mem || v.rxb || v.txb) return -5;
    const pci_dev_t* p = pci_find(VIRTIO_VENDOR, 0x1041);
    if (!p) p = pci_find(VIRTIO_VENDOR, 0x1000);                  /* переходный с capabilities 1.0 */
    if (!p) return -1;
    if (virtio_attach(&v.dev, p) || !v.dev.devcfg ||
        virtio_begin(&v.dev, F_MAC | (1ull << 16))) {
        net_abort();
        uart_print("[KIRA NET] virtio-net: no virtio 1.0 interface.\n");
        return -2;
    }
    uint64_t ma; uint32_t md; int has_irq = 0;
    if (msi_alloc(p, irq, 0, &ma, &md) == 0 && pci_msix_setup(p, 0, ma, md) == 0) { v.dev.msix_vec = 0; has_irq = 1; }
    if (virtio_queue_setup(&v.dev, &v.rx, 0, NRX) ||
        virtio_queue_setup(&v.dev, &v.tx, 1, NTX)) { net_abort(); return -3; }
    v.rxb = (uint8_t*)virtio_dma_alloc(NRX * BUF, &v.rxb_p);
    v.txb = (uint8_t*)virtio_dma_alloc(NTX * BUF, &v.txb_p);
    if (!v.rxb || !v.txb) { net_abort(); return -4; }
    unsigned nrx = v.rx.size < NRX ? v.rx.size : NRX;
    for (unsigned i = 0; i < nrx; i++) virtq_post(&v.rx, (uint16_t)i, v.rxb_p + i * BUF, BUF, 1);
    virtio_driver_ok(&v.dev);
    virtq_kick(&v.rx);
    if (v.dev.features & F_MAC) for (int i = 0; i < 6; i++) v.nd.mac[i] = MMIO_R8(v.dev.devcfg + (unsigned)i);
    else { static const uint8_t m[6] = { 0x52, 0x54, 0x00, 0x4B, 0x49, 0x52 }; for (int i = 0; i < 6; i++) v.nd.mac[i] = m[i]; }
    const char nm[] = "virtio-net";
    for (unsigned i = 0; i < sizeof nm; i++) v.nd.name[i] = nm[i];
    v.nd.send = send; v.nd.recv = recv; v.nd.link = link; v.nd.priv = 0;
    v.nd.iw = has_irq ? &v.iw : 0;
    static const char hx[] = "0123456789abcdef";
    char m[18];
    for (int i = 0; i < 6; i++) { m[3 * i] = hx[v.nd.mac[i] >> 4]; m[3 * i + 1] = hx[v.nd.mac[i] & 15]; m[3 * i + 2] = i < 5 ? ':' : 0; }
    uart_print("[KIRA NET] virtio-net: MAC "); uart_print(m);
    uart_print(has_irq ? ", receive wakes the network thread by MSI-X\n" : ", polled\n");
    net_register(&v.nd);
    return 0;
}
