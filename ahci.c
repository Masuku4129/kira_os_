#include "cpu.h"
#include "blk.h"
#include "msi.h"
#include "pci.h"
#include "iommu.h"
#include "uart.h"

/* AHCI (SATA): контроллер в памяти (BAR5, «ABAR»), у каждого порта - список команд, область принятых FIS и таблица
   команды с PRDT. Порядок по спецификации AHCI 1.3: передача от BIOS (BOHC), включение AHCI (GHC.AE), сброс
   контроллера, для порта - остановка, адреса структур, раскрутка (SUD), при необходимости COMRESET, FRE, ожидание
   готовности устройства, ST. Команды ATA: IDENTIFY, READ/WRITE DMA (EXT для LBA48), FLUSH CACHE (EXT).
   Опрос без прерываний: одна команда за раз в слоте 0 (диск всё равно обслуживается под мьютексом).
   Ошибка устройства (TFES) - восстановление порта: стоп, сброс SERR/IS, при зависшем BSY/DRQ - COMRESET, пуск.
   Данные идут через буфер DMA порта (64 КБ): буферы вызывающих могут быть где угодно в виртуальной памяти. */

#define MAXCTL   16
#define MAXPORT  32
#define BOUNCE   (64 * 1024)
#define CHUNK    (BOUNCE / BLK_SECTOR)

/* регистры контроллера */
#define CAP  0x00
#define GHC  0x04
#define IS   0x08
#define PI   0x0C
#define VS   0x10
#define CAP2 0x24
#define BOHC 0x28
#define GHC_AE (1u << 31)
#define GHC_HR 1u
#define GHC_IE 2u
/* регистры порта */
#define P_CLB  0x00
#define P_FB   0x08
#define P_IS   0x10
#define P_IE   0x14
#define P_CMD  0x18
#define P_TFD  0x20
#define P_SIG  0x24
#define P_SSTS 0x28
#define P_SCTL 0x2C
#define P_SERR 0x30
#define P_SACT 0x34
#define P_CI   0x38
#define CMD_ST  (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)
#define CMD_CPD (1u << 20)
#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define IS_TFES (1u << 30)
#define IS_FATAL ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26) | (1u << 24) | (1u << 23))

typedef struct {
    int used;
    const pci_dev_t* pci;
    volatile uint8_t* abar;
    int s64;
    uint32_t cap;
    int irq;                            /* события портов приходят сообщением MSI */
    int dma_failed;                     /* SMMU failure quarantines the whole controller */
} ahci_ctl_t;

typedef struct {
    blk_dev_t dev;
    ahci_ctl_t* c;
    volatile uint8_t* pr;               /* регистры порта */
    int port, lba48, dead;
    uint8_t* mem; uint64_t mem_phys;    /* список команд, FIS, таблица команды */
    uint8_t* buf; uint64_t buf_phys;    /* данные */
    uint32_t errors;
    irqwait_t iw;                       /* конец команды этого порта (прерывание) */
    volatile uint32_t is_seen;          /* флаги PxIS, снятые обработчиком прерывания (для проверки ошибок команды) */
} ahci_port_t;

static ahci_ctl_t ctls[MAXCTL];
static ahci_port_t ports[MAXCTL * 8];
static int nports;

void* virtio_dma_alloc(uint32_t bytes, uint64_t* phys);

static inline uint32_t r32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void w32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
#define BARRIER() cpu_mb()            /* DMA и регистры устройства: на ARM нужен настоящий барьер */

/* Ждать, пока (reg & mask) == want; не дольше ms миллисекунд. 0 - дождались. */
static int wait_reg(volatile uint8_t* b, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
    blk_wait_t w; blk_wait_begin(&w, ms);
    do { if ((r32(b, off) & mask) == want) return 0; } while (blk_wait_step(&w));
    return (r32(b, off) & mask) == want ? 0 : -1;
}

