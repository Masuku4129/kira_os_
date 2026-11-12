/* Intel 8254x/8257x (e1000, e1000e 82574): сетевая карта QEMU/Limbo по умолчанию и многих ПК. Кольца дескрипторов
   приёма и передачи в памяти ядра. Если карта умеет MSI (82546, 82571, 82574 - QEMU e1000e) - прерывание приёма будит
   поток «net»; у 82540EM (e1000 QEMU и Limbo) MSI нет, а маршрут INTx на x86 описан только в AML (_PRT) - там опрос
   каждый тик. */
#include "e1000.h"
#include "net.h"
#include "pci.h"
#include "virtio.h"          /* virtio_dma_alloc (непрерывная память для устройства), MMIO_R32/W32 */
#include "uart.h"
#include "spinlock.h"
#include "vmm.h"
#include "cpu.h"
#include "msi.h"

int platform_dma_noncoherent(void) __attribute__((weak));

enum { CTRL = 0x0, STATUS = 0x8, EERD = 0x14, ICR = 0xC0, IMS = 0xD0, IMC = 0xD8, RCTL = 0x100, TCTL = 0x400, TIPG = 0x410,
       RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
       TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818, MTA = 0x5200, RAL0 = 0x5400, RAH0 = 0x5404 };
#define NRX 64
#define NTX 32
#define BUF 2048

typedef struct { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } rxd_t;
typedef struct { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } txd_t;

static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x1009, 0x100C, 0x100D, 0x1010, 0x1011, 0x1015, 0x1026, 0x1076, 0x1079, 0x107C,
                                0x10D3 };   /* 82574L (QEMU e1000e): тот же устаревший формат дескрипторов */

static struct {
    volatile uint8_t* r;
    rxd_t* rx; txd_t* tx;
    uint64_t rx_p, tx_p;
    uint8_t* rxb; uint8_t* txb;
    uint64_t rxb_p, txb_p;
    unsigned rx_next, tx_next;
    spinlock_t txlk;
    netdev_t nd;
    irqwait_t iw;
    uint64_t irqs;
} e = { .iw = IRQWAIT_INIT };

/* Прерывание (MSI): причина читается - это её же и сбрасывает; поток сети просыпается и забирает кадры. */
static void irq(void* ctx) { (void)ctx; uint32_t c = MMIO_R32(e.r + ICR); if (c) { e.irqs++; irqwait_fire(&e.iw); } }

static uint32_t rd(unsigned o) { return MMIO_R32(e.r + o); }
static void wr(unsigned o, uint32_t v) { MMIO_W32(e.r + o, v); }

static int send(netdev_t* d, const void* frame, uint32_t len) {
    (void)d;
    if (len > BUF || len < 14) return -1;
    uint32_t n = len;
    if (len < 60) len = 60;                                        /* короче нельзя: дополняется нулями */
    uint64_t f = spin_lock_irqsave(&e.txlk);
    unsigned slot = e.tx_next;
    txd_t* t = &e.tx[slot];
    uint64_t td_phys = e.tx_p + (uint64_t)slot * sizeof(*t);
    if (virtio_dma_sync_for_cpu(t, td_phys, sizeof(*t)) != 0) {
        spin_unlock_irqrestore(&e.txlk, f); return -1;
    }
    if (t->cmd && !(t->status & 1)) { spin_unlock_irqrestore(&e.txlk, f); return -1; }   /* кольцо полно */
    uint8_t* b = e.txb + slot * BUF;
    const uint8_t* s = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) b[i] = i < n ? s[i] : 0;
    t->addr = e.txb_p + e.tx_next * BUF; t->len = (uint16_t)len; t->cso = 0; t->css = 0; t->special = 0;
    t->status = 0; t->cmd = 1 | 2 | 8;                            /* EOP, IFCS (контрольную сумму кадра считает карта), RS */
    if (virtio_dma_sync_for_device(b, e.txb_p + (uint64_t)slot * BUF, len) != 0 ||
        virtio_dma_sync_for_device(t, td_phys, sizeof(*t)) != 0) {
        spin_unlock_irqrestore(&e.txlk, f); return -1;
    }
    e.tx_next = (slot + 1) % NTX;
    cpu_mb();
    wr(TDT, e.tx_next);
    spin_unlock_irqrestore(&e.txlk, f);
    return 0;
}

