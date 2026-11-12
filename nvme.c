#include "cpu.h"
#include "blk.h"
#include "pci.h"
#include "iommu.h"
#include "uart.h"
#include "kmutex.h"
#include "msi.h"

/* NVMe: регистры в памяти (BAR0), очереди команд и завершений в памяти, «дверные звонки» по смещению 0x1000 с шагом
   из CAP.DSTRD. Порядок по спецификации NVMe 1.4: выключить контроллер (CC.EN = 0, ждать CSTS.RDY = 0), очереди
   администратора (AQA/ASQ/ACQ), включить (страница 4 КБ, записи 64/16 байт), Identify контроллера, список активных
   пространств имён, Identify каждого, Set Features (число очередей), Create I/O CQ, Create I/O SQ.
   Одна очередь ввода-вывода на контроллер, одна команда за раз, опрос по фазовому биту без прерываний.
   Пространство имён с сектором 4 КБ работает так же: запись неполного сектора - чтение, изменение, запись.
   Данные - через буфер DMA контроллера (64 КБ) со списком PRP. */

#define MAXCTL 16
#define MAXNS  32
#define QN     32                  /* записей в очередях */
#define BOUNCE (64 * 1024)
#define PAGE   4096

#define R_CAP  0x00
#define R_VS   0x08
#define R_CC   0x14
#define R_CSTS 0x1C
#define R_AQA  0x24
#define R_ASQ  0x28
#define R_ACQ  0x30

typedef struct {
    int used, dead;
    volatile uint8_t* regs;
    uint32_t dstrd;                 /* шаг звонков в байтах */
    uint32_t timeout_ms;
    uint32_t max_bytes;             /* MDTS */
    uint8_t* mem; uint64_t mem_phys;   /* asq | acq | iosq | iocq | идентификация */
    uint8_t* buf; uint64_t buf_phys;   /* данные */
    uint64_t* prp; uint64_t prp_phys;  /* список PRP буфера (страницы 1..15) */
    struct { uint8_t* sq; uint64_t sq_phys; uint8_t* cq; uint64_t cq_phys; uint16_t tail, head, phase, qid; } q[2];
    uint16_t cid;
    char model[41];
    int index;
    kmutex_t mx;                        /* пространства имён одного контроллера делят очередь и буфер */
    int irq;                            /* завершения приходят прерыванием MSI-X/MSI (вектор 0 обеих очередей) */
    irqwait_t iw;
    uint8_t keep[PAGE];                 /* хвостовой блок при записи неполных блоков 4 КБ */
} nvme_ctl_t;

typedef struct {
    blk_dev_t dev;
    nvme_ctl_t* c;
    uint32_t nsid;
    uint32_t shift;                  /* log2(размер блока / 512) */
} nvme_ns_t;

static nvme_ctl_t ctls[MAXCTL];
static nvme_ns_t nss[MAXCTL * MAXNS];
static int nns;

void* virtio_dma_alloc(uint32_t bytes, uint64_t* phys);

static inline uint32_t r32(volatile uint8_t* b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static inline void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }
static inline uint64_t r64(volatile uint8_t* b, uint32_t o) { return (uint64_t)r32(b, o) | (uint64_t)r32(b, o + 4) << 32; }
static inline void w64(volatile uint8_t* b, uint32_t o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }
#define BARRIER() cpu_mb()            /* DMA и регистры устройства: на ARM нужен настоящий барьер */

static int wait_rdy(nvme_ctl_t* c, uint32_t want) {
    blk_wait_t w; blk_wait_begin(&w, c->timeout_ms);
    do {
        uint32_t s = r32(c->regs, R_CSTS);
        if (s == 0xFFFFFFFFu) return -1;                                /* устройство пропало с шины */
        if ((s & 1u) == want) return 0;
        if (want && (s & 2u)) return -1;                                /* CFS: фатальная ошибка контроллера */
    } while (blk_wait_step(&w));
    return -1;
}