static int port_stop(ahci_port_t* p) {
    w32(p->pr, P_CMD, r32(p->pr, P_CMD) & ~CMD_ST);
    if (wait_reg(p->pr, P_CMD, CMD_CR, 0, 500)) return -1;
    w32(p->pr, P_CMD, r32(p->pr, P_CMD) & ~CMD_FRE);
    return wait_reg(p->pr, P_CMD, CMD_FR, 0, 500);
}

/* COMRESET: заново установить связь с устройством. 0 - устройство на связи. */
static int comreset(ahci_port_t* p) {
    w32(p->pr, P_SCTL, (r32(p->pr, P_SCTL) & ~0xFu) | 1u);
    blk_udelay(2000);                                                   /* не меньше 1 мс */
    w32(p->pr, P_SCTL, r32(p->pr, P_SCTL) & ~0xFu);
    if (wait_reg(p->pr, P_SSTS, 0xF, 3, 1000)) return -1;
    w32(p->pr, P_SERR, 0xFFFFFFFFu);
    return 0;
}

/* Порядок по AHCI 1.3 (10.3.1): сначала приём FIS (FRE) - без него первый D2H FIS устройства с подписью и
   состоянием некуда положить, и TFD так и остаётся «занят»; потом ждать BSY/DRQ = 0; потом ST. */
static int port_start(ahci_port_t* p) {
    w32(p->pr, P_CMD, r32(p->pr, P_CMD) | CMD_FRE);
    if (wait_reg(p->pr, P_TFD, TFD_BSY | TFD_DRQ, 0, 3000)) {          /* диск занят - сброс линии и ещё попытка */
        if (comreset(p) || wait_reg(p->pr, P_TFD, TFD_BSY | TFD_DRQ, 0, 5000)) return -1;
    }
    w32(p->pr, P_CMD, r32(p->pr, P_CMD) | CMD_ST);
    return 0;
}

static void recover(ahci_port_t* p) {
    p->errors++;
    port_stop(p);
    w32(p->pr, P_SERR, 0xFFFFFFFFu);
    w32(p->pr, P_IS, 0xFFFFFFFFu);
    if (port_start(p)) { p->dead = 1; uart_print("[KIRA AHCI] "); uart_print(p->dev.name); uart_print(": device does not recover, disabled\n"); }
}

/* Одна команда ATA. write: данные из буфера порта на диск. 0 - успех. */
static int ata_cmd(ahci_port_t* p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, int write, uint32_t timeout_ms) {
    if (p->dead) return -1;
    volatile uint32_t* hdr = (volatile uint32_t*)p->mem;                /* заголовок слота 0 */
    uint8_t* ct = p->mem + 0x500;
    for (int i = 0; i < 0x90; i++) ct[i] = 0;
    uint8_t* f = ct;                                                    /* H2D Register FIS */
    f[0] = 0x27; f[1] = 0x80; f[2] = cmd;
    f[4] = (uint8_t)lba; f[5] = (uint8_t)(lba >> 8); f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                                                        /* LBA */
    if (cmd == 0xC8 || cmd == 0xCA) f[7] |= (uint8_t)((lba >> 24) & 0x0F);
    f[8] = (uint8_t)(lba >> 24); f[9] = (uint8_t)(lba >> 32); f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count; f[13] = (uint8_t)(count >> 8);
    volatile uint32_t* prd = (volatile uint32_t*)(ct + 0x80);
    uint32_t nprd = 0;
    if (bytes) {
        prd[0] = (uint32_t)p->buf_phys; prd[1] = (uint32_t)(p->buf_phys >> 32); prd[2] = 0;
        prd[3] = (bytes - 1) & 0x3FFFFF;
        nprd = 1;
    }
    hdr[0] = 5u | (write ? 1u << 6 : 0) | (nprd << 16);                /* FIS 5 двойных слов */
    hdr[1] = 0;
    uint64_t ctp = p->mem_phys + 0x500;
    hdr[2] = (uint32_t)ctp; hdr[3] = (uint32_t)(ctp >> 32);
    BARRIER();
    w32(p->pr, P_IS, 0xFFFFFFFFu);
    __atomic_store_n(&p->is_seen, 0, __ATOMIC_RELEASE);
    blk_wait_t w; blk_wait_begin(&w, timeout_ms);
    if (p->c->irq) blk_wait_irq(&w, &p->iw);                          /* до запуска: конец команды не проскочит */
    w32(p->pr, P_CI, 1u);
    do {
        uint32_t is = r32(p->pr, P_IS) | __atomic_load_n(&p->is_seen, __ATOMIC_ACQUIRE);
        if (is & IS_FATAL) break;
        if (!(r32(p->pr, P_CI) & 1u)) {
            BARRIER();
            if (r32(p->pr, P_TFD) & TFD_ERR) break;
            if (bytes && !write && hdr[1] != bytes) break;              /* устройство отдало не всё */
            return 0;
        }
    } while (blk_wait_step(&w));
    uart_print("[KIRA AHCI] "); uart_print(p->dev.name); uart_print(": command "); uart_print_hex(cmd);
    uart_print(" failed, TFD "); uart_print_hex(r32(p->pr, P_TFD)); uart_print(", IS "); uart_print_hex(r32(p->pr, P_IS));
    uart_print(", SERR "); uart_print_hex(r32(p->pr, P_SERR)); uart_print("\n");
    recover(p);
    return -1;
}