static int recv(netdev_t* d, void* buf, uint32_t max) {
    (void)d;
    unsigned slot = e.rx_next;
    rxd_t* x = &e.rx[slot];
    uint64_t rd_phys = e.rx_p + (uint64_t)slot * sizeof(*x);
    if (virtio_dma_sync_for_cpu(x, rd_phys, sizeof(*x)) != 0) return -1;
    cpu_mb();
    if (!(x->status & 1)) return 0;                               /* DD: кадр ещё не принят */
    uint32_t n = x->len;
    int ok = (x->status & 2) && !x->errors && n <= max;           /* целый кадр (EOP), без ошибок */
    if (ok) {
        const uint8_t* s = e.rxb + slot * BUF;
        if (virtio_dma_sync_for_cpu((void*)s, e.rxb_p + (uint64_t)slot * BUF, n) != 0) return -1;
        uint8_t* o = (uint8_t*)buf; for (uint32_t i = 0; i < n; i++) o[i] = s[i];
    }
    x->status = 0;
    if (virtio_dma_sync_for_device(x, rd_phys, sizeof(*x)) != 0) return -1;
    unsigned done = slot;
    e.rx_next = (e.rx_next + 1) % NRX;
    cpu_mb();
    wr(RDT, done);                                                /* буфер снова у карты */
    return ok ? (int)n : 0;
}

static int link(netdev_t* d) { (void)d; return (rd(STATUS) >> 1) & 1; }

/* Групповой адрес в таблицу MTA (хэш по битам 47:36 адреса при RCTL.MO = 0, как у драйвера Intel): карта принимает кадры
   на него, остальные групповые по-прежнему отбрасываются. Биты только добавляются (сброс - при инициализации). */
static int mcast_add(netdev_t* d, const uint8_t mac[6]) {
    (void)d;
    unsigned h = (((unsigned)mac[4] >> 4) | ((unsigned)mac[5] << 4)) & 0xFFF;
    wr(MTA + 4 * (h >> 5), rd(MTA + 4 * (h >> 5)) | (1u << (h & 31)));
    return 0;
}

static int e82574;                                                 /* у 82574 другой формат EERD */
static uint16_t eeprom(uint8_t a) {
    wr(EERD, 1u | ((uint32_t)a << (e82574 ? 2 : 8)));
    for (int i = 0; i < 100000; i++) { uint32_t v = rd(EERD); if (v & (e82574 ? 0x2 : 0x10)) return (uint16_t)(v >> 16); }
    return 0xFFFF;
}