/* Отправить команду в очередь qi (0 - администратора) и дождаться завершения. Код состояния (0 - успех) или -1. */
static int submit(nvme_ctl_t* c, int qi, uint32_t* cmd, uint32_t* result) {
    if (c->dead) return -1;
    uint16_t cid = ++c->cid;
    cmd[0] = (cmd[0] & 0xFFFFu) | (uint32_t)cid << 16;
    volatile uint32_t* e = (volatile uint32_t*)(c->q[qi].sq + (uint32_t)c->q[qi].tail * 64);
    for (int i = 0; i < 16; i++) e[i] = cmd[i];
    BARRIER();
    c->q[qi].tail = (uint16_t)((c->q[qi].tail + 1) % QN);
    uint16_t qid = c->q[qi].qid;
    blk_wait_t w; blk_wait_begin(&w, c->timeout_ms);
    if (c->irq) blk_wait_irq(&w, &c->iw);                            /* до звонка: ответ не проскочит */
    w32(c->regs, 0x1000 + (2u * qid) * c->dstrd, c->q[qi].tail);
    do {
        volatile uint32_t* ce = (volatile uint32_t*)(c->q[qi].cq + (uint32_t)c->q[qi].head * 16);
        BARRIER();
        uint32_t dw3 = ce[3];
        if (((dw3 >> 16) & 1u) == c->q[qi].phase) {
            uint32_t res = ce[0];
            c->q[qi].head = (uint16_t)(c->q[qi].head + 1);
            if (c->q[qi].head == QN) { c->q[qi].head = 0; c->q[qi].phase ^= 1; }
            w32(c->regs, 0x1000 + (2u * qid + 1) * c->dstrd, c->q[qi].head);
            if ((dw3 & 0xFFFFu) != cid) continue;                       /* чужое (не бывает при одной команде) */
            if (result) *result = res;
            return (int)((dw3 >> 17) & 0x7FFF);
        }
        if (r32(c->regs, R_CSTS) & 2u) break;
    } while (blk_wait_step(&w));
    c->dead = 1;
    uart_print("[KIRA NVMe] controller does not answer (CSTS "); uart_print_hex(r32(c->regs, R_CSTS)); uart_print("): disabled\n");
    return -1;
}

static void prp_fill(nvme_ctl_t* c, uint32_t* cmd, uint32_t bytes) {
    cmd[6] = (uint32_t)c->buf_phys; cmd[7] = (uint32_t)(c->buf_phys >> 32);
    uint64_t p2 = 0;
    if (bytes > 2 * PAGE) p2 = c->prp_phys;
    else if (bytes > PAGE) p2 = c->buf_phys + PAGE;
    cmd[8] = (uint32_t)p2; cmd[9] = (uint32_t)(p2 >> 32);
}

static int admin_identify(nvme_ctl_t* c, uint32_t cns, uint32_t nsid) {
    uint32_t cmd[16] = { 0x06, nsid };
    prp_fill(c, cmd, PAGE);
    cmd[10] = cns;
    return submit(c, 0, cmd, 0);
}

/* Блоки устройства [blba, blba+nb) <-> буфер контроллера. */
static int io(nvme_ns_t* n, int write, uint64_t blba, uint32_t nb) {
    uint32_t cmd[16] = { write ? 0x01u : 0x02u, n->nsid };
    prp_fill(n->c, cmd, nb << (n->shift + 9));
    cmd[10] = (uint32_t)blba; cmd[11] = (uint32_t)(blba >> 32);
    cmd[12] = nb - 1;
    for (int attempt = 0; attempt < 3; attempt++) {
        int st = submit(n->c, 1, cmd, 0);
        if (st == 0) return 0;
        if (st < 0) return -1;
        uart_print("[KIRA NVMe] "); uart_print(n->dev.name); uart_print(write ? ": write" : ": read");
        uart_print(" error, status "); uart_print_hex((uint64_t)st); uart_print("\n");
        if (st & 0x4000) return -1;                                     /* DNR: повтор не поможет */
    }
    return -1;
}

static uint32_t chunk_blocks(nvme_ns_t* n) {
    uint32_t bytes = BOUNCE < n->c->max_bytes ? BOUNCE : n->c->max_bytes;
    return bytes >> (n->shift + 9);
}

static void mcpy(uint8_t* d, const uint8_t* s, uint32_t n) { while (n--) *d++ = *s++; }

static int do_read(nvme_ns_t* n, uint64_t lba, uint32_t count, uint8_t* out) {
    uint32_t per = 1u << n->shift, cb = chunk_blocks(n);
    while (count) {
        uint64_t b = lba >> n->shift; uint32_t off = (uint32_t)(lba & (per - 1));
        uint32_t secs = cb * per - off; if (secs > count) secs = count;
        uint32_t nb = (off + secs + per - 1) >> n->shift;
        if (io(n, 0, b, nb)) return -1;
        mcpy(out, n->c->buf + off * BLK_SECTOR, secs * BLK_SECTOR);
        out += secs * BLK_SECTOR; lba += secs; count -= secs;
    }
    return 0;
}