static int rw(ahci_port_t* p, uint64_t lba, uint32_t n, int write) {
    uint8_t cmd = p->lba48 ? (write ? 0x35 : 0x25) : (write ? 0xCA : 0xC8);
    if (!p->lba48 && (lba + n > (1ull << 28) || n > 256)) return -1;
    for (int attempt = 0; attempt < 3; attempt++)
        if (ata_cmd(p, cmd, lba, n == 65536 ? 0 : n, n * BLK_SECTOR, write, 10000) == 0) return 0;
    return -1;
}

static int ahci_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) {
    ahci_port_t* p = (ahci_port_t*)d->priv;
    uint8_t* out = (uint8_t*)buf;
    while (count) {
        uint32_t n = count < CHUNK ? count : CHUNK;
        if (rw(p, lba, n, 0)) return -1;
        for (uint32_t i = 0; i < n * BLK_SECTOR; i++) out[i] = p->buf[i];
        out += n * BLK_SECTOR; lba += n; count -= n;
    }
    return 0;
}
static int ahci_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) {
    ahci_port_t* p = (ahci_port_t*)d->priv;
    const uint8_t* in = (const uint8_t*)buf;
    while (count) {
        uint32_t n = count < CHUNK ? count : CHUNK;
        for (uint32_t i = 0; i < n * BLK_SECTOR; i++) p->buf[i] = in[i];
        if (rw(p, lba, n, 1)) return -1;
        in += n * BLK_SECTOR; lba += n; count -= n;
    }
    return 0;
}
static int ahci_flush(blk_dev_t* d) {
    ahci_port_t* p = (ahci_port_t*)d->priv;
    return ata_cmd(p, p->lba48 ? 0xEA : 0xE7, 0, 0, 0, 0, 30000);       /* кэш записи диска - на пластины/флеш */
}

static void model_string(const uint16_t* id, char* out, unsigned cap) {
    unsigned k = 0;
    for (int w = 27; w <= 46 && k + 2 < cap; w++) { out[k++] = (char)(id[w] >> 8); out[k++] = (char)id[w]; }
    while (k && (out[k - 1] == ' ' || out[k - 1] == 0)) k--;
    out[k] = 0;
    unsigned s = 0; while (out[s] == ' ') s++;
    if (s) { unsigned i = 0; while (out[s + i]) { out[i] = out[s + i]; i++; } out[i] = 0; }
}