int e1000_probe(void) {
    const pci_dev_t* p = 0;
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0] && !p; i++) p = pci_find(0x8086, ids[i]);
    if (!p) return -1;
    e82574 = p->device == 0x10D3;
    if (p->bar[0].io || !p->bar[0].base) { uart_print("[KIRA NET] e1000: register BAR is not memory - not supported.\n"); return -2; }
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    e.r = (volatile uint8_t*)vmm_map_mmio(p->bar[0].base, p->bar[0].size ? p->bar[0].size : 0x20000, VMM_MMIO_UC);
    if (!e.r) return -3;

    wr(IMC, 0xFFFFFFFF);                                          /* без прерываний: опрос */
    wr(CTRL, rd(CTRL) | (1u << 26));                              /* сброс */
    for (int i = 0; i < 1000000 && (rd(CTRL) & (1u << 26)); i++) cpu_relax();
    wr(IMC, 0xFFFFFFFF);
    wr(CTRL, (rd(CTRL) | (1u << 6) | (1u << 5)) & ~(1u << 3));    /* SLU, ASDE; без сброса канала */

    uint32_t lo = rd(RAL0), hi = rd(RAH0);
    if (hi & (1u << 31)) {
        for (int i = 0; i < 4; i++) e.nd.mac[i] = (uint8_t)(lo >> (8 * i));
        e.nd.mac[4] = (uint8_t)hi; e.nd.mac[5] = (uint8_t)(hi >> 8);
    } else {                                                      /* адрес не загружен - прочитать из EEPROM */
        for (int i = 0; i < 3; i++) { uint16_t w = eeprom((uint8_t)i); e.nd.mac[2 * i] = (uint8_t)w; e.nd.mac[2 * i + 1] = (uint8_t)(w >> 8); }
        wr(RAL0, (uint32_t)e.nd.mac[0] | (uint32_t)e.nd.mac[1] << 8 | (uint32_t)e.nd.mac[2] << 16 | (uint32_t)e.nd.mac[3] << 24);
        wr(RAH0, (uint32_t)e.nd.mac[4] | (uint32_t)e.nd.mac[5] << 8 | (1u << 31));
    }
    for (int i = 0; i < 128; i++) wr(MTA + 4 * (unsigned)i, 0);

    uint64_t rx_p, tx_p;
    e.rx = (rxd_t*)virtio_dma_alloc(NRX * sizeof(rxd_t), &rx_p);
    e.tx = (txd_t*)virtio_dma_alloc(NTX * sizeof(txd_t), &tx_p);
    e.rx_p = rx_p; e.tx_p = tx_p;
    e.rxb = (uint8_t*)virtio_dma_alloc(NRX * BUF, &e.rxb_p);
    e.txb = (uint8_t*)virtio_dma_alloc(NTX * BUF, &e.txb_p);
    if (!e.rx || !e.tx || !e.rxb || !e.txb) { uart_print("[KIRA NET] e1000: no memory for rings.\n"); return -4; }
    for (unsigned i = 0; i < NRX; i++) { e.rx[i].addr = e.rxb_p + i * BUF; e.rx[i].status = 0; }
    for (unsigned i = 0; i < NTX; i++) { e.tx[i].cmd = 0; e.tx[i].status = 1; }
    if (virtio_dma_sync_for_device(e.rx, rx_p, NRX * sizeof(rxd_t)) != 0 ||
        virtio_dma_sync_for_device(e.tx, tx_p, NTX * sizeof(txd_t)) != 0 ||
        virtio_dma_sync_for_device(e.rxb, e.rxb_p, NRX * BUF) != 0 ||
        virtio_dma_sync_for_device(e.txb, e.txb_p, NTX * BUF) != 0) {
        uart_print("[KIRA NET] e1000: DMA ownership setup failed; device not started.\n");
        return -7;
    }

    wr(RDBAL, (uint32_t)rx_p); wr(RDBAH, (uint32_t)(rx_p >> 32)); wr(RDLEN, NRX * sizeof(rxd_t));
    wr(RDH, 0); wr(RDT, NRX - 1);
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));               /* EN, BAM (широковещательные), SECRC; буфер 2048 */
    wr(TDBAL, (uint32_t)tx_p); wr(TDBAH, (uint32_t)(tx_p >> 32)); wr(TDLEN, NTX * sizeof(txd_t));
    wr(TDH, 0); wr(TDT, 0);
    wr(TIPG, 0x0060200A);
    wr(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12)); /* EN, PSP, порог коллизий, полный дуплекс */

    const char nm[] = "e1000";
    for (unsigned i = 0; i < sizeof nm; i++) e.nd.name[i] = nm[i];
    e.nd.send = send; e.nd.recv = recv; e.nd.link = link; e.nd.priv = 0; e.nd.iw = 0; e.nd.mcast_add = mcast_add;
    int has_msi = 0; uint8_t cid;
    for (uint16_t c = pci_next_cap(p, 0, &cid); c; c = pci_next_cap(p, c, &cid)) if (cid == 0x05) has_msi = 1;
    uint64_t ma; uint32_t md;
    if (has_msi && msi_alloc(p, irq, 0, &ma, &md) == 0 && pci_msi_setup(p, ma, md) == 0) {
        (void)rd(ICR);
        wr(IMS, (1u << 7) | (1u << 6) | (1u << 4) | (1u << 2));   /* RXT0 (кадр принят), RXO, RXDMT0 (кольцо почти пусто), LSC */
        e.nd.iw = &e.iw;
    }
    static const char hx[] = "0123456789abcdef";
    char m[18];
    for (int i = 0; i < 6; i++) { m[3 * i] = hx[e.nd.mac[i] >> 4]; m[3 * i + 1] = hx[e.nd.mac[i] & 15]; m[3 * i + 2] = i < 5 ? ':' : 0; }
    char id[5]; for (int i = 0; i < 4; i++) id[i] = hx[(p->device >> (12 - 4 * i)) & 15]; id[4] = 0;
    uart_print("[KIRA NET] e1000 (8086:"); uart_print(id); uart_print("): MAC "); uart_print(m);
    uart_print(link(&e.nd) ? ", link up" : ", no link yet");
    uart_print(e.nd.iw ? ", receive by MSI interrupt\n" : has_msi ? ", MSI refused - polled\n" : ", no MSI on this card - polled every tick\n");
    net_register(&e.nd);
    return 0;
}