static int do_write(nvme_ns_t* n, uint64_t lba, uint32_t count, const uint8_t* in) {
    nvme_ctl_t* c = n->c;
    uint32_t per = 1u << n->shift, bsz = per * BLK_SECTOR, cb = chunk_blocks(n);
    while (count) {
        uint64_t b = lba >> n->shift; uint32_t off = (uint32_t)(lba & (per - 1));
        uint32_t secs = cb * per - off; if (secs > count) secs = count;
        uint32_t nb = (off + secs + per - 1) >> n->shift, last = nb - 1;
        int head = off != 0, tail = ((off + secs) & (per - 1)) != 0;
        int tail_own = tail && !(head && last == 0);                    /* хвост в другом блоке, чем голова */
        if (tail_own) { if (io(n, 0, b + last, 1)) return -1; mcpy(c->keep, c->buf, bsz); }
        if (head && io(n, 0, b, 1)) return -1;                          /* неполный блок: прочитать, изменить, записать */
        if (tail_own) mcpy(c->buf + last * bsz, c->keep, bsz);
        mcpy(c->buf + off * BLK_SECTOR, in, secs * BLK_SECTOR);
        if (io(n, 1, b, nb)) return -1;
        in += secs * BLK_SECTOR; lba += secs; count -= secs;
    }
    return 0;
}

static int nvme_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) {
    nvme_ns_t* n = (nvme_ns_t*)d->priv;
    kmutex_lock(&n->c->mx); int r = do_read(n, lba, count, (uint8_t*)buf); kmutex_unlock(&n->c->mx);
    return r;
}
static int nvme_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) {
    nvme_ns_t* n = (nvme_ns_t*)d->priv;
    kmutex_lock(&n->c->mx); int r = do_write(n, lba, count, (const uint8_t*)buf); kmutex_unlock(&n->c->mx);
    return r;
}
static int nvme_flush(blk_dev_t* d) {
    nvme_ns_t* n = (nvme_ns_t*)d->priv;
    uint32_t cmd[16] = { 0x00, n->nsid };
    kmutex_lock(&n->c->mx); int r = submit(n->c, 1, cmd, 0) == 0 ? 0 : -1; kmutex_unlock(&n->c->mx);
    return r;
}