static void port_init(ahci_ctl_t* c, int pn, int ctl_index) {
    volatile uint8_t* pr = c->abar + 0x100 + (uint32_t)pn * 0x80;
    uint32_t ssts = r32(pr, P_SSTS);
    if ((ssts & 0xF) == 0) return;                                      /* ничего не подключено */
    if (nports >= (int)(sizeof ports / sizeof ports[0])) return;
    ahci_port_t* p = &ports[nports];
    *p = (ahci_port_t){ 0 };
    p->c = c; p->pr = pr; p->port = pn;
    char name[12] = "sata"; int nl = 4;
    int idx = nports; if (idx >= 10) name[nl++] = (char)('0' + idx / 10);
    name[nl++] = (char)('0' + idx % 10); name[nl] = 0;
    for (int i = 0; i <= nl; i++) p->dev.name[i] = name[i];
    (void)ctl_index;

    if (port_stop(p)) { uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": does not stop, skipped\n"); return; }
    p->mem = (uint8_t*)virtio_dma_alloc(4096, &p->mem_phys);
    p->buf = (uint8_t*)virtio_dma_alloc(BOUNCE, &p->buf_phys);
    if (!p->mem || !p->buf) { uart_print("[KIRA AHCI] no DMA memory\n"); return; }
    if (!c->s64 && (p->mem_phys + 4096 > 0x100000000ull || p->buf_phys + BOUNCE > 0x100000000ull)) {
        uart_print("[KIRA AHCI] controller is 32-bit, DMA memory above 4 GB: port skipped\n"); return;
    }
#if defined(__aarch64__) || defined(__x86_64__)
    if (!iommu_device_ready(c->pci)) {
        pci_disable_bus_master(c->pci);
        iommu_unmap_all(c->pci);
        c->dma_failed = 1;
        uart_print("[KIRA AHCI] requester lost DMA authorization; controller quarantined.\n");
        return;
    }
    if (iommu_device_isolated(c->pci)) {
        int map_rc = iommu_map(c->pci, p->mem_phys, p->mem_phys, 4096, IOMMU_R | IOMMU_W);
        if (map_rc != IOMMU_OK) {
            if (!iommu_device_ready(c->pci)) {
                if (pci_disable_bus_master(c->pci))
                    iommu_unmap_all(c->pci);
                else
                    uart_print("[KIRA AHCI] bus mastering did not stop; mappings remain pinned.\\n");
                c->dma_failed = 1;
            }
            uart_print("[KIRA AHCI] command/FIS DMA mapping failed; port skipped.\n");
            return;
        }
        map_rc = iommu_map(c->pci, p->buf_phys, p->buf_phys, BOUNCE, IOMMU_R | IOMMU_W);
        if (map_rc != IOMMU_OK) {
            /* This port has not been started: roll back only its first map so
               already-running ports keep their valid DMA mappings. A failed
               rollback or SMMU quarantine shuts down the entire controller. */
            if (!iommu_device_ready(c->pci) ||
                iommu_unmap(c->pci, p->mem_phys, 4096) != IOMMU_OK) {
                if (pci_disable_bus_master(c->pci))
                    iommu_unmap_all(c->pci);
                else
                    uart_print("[KIRA AHCI] bus mastering did not stop; mappings remain pinned.\\n");
                c->dma_failed = 1;
            }
            uart_print("[KIRA AHCI] data DMA mapping failed; port skipped.\n");
            return;
        }
    }