static void str_trim(char* out, const uint8_t* s, int len) {
    int k = 0;
    for (int i = 0; i < len; i++) out[k++] = (char)(s[i] >= 0x20 && s[i] < 0x7F ? s[i] : ' ');
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void add_ns(nvme_ctl_t* c, uint32_t nsid) {
    if (nns >= (int)(sizeof nss / sizeof nss[0])) return;
    if (admin_identify(c, 0, nsid) != 0) return;
    const uint8_t* id = c->buf;
    uint64_t nsze = *(const uint64_t*)id;
    uint8_t fl = id[26] & 15;
    uint32_t lbaf = *(const uint32_t*)(id + 128 + 4 * fl);
    uint32_t lbads = (lbaf >> 16) & 0xFF, ms = lbaf & 0xFFFF;
    if (!nsze) return;
    if (lbads < 9 || lbads > 12 || ms) {
        uart_print("[KIRA NVMe] namespace "); uart_print_dec(nsid); uart_print(": block 2^"); uart_print_dec(lbads);
        uart_print(ms ? " with metadata" : ""); uart_print(" - not supported\n");
        return;
    }
    nvme_ns_t* n = &nss[nns++];
    *n = (nvme_ns_t){ 0 };
    n->c = c; n->nsid = nsid; n->shift = lbads - 9;
    char nm[16] = "nvme"; int k = 4;
    n->dev.name[0] = 0;
    nm[k++] = (char)('0' + c->index); nm[k++] = 'n';
    if (nsid >= 10) nm[k++] = (char)('0' + (nsid / 10) % 10);
    nm[k++] = (char)('0' + nsid % 10); nm[k] = 0;
    for (int i = 0; i <= k; i++) n->dev.name[i] = nm[i];
    n->dev.sectors = nsze << n->shift;
    n->dev.read = nvme_read; n->dev.write = nvme_write; n->dev.flush = nvme_flush; n->dev.priv = n;
    uart_print("[KIRA NVMe] "); uart_print(nm); uart_print(": "); uart_print_dec(n->dev.sectors / 2048); uart_print(" MB, block ");
    uart_print_dec(1u << lbads); uart_print(" B\n");
    blk_register(&n->dev, nm, c->model);
}

static void nvme_irq(void* ctx) { irqwait_fire(&((nvme_ctl_t*)ctx)->iw); }

static void ctl_init(const pci_dev_t* pd, int ci) {
    if (pd->bar[0].io || !pd->bar[0].base || pd->bar[0].size < 0x2000) { uart_print("[KIRA NVMe] controller without BAR0: skipped\n"); return; }
#if defined(__aarch64__) || defined(__x86_64__)
    int iommu_rc = iommu_isolate_device(pd);
    if (iommu_rc != IOMMU_OK && iommu_rc != IOMMU_E_NONE &&
        iommu_rc != IOMMU_E_NOCOVER) {
        uart_print("[KIRA NVMe] IOMMU isolation failed; controller disabled.\n");
        return;
    }
    if (!iommu_device_ready(pd)) {
        uart_print("[KIRA NVMe] requester is not isolated or safely unrestricted; controller disabled.\n");
        return;
    }
#endif

    pci_enable(pd, PCI_CMD_MEM | PCI_CMD_INTX_OFF);
    nvme_ctl_t* c = &ctls[ci];
    for (unsigned i = 0; i < sizeof *c; i++) ((uint8_t*)c)[i] = 0;
    c->mx = (kmutex_t)KMUTEX_INIT;
    c->index = ci;
    volatile uint8_t* rg = (volatile uint8_t*)pci_map_bar(pd, 0, 0, 0x1000);
    if (!rg) { uart_print("[KIRA NVMe] cannot map registers\n"); return; }
    uint64_t cap = r64(rg, R_CAP);
    c->dstrd = 4u << ((cap >> 32) & 15);
    uint64_t need = 0x1000 + 4ull * c->dstrd;
    if (need > pd->bar[0].size) { uart_print("[KIRA NVMe] doorbells outside BAR0\n"); return; }
    rg = (volatile uint8_t*)pci_map_bar(pd, 0, 0, need);
    if (!rg) return;
    c->regs = rg;
    {   /* завершения - прерыванием: MSI-X (запись 0), иначе MSI; без них - опрос */
        uint64_t ma; uint32_t md;
        if (msi_alloc(pd, nvme_irq, c, &ma, &md) == 0 && (pci_msix_setup(pd, 0, ma, md) == 0 || pci_msi_setup(pd, ma, md) == 0)) c->irq = 1;
    }
    c->timeout_ms = (uint32_t)((cap >> 24) & 0xFF) * 500; if (c->timeout_ms < 500) c->timeout_ms = 500;
    if (!((cap >> 37) & 1)) { uart_print("[KIRA NVMe] controller without NVM command set: skipped\n"); return; }
    if (((cap >> 48) & 15) != 0) { uart_print("[KIRA NVMe] controller does not support 4 KB pages: skipped\n"); return; }
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    if (mqes < QN) { uart_print("[KIRA NVMe] queues too small\n"); return; }

    c->mem = (uint8_t*)virtio_dma_alloc(5 * PAGE, &c->mem_phys);
    c->buf = (uint8_t*)virtio_dma_alloc(BOUNCE, &c->buf_phys);
    c->prp = (uint64_t*)virtio_dma_alloc(PAGE, &c->prp_phys);
    if (!c->mem || !c->buf || !c->prp) { uart_print("[KIRA NVMe] no DMA memory\n"); return; }
#if defined(__aarch64__) || defined(__x86_64__)
    if (iommu_device_isolated(pd) &&
        (iommu_map(pd, c->mem_phys, c->mem_phys, 5 * PAGE, IOMMU_R | IOMMU_W) != IOMMU_OK ||
         iommu_map(pd, c->buf_phys, c->buf_phys, BOUNCE, IOMMU_R | IOMMU_W) != IOMMU_OK ||
         iommu_map(pd, c->prp_phys, c->prp_phys, PAGE, IOMMU_R | IOMMU_W) != IOMMU_OK)) {
        uart_print("[KIRA NVMe] IOMMU DMA mapping failed; controller disabled.\n");
        iommu_unmap_all(pd);
        return;
    }
#endif
    for (uint32_t i = 1; i < BOUNCE / PAGE; i++) c->prp[i - 1] = c->buf_phys + (uint64_t)i * PAGE;
    for (int qi = 0; qi < 2; qi++) {
        c->q[qi].sq = c->mem + (uint32_t)qi * 2 * PAGE; c->q[qi].sq_phys = c->mem_phys + (uint64_t)qi * 2 * PAGE;
        c->q[qi].cq = c->q[qi].sq + PAGE; c->q[qi].cq_phys = c->q[qi].sq_phys + PAGE;
        c->q[qi].tail = c->q[qi].head = 0; c->q[qi].phase = 1; c->q[qi].qid = (uint16_t)qi;
    }

    w32(rg, R_CC, r32(rg, R_CC) & ~1u);                                 /* выключить */
    if (wait_rdy(c, 0)) { uart_print("[KIRA NVMe] controller does not stop\n"); return; }
    w32(rg, R_AQA, (uint32_t)(QN - 1) << 16 | (QN - 1));
    w64(rg, R_ASQ, c->q[0].sq_phys);
    w64(rg, R_ACQ, c->q[0].cq_phys);
    pci_enable(pd, PCI_CMD_MASTER); /* all admin queues and bounce buffers are ready */
    if (!(pci_read16(pd, 0x04) & PCI_CMD_MASTER)) {
        if (pci_disable_bus_master(pd))
            (void)iommu_unmap_all(pd);
        else
            uart_print("[KIRA NVMe] bus mastering did not stop; DMA mappings remain pinned.\\n");
        uart_print("[KIRA NVMe] PCI DMA authorization was not granted; controller disabled.\\n");
        return;
    }
    w32(rg, R_CC, (6u << 16) | (4u << 20) | 1u);                        /* IOSQES 64, IOCQES 16, страница 4 КБ, NVM, вкл. */
    if (wait_rdy(c, 1)) { uart_print("[KIRA NVMe] controller does not start (CSTS "); uart_print_hex(r32(rg, R_CSTS)); uart_print(")\n"); return; }

    if (admin_identify(c, 1, 0) != 0) { uart_print("[KIRA NVMe] Identify failed\n"); return; }
    str_trim(c->model, c->buf + 24, 40);
    uint8_t mdts = c->buf[77];
    c->max_bytes = mdts ? (mdts >= 5 ? BOUNCE : (uint32_t)PAGE << mdts) : BOUNCE;
    uint32_t nn = *(const uint32_t*)(c->buf + 516);
    uint32_t vs = r32(rg, R_VS);
    uart_print("[KIRA NVMe] "); uart_print(c->model); uart_print(", NVMe "); uart_print_dec(vs >> 16); uart_print(".");
    uart_print_dec((vs >> 8) & 0xFF); uart_print(", namespaces "); uart_print_dec(nn);
    uart_print(c->irq ? ", completion by interrupt\n" : ", polled\n");

    uint32_t cmd[16] = { 0x09 };                                        /* Set Features: одна пара очередей */
    cmd[10] = 0x07; cmd[11] = 0;
    submit(c, 0, cmd, 0);
    uint32_t cq[16] = { 0x05 };                                         /* Create I/O Completion Queue 1 */
    cq[6] = (uint32_t)c->q[1].cq_phys; cq[7] = (uint32_t)(c->q[1].cq_phys >> 32);
    cq[10] = (uint32_t)(QN - 1) << 16 | 1u;
    cq[11] = c->irq ? 3u : 1u;                                          /* непрерывная; IEN - прерывания (вектор 0) */
    if (submit(c, 0, cq, 0) != 0) { uart_print("[KIRA NVMe] Create I/O CQ failed\n"); return; }
    uint32_t sq[16] = { 0x01 };                                         /* Create I/O Submission Queue 1 -> CQ 1 */
    sq[6] = (uint32_t)c->q[1].sq_phys; sq[7] = (uint32_t)(c->q[1].sq_phys >> 32);
    sq[10] = (uint32_t)(QN - 1) << 16 | 1u; sq[11] = (1u << 16) | 1u;
    if (submit(c, 0, sq, 0) != 0) { uart_print("[KIRA NVMe] Create I/O SQ failed\n"); return; }
    c->q[1].qid = 1;
    c->used = 1;

    /* активные пространства имён (NVMe 1.1+); у старых - перебор 1..NN */
    static uint32_t list[1024];
    int got = 0;
    if (vs >= 0x10100 && admin_identify(c, 2, 0) == 0) {
        for (int i = 0; i < 1024; i++) list[i] = ((const uint32_t*)c->buf)[i];
        for (int i = 0; i < 1024 && list[i] && got < MAXNS; i++, got++) add_ns(c, list[i]);
    } else for (uint32_t ns = 1; ns <= nn && ns <= MAXNS; ns++) add_ns(c, ns);
}

void nvme_probe(void) {
    int nc = 0;
    for (int i = 0; i < pci_count() && nc < MAXCTL; i++) {
        const pci_dev_t* p = pci_get(i);
        if (p->class_code == 0x01 && p->subclass == 0x08 && p->prog_if == 0x02) ctl_init(p, nc++);
    }
}