#endif
    w32(pr, P_CLB, (uint32_t)p->mem_phys); w32(pr, P_CLB + 4, (uint32_t)(p->mem_phys >> 32));
    uint64_t fb = p->mem_phys + 0x400;
    w32(pr, P_FB, (uint32_t)fb); w32(pr, P_FB + 4, (uint32_t)(fb >> 32));
    uint32_t cmd = r32(pr, P_CMD);
    if (cmd & CMD_CPD) cmd |= CMD_POD;
    if (c->cap & (1u << 27)) cmd |= CMD_SUD;                            /* раскрутка (Staggered Spin-up) */
    w32(pr, P_CMD, cmd);
    w32(pr, P_SERR, 0xFFFFFFFFu);
    w32(pr, P_IE, c->irq ? 0x7800000Fu : 0);                          /* D2H, PIO, DMA, SDB и ошибки - или опрос */
    if ((r32(pr, P_SSTS) & 0xF) != 3 && wait_reg(pr, P_SSTS, 0xF, 3, 50) && comreset(p)) {
        uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": no link\n"); return;
    }
    w32(pr, P_SERR, 0xFFFFFFFFu);
    w32(pr, P_IS, 0xFFFFFFFFu);
    pci_enable(c->pci, PCI_CMD_MASTER);
    if (port_start(p)) { uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": device stays busy\n"); return; }
    uint32_t sig = r32(pr, P_SIG);
    if (sig == 0xEB140101u) { uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": optical drive (ATAPI) - not used yet\n"); port_stop(p); return; }
    if (sig == 0x96690101u) { uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": port multiplier - not supported yet\n"); port_stop(p); return; }
    if (ata_cmd(p, 0xEC, 0, 0, 512, 0, 5000)) { uart_print("[KIRA AHCI] port "); uart_print_dec((uint64_t)pn); uart_print(": IDENTIFY failed\n"); port_stop(p); return; }
    const uint16_t* id = (const uint16_t*)p->buf;
    uint64_t sectors;
    if (id[83] & (1u << 10)) { p->lba48 = 1; sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48; }
    else sectors = (uint64_t)id[60] | (uint64_t)id[61] << 16;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {      /* логический сектор не 512 байт */
        uint32_t words = (uint32_t)id[117] | (uint32_t)id[118] << 16;
        if (words != 256) { uart_print("[KIRA AHCI] "); uart_print(p->dev.name); uart_print(": logical sector is not 512 bytes - not supported\n"); port_stop(p); return; }
    }
    if (!sectors) { port_stop(p); return; }
    char model[41];
    model_string(id, model, sizeof model);
    p->dev.sectors = sectors;
    p->dev.read = ahci_read; p->dev.write = ahci_write; p->dev.flush = ahci_flush; p->dev.priv = p;
    nports++;
    uart_print("[KIRA AHCI] "); uart_print(p->dev.name); uart_print(" (port "); uart_print_dec((uint64_t)pn); uart_print("): ");
    uart_print(model); uart_print(", "); uart_print_dec(sectors / 2048); uart_print(" MB");
    uart_print(p->lba48 ? ", LBA48" : ", LBA28"); uart_print("\n");
    blk_register(&p->dev, p->dev.name, model);
}

/* Сообщение контроллера: флаги отметившихся портов снимаются здесь (сначала PxIS, потом IS - по AHCI 1.3, иначе
   контроллер повторяет сообщение без конца) и сохраняются для команды, которая проверит в них ошибки. */
static void ahci_irq(void* ctx) {
    ahci_ctl_t* c = (ahci_ctl_t*)ctx;
    uint32_t is = r32(c->abar, IS);
    for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; i++) {       /* и порт, который ещё опознаётся */
        ahci_port_t* p = &ports[i];
        if (p->c != c || !(is & (1u << p->port)) || !p->pr) continue;
        uint32_t pis = r32(p->pr, P_IS);
        w32(p->pr, P_IS, pis);
        __atomic_or_fetch(&p->is_seen, pis, __ATOMIC_RELEASE);
        irqwait_fire(&p->iw);
    }
    w32(c->abar, IS, is);
}

static void ctl_init(const pci_dev_t* pd, int ci) {
    if (pd->bar[5].io || !pd->bar[5].base || pd->bar[5].size < 0x180) { uart_print("[KIRA AHCI] controller without ABAR: skipped\n"); return; }
#if defined(__aarch64__) || defined(__x86_64__)
    int iommu_rc = iommu_isolate_device(pd);
    if (iommu_rc != IOMMU_OK && iommu_rc != IOMMU_E_NONE &&
        iommu_rc != IOMMU_E_NOCOVER) {
        uart_print("[KIRA AHCI] IOMMU isolation failed; controller disabled.\n");
        return;
    }
    if (!iommu_device_ready(pd)) {
        uart_print("[KIRA AHCI] requester is not isolated or safely unrestricted; controller disabled.\n");
        return;
    }
#endif

    pci_enable(pd, PCI_CMD_MEM | PCI_CMD_INTX_OFF);
    uint64_t sz = pd->bar[5].size > 0x1100 ? 0x1100 : pd->bar[5].size;
    volatile uint8_t* ab = (volatile uint8_t*)pci_map_bar(pd, 5, 0, sz);
    if (!ab) { uart_print("[KIRA AHCI] cannot map ABAR\n"); return; }
    ahci_ctl_t* c = &ctls[ci];
    c->pci = pd;
    c->abar = ab;
    c->dma_failed = 0;
    w32(ab, GHC, r32(ab, GHC) | GHC_AE);
    if (r32(ab, CAP2) & 1u) {                                          /* BIOS/OS handoff */
        w32(ab, BOHC, r32(ab, BOHC) | 2u);
        wait_reg(ab, BOHC, 1u, 0, 25);
        blk_udelay(25000);
        if (r32(ab, BOHC) & (1u << 4)) wait_reg(ab, BOHC, 1u << 4, 0, 2000);
    }
    w32(ab, GHC, r32(ab, GHC) | GHC_HR);                               /* сброс контроллера */
    if (wait_reg(ab, GHC, GHC_HR, 0, 1000)) { uart_print("[KIRA AHCI] controller reset timeout\n"); return; }
    {   /* прерывание сообщением MSI: поток спит до конца команды */
        uint64_t ma; uint32_t md;
        if (msi_alloc(pd, ahci_irq, c, &ma, &md) == 0 && pci_msi_setup(pd, ma, md) == 0) c->irq = 1;
    }
    w32(ab, IS, 0xFFFFFFFFu);
    w32(ab, GHC, c->irq ? GHC_AE | GHC_IE : GHC_AE);
    c->cap = r32(ab, CAP);
    c->s64 = (c->cap >> 31) & 1;
    uint32_t pi = r32(ab, PI), np = (c->cap & 31) + 1;
    uint32_t vs = r32(ab, VS);
    uart_print("[KIRA AHCI] controller "); uart_print_hex(pd->vendor); uart_print(":"); uart_print_hex(pd->device);
    uart_print(", AHCI "); uart_print_dec(vs >> 16); uart_print("."); uart_print_dec((vs >> 8) & 0xFF);
    uart_print(", ports "); uart_print_dec(np); uart_print(c->s64 ? ", 64-bit DMA" : ", 32-bit DMA");
    uart_print(c->irq ? ", completion by MSI interrupt\n" : ", polled\n");
    blk_udelay(10000);                                                 /* линии успевают подняться после сброса */
    if ((uint64_t)0x100 + 0x80 * MAXPORT > sz) pi &= (uint32_t)((1ull << ((sz - 0x100) / 0x80)) - 1);
    for (int pn = 0; pn < MAXPORT; pn++) {
        if (!(pi & (1u << pn))) continue;
        port_init(c, pn, ci);
        if (c->dma_failed) break;
    }
    c->used = 1;
}

void ahci_probe(void) {
    int nc = 0;
    for (int i = 0; i < pci_count() && nc < MAXCTL; i++) {
        const pci_dev_t* p = pci_get(i);
        if (p->class_code == 0x01 && p->subclass == 0x06 && p->prog_if == 0x01) ctl_init(p, nc++);
    }
}
