#include "cpu.h"
#include "usb.h"
#include "pci.h"
#include "iommu.h"

/* xHCI (USB 3.x): регистры в памяти (BAR0), команды и передачи - кольцами TRB в памяти, итоги - кольцом событий.
   Порядок запуска по спецификации: передача контроллера от BIOS (USB Legacy Support), сброс, DCBAA, кольцо команд,
   кольцо событий, пуск; у устройства - Enable Slot, Address Device, при необходимости Evaluate Context (размер
   пакета EP0), Configure Endpoint для прерывающих и массовых точек. Устройства за хабами адресуются строкой маршрута,
   LS/FS за хабом USB 2.0 - через его переводчик транзакций (TT); у хаба в контексте слота отмечается, что он хаб.
   Массовая передача - один Normal TRB до 16 КБ (буфер выровнен и не пересекает границу 64 КБ). STALL оставляет точку
   в Halted до bulk_reset (Reset Endpoint + переконфигурация: последовательность пакетов с нуля, как у устройства после
   CLEAR_FEATURE). Прерывания не включаются: кольцо событий читается опросом. */

#define TRB_NORMAL     1
#define TRB_SETUP      2
#define TRB_DATA       3
#define TRB_STATUS     4
#define TRB_LINK       6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_DEQ      16
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD       33
#define TRB_EV_PORT      34

#define CC_SUCCESS 1
#define CC_SHORT   13
#define CC_STALL   6

#define RING_N 64             /* TRB в кольце (последний - ссылка на начало) */
#define EV_N   128
#define NPIPE  128
#define MAXSLOT 255
#define BULK_MAX 16384

typedef struct { volatile uint32_t p0, p1, st, ctl; } trb_t;

typedef struct {
    trb_t* t; uint64_t phys;
    unsigned enq; uint32_t cycle;
} ring_t;

struct usb_pipe {
    int used;
    usb_dev_t* dev;
    uint8_t dci, type, iv, bulk;      /* type: тип точки в контексте (3 Bulk IN... 7 Interrupt IN) */
    ring_t ring;
    uint8_t* buf; uint64_t buf_phys;  /* 1 КБ - для прерывающих */
    uint8_t* big; uint64_t big_phys;  /* 16 КБ - для массовых */
    uint16_t maxp;
    volatile int done; volatile uint32_t cc, residual;
    uint64_t pending;            /* адрес TRB в работе */
};

typedef struct {
    volatile uint8_t* cap; volatile uint8_t* op; volatile uint8_t* rt; volatile uint32_t* db;
    uint32_t nports, maxslots, csz;
    int ac64;
    const pci_dev_t* pci;
    int dma_failed;
    volatile uint64_t* dcbaa; uint64_t dcbaa_phys;
    ring_t cmd;
    trb_t* ev; uint64_t ev_phys; unsigned ev_deq; uint32_t ev_cycle;
    volatile uint64_t* erst; uint64_t erst_phys;
    /* итог последней команды */
    volatile int cmd_done; volatile uint32_t cmd_cc, cmd_slot; uint64_t cmd_trb;
    /* устройства по слотам */
    struct { ring_t ep0; uint8_t* out; uint64_t out_phys; uint8_t* in; uint64_t in_phys; uint8_t* buf; uint64_t buf_phys;
             volatile int done; volatile uint32_t cc, residual; uint64_t wait_trb, data_trb; volatile uint32_t data_residual; volatile int data_short; } slot[MAXSLOT + 1];
    struct usb_pipe pipes[NPIPE];
    uint8_t port_seen[256];
} xhci_t;

/* Контроллеров xHCI бывает несколько (у ПК часто 2-4: процессор, чипсет, отдельные платы). Состояние у каждого своё;
   работает с ними только поток usb (один исполнитель), поэтому «текущий» контроллер - просто указатель, который
   каждая операция контроллера ставит на свой hc->priv. */
#define XHCI_MAX 16
static xhci_t* xall[XHCI_MAX];          /* выделяются по числу найденных контроллеров (каждый - сотни КБ таблиц) */
static int started;
static xhci_t* X;
#define xs (*X)

static void* xhci_dma_alloc(uint32_t bytes, uint64_t* phys, int below4g) {
    if (!phys || !bytes || xs.dma_failed)
        return 0;
    void* memory = usb_dma_alloc(bytes, phys, below4g);
    if (!memory)
        return 0;
#if defined(__aarch64__)
    if (xs.pci && iommu_device_isolated(xs.pci) &&
        iommu_map(xs.pci, *phys, *phys, bytes, IOMMU_R | IOMMU_W) != IOMMU_OK) {
        /* Keep backing pages pinned; disable DMA before invalidating old maps. */
        pci_disable_bus_master(xs.pci);
        iommu_unmap_all(xs.pci);
        xs.dma_failed = 1;
        usb_log("[KIRA USB] xHCI: IOMMU DMA map failed; controller quarantined\n");
        return 0;
    }
#endif
    return memory;
}

#define BARRIER() cpu_mb()            /* DMA и регистры устройства: на ARM нужен настоящий барьер */
static inline uint32_t r32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void w32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline void w64(volatile uint8_t* b, uint32_t off, uint64_t v) { w32(b, off, (uint32_t)v); w32(b, off + 4, (uint32_t)(v >> 32)); }

/* ---------------- кольца ---------------- */

static int ring_init(ring_t* r) {
    r->t = (trb_t*)xhci_dma_alloc(RING_N * 16, &r->phys, !xs.ac64);
    if (!r->t) return -1;
    r->enq = 0; r->cycle = 1;
    trb_t* l = &r->t[RING_N - 1];
    l->p0 = (uint32_t)r->phys; l->p1 = (uint32_t)(r->phys >> 32); l->st = 0;
    l->ctl = (TRB_LINK << 10) | 2u;                       /* Toggle Cycle; бит цикла ставится при проходе */
    return 0;
}

/* Кладёт TRB, возвращает его физический адрес. Бит цикла пишется последним: до этого контроллер TRB не видит. */
static uint64_t ring_put(ring_t* r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl) {
    trb_t* t = &r->t[r->enq];
    uint64_t at = r->phys + (uint64_t)r->enq * 16;
    t->p0 = p0; t->p1 = p1; t->st = st;
    BARRIER();
    t->ctl = (ctl & ~1u) | r->cycle;
    BARRIER();
    if (++r->enq == RING_N - 1) {                          /* дошли до ссылки: передать её контроллеру и сменить цикл */
        trb_t* l = &r->t[RING_N - 1];
        l->ctl = (l->ctl & ~1u) | r->cycle;
        BARRIER();
        r->enq = 0; r->cycle ^= 1;
    }
    return at;
}

/* ---------------- события ---------------- */

static void process_events(void) {
    for (int guard = 0; guard < EV_N * 2; guard++) {
        trb_t* e = &xs.ev[xs.ev_deq];
        BARRIER();
        uint32_t ctl = e->ctl;
        if ((ctl & 1u) != xs.ev_cycle) break;
        uint32_t type = (ctl >> 10) & 63, cc = e->st >> 24;
        uint64_t ptr = (uint64_t)e->p0 | (uint64_t)e->p1 << 32;
        if (type == TRB_EV_CMD) {
            if (ptr == xs.cmd_trb) { xs.cmd_cc = cc; xs.cmd_slot = ctl >> 24; xs.cmd_done = 1; }
        } else if (type == TRB_EV_TRANSFER) {
            uint32_t slot = ctl >> 24, ep = (ctl >> 16) & 31, residual = e->st & 0xFFFFFF;
            if (slot >= 1 && slot <= MAXSLOT) {
                if (ep == 1) {
                    if (ptr == xs.slot[slot].data_trb) { xs.slot[slot].data_residual = residual; xs.slot[slot].data_short = cc == CC_SHORT; if (cc != CC_SUCCESS && cc != CC_SHORT) { xs.slot[slot].cc = cc; xs.slot[slot].done = 1; } }
                    if (ptr == xs.slot[slot].wait_trb) { xs.slot[slot].cc = cc; xs.slot[slot].done = 1; }
                    else if (cc != CC_SUCCESS && cc != CC_SHORT) { xs.slot[slot].cc = cc; xs.slot[slot].done = 1; }
                } else {
                    for (int i = 0; i < NPIPE; i++) {
                        struct usb_pipe* p = &xs.pipes[i];
                        if (p->used && p->dev->hc_slot == slot && p->dci == ep && ptr == p->pending) { p->cc = cc; p->residual = residual; p->done = 1; }
                    }
                }
            }
        }
        if (++xs.ev_deq == EV_N) { xs.ev_deq = 0; xs.ev_cycle ^= 1; }
        w64(xs.rt, 0x20 + 0x18, (xs.ev_phys + (uint64_t)xs.ev_deq * 16) | 8u);   /* ERDP, EHB сбросить */
    }
}

static int command(uint32_t p0, uint32_t p1, uint32_t ctl, uint32_t* slot_out) {
    xs.cmd_done = 0;
    xs.cmd_trb = ring_put(&xs.cmd, p0, p1, 0, ctl);
    xs.db[0] = 0;
    uint32_t deadline = usb_now_ms() + 500;
    while (!xs.cmd_done) {
        process_events();
        if (xs.cmd_done) break;
        if ((int32_t)(usb_now_ms() - deadline) > 0) { usb_log("[KIRA USB] xHCI command timeout\n"); return -1; }
        usb_sleep_ms(1);
    }
    if (slot_out) *slot_out = xs.cmd_slot;
    return xs.cmd_cc == CC_SUCCESS ? 0 : -(int)xs.cmd_cc;
}

/* ---------------- контексты ---------------- */

static volatile uint32_t* ctx(uint8_t* base, unsigned idx) { return (volatile uint32_t*)(base + idx * xs.csz); }
/* Входной контекст: [0] - управляющий, [1] - слот, [1 + DCI] - конечная точка. */
static volatile uint32_t* in_ctl(uint32_t s) { return ctx(xs.slot[s].in, 0); }
static volatile uint32_t* in_slot(uint32_t s) { return ctx(xs.slot[s].in, 1); }
static volatile uint32_t* in_ep(uint32_t s, unsigned dci) { return ctx(xs.slot[s].in, 1 + dci); }

static void clear_input(uint32_t s) { for (unsigned i = 0; i < 33 * xs.csz; i++) xs.slot[s].in[i] = 0; }

static uint32_t speed_code(uint8_t sp) { return sp == USB_SPEED_FULL ? 1 : sp == USB_SPEED_LOW ? 2 : sp == USB_SPEED_HIGH ? 3 : 4; }

/* ---------------- управляющие передачи ---------------- */

static int xhci_control(usb_hc_t* hc, usb_dev_t* d, const usb_setup_t* s, void* data) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    uint32_t sl = d->hc_slot;
    if (!sl || sl > MAXSLOT) return -1;
    unsigned len = s->wLength;
    if (len > 1024) return -1;
    int in = (s->bmRequestType & 0x80) != 0;
    const uint8_t* sp = (const uint8_t*)s;
    uint32_t lo = (uint32_t)sp[0] | (uint32_t)sp[1] << 8 | (uint32_t)sp[2] << 16 | (uint32_t)sp[3] << 24;
    uint32_t hi = (uint32_t)sp[4] | (uint32_t)sp[5] << 8 | (uint32_t)sp[6] << 16 | (uint32_t)sp[7] << 24;
    if (!in && len) for (unsigned i = 0; i < len; i++) xs.slot[sl].buf[i] = ((const uint8_t*)data)[i];
    ring_t* r = &xs.slot[sl].ep0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    xs.slot[sl].done = 0; xs.slot[sl].data_trb = ~0ull; xs.slot[sl].data_residual = 0; xs.slot[sl].data_short = 0;
    ring_put(r, lo, hi, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));          /* IDT: SETUP прямо в TRB */
    if (len) xs.slot[sl].data_trb = ring_put(r, (uint32_t)xs.slot[sl].buf_phys, (uint32_t)(xs.slot[sl].buf_phys >> 32), len,
                                             (TRB_DATA << 10) | (1u << 2) | (in ? 1u << 16 : 0));          /* ISP: короткий - событие */
    xs.slot[sl].wait_trb = ring_put(r, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((in && len) ? 0 : 1u << 16));
    xs.db[sl] = 1;
    uint32_t deadline = usb_now_ms() + 1000;
    while (!xs.slot[sl].done) {
        process_events();
        if (xs.slot[sl].done) break;
        if ((int32_t)(usb_now_ms() - deadline) > 0) return -1;
        usb_sleep_ms(1);
    }
    if (xs.slot[sl].cc != CC_SUCCESS && xs.slot[sl].cc != CC_SHORT) {
        /* STALL и ошибки останавливают EP0 (Halted): сбросить точку и поставить чтение кольца на свободное место,
           иначе все следующие запросы к устройству молча ждали бы */
        command(0, 0, (TRB_RESET_EP << 10) | (1u << 16) | (sl << 24), 0);
        uint64_t deq = r->phys + (uint64_t)r->enq * 16;
        command((uint32_t)deq | r->cycle, (uint32_t)(deq >> 32), (TRB_SET_DEQ << 10) | (1u << 16) | (sl << 24), 0);
        return -1;
    }
    unsigned got = len;
    if (xs.slot[sl].data_short) got = xs.slot[sl].data_residual <= len ? len - xs.slot[sl].data_residual : 0;
    if (in) for (unsigned i = 0; i < got; i++) ((uint8_t*)data)[i] = xs.slot[sl].buf[i];
    return (int)got;
}

/* ---------------- конечные точки ---------------- */

static void ring_reset(ring_t* r) {
    for (unsigned i = 0; i < RING_N - 1; i++) r->t[i].ctl = 0;
    r->t[RING_N - 1].ctl = (TRB_LINK << 10) | 2u;
    r->enq = 0; r->cycle = 1;
}

static uint32_t ep_state(uint32_t sl, unsigned dci) { return ctx(xs.slot[sl].out, dci)[0] & 7; }

/* Входной контекст слота: как сейчас у контроллера, с нужным числом записей и признаками хаба. */
static void input_slot(uint32_t sl, usb_dev_t* d, unsigned min_entries) {
    volatile uint32_t* sc = ctx(xs.slot[sl].out, 0);
    for (unsigned i = 0; i < 4; i++) in_slot(sl)[i] = sc[i];
    uint32_t entries = (in_slot(sl)[0] >> 27) & 31;
    if (entries < min_entries) entries = min_entries;
    uint32_t dw0 = (in_slot(sl)[0] & ~(31u << 27)) | (entries << 27);
    if (d->is_hub) {
        dw0 |= 1u << 26;                                               /* Hub */
        if (d->hub_mtt) dw0 |= 1u << 25;
        in_slot(sl)[1] = (in_slot(sl)[1] & 0x00FFFFFFu) | ((uint32_t)d->hub_nports << 24);
        in_slot(sl)[2] = (in_slot(sl)[2] & ~(3u << 16)) | ((uint32_t)(d->hub_ttt & 3) << 16);
    }
    in_slot(sl)[0] = dw0;
    in_slot(sl)[3] = 0;                                                /* адрес и состояние слота задаёт контроллер */
}

/* Configure Endpoint: drop - снять точку, add - (пере)создать её на кольце pipe. */
static int ep_configure(struct usb_pipe* p, int drop, int add) {
    usb_dev_t* d = p->dev;
    uint32_t sl = d->hc_slot;
    if (!sl || sl > MAXSLOT) return -1;
    clear_input(sl);
    in_ctl(sl)[0] = drop ? 1u << p->dci : 0;
    in_ctl(sl)[1] = 1u | (add ? 1u << p->dci : 0);
    input_slot(sl, d, p->dci);
    if (add) {
        volatile uint32_t* e = in_ep(sl, p->dci);
        unsigned burst = 0;
        e[0] = (uint32_t)p->iv << 16;
        e[1] = (3u << 1) | ((uint32_t)p->type << 3) | (burst << 8) | ((uint32_t)p->maxp << 16);   /* CErr 3 */
        e[2] = (uint32_t)p->ring.phys | 1u; e[3] = (uint32_t)(p->ring.phys >> 32);
        e[4] = p->bulk ? 3072u : (uint32_t)p->maxp | ((uint32_t)p->maxp << 16);   /* средняя длина TRB; для прерывающих - и макс. нагрузка */
    }
    BARRIER();
    return command((uint32_t)xs.slot[sl].in_phys, (uint32_t)(xs.slot[sl].in_phys >> 32), (TRB_CONFIG_EP << 10) | (sl << 24), 0);
}

static struct usb_pipe* pipe_new(usb_dev_t* d, uint8_t ep, uint16_t maxp, int bulk) {
    if (!d->hc_slot || !maxp || maxp > 1024) return 0;
    struct usb_pipe* p = 0;
    for (int i = 0; i < NPIPE; i++) if (!xs.pipes[i].used) { p = &xs.pipes[i]; break; }
    if (!p) { usb_log("[KIRA USB] xHCI: too many open endpoints\n"); return 0; }
    if (!p->ring.t && ring_init(&p->ring) != 0) return 0;
    if (!bulk && !p->buf && !(p->buf = (uint8_t*)xhci_dma_alloc(1024, &p->buf_phys, !xs.ac64))) return 0;
    if (bulk && !p->big) {                                             /* 32 КБ, из них 16 КБ, выровненные на 16 КБ */
        uint64_t ph; uint8_t* m = (uint8_t*)xhci_dma_alloc(2 * BULK_MAX, &ph, !xs.ac64);
        if (!m) return 0;
        uint64_t skip = (BULK_MAX - (ph & (BULK_MAX - 1))) & (BULK_MAX - 1);
        p->big = m + skip; p->big_phys = ph + skip;
    }
    ring_reset(&p->ring);
    p->dev = d; p->maxp = maxp; p->bulk = (uint8_t)bulk;
    p->dci = (uint8_t)((ep & 15) * 2 + ((ep & 0x80) ? 1 : 0));
    p->done = 0; p->pending = ~0ull;
    return p;
}

static void pipe_arm(struct usb_pipe* p) {
    p->done = 0;
    p->pending = ring_put(&p->ring, (uint32_t)p->buf_phys, (uint32_t)(p->buf_phys >> 32), p->maxp,
                          (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));                 /* IOC, ISP */
    xs.db[p->dev->hc_slot] = p->dci;
}

static usb_pipe_t* xhci_intr_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp, uint8_t interval) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    if (!(ep & 0x80)) return 0;
    struct usb_pipe* p = pipe_new(d, ep, maxp, 0);
    if (!p) return 0;
    /* интервал в степенях двойки по 125 мкс: для LS/FS bInterval - в кадрах (1 мс), для HS/SS - уже степень */
    unsigned iv;
    if (d->speed == USB_SPEED_LOW || d->speed == USB_SPEED_FULL) { unsigned f = interval ? interval : 1, e = 3; while ((1u << (e + 1)) <= f * 8 && e < 10) e++; iv = e; }
    else iv = interval ? (unsigned)(interval - 1 > 15 ? 15 : interval - 1) : 3;
    p->type = 7; p->iv = (uint8_t)iv;                                  /* Interrupt IN */
    if (ep_configure(p, 0, 1) != 0) { usb_log("[KIRA USB] xHCI: Configure Endpoint failed\n"); return 0; }
    p->used = 1;
    pipe_arm(p);
    return p;
}

static int xhci_intr_poll(usb_hc_t* hc, usb_pipe_t* p, uint8_t* buf, unsigned cap) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    process_events();
    if (!p->done) return 0;
    if (p->cc != CC_SUCCESS && p->cc != CC_SHORT) return -1;
    unsigned n = p->residual <= p->maxp ? p->maxp - p->residual : 0;
    if (n > cap) n = cap;
    for (unsigned i = 0; i < n; i++) buf[i] = p->buf[i];
    pipe_arm(p);
    return (int)n;
}

static usb_pipe_t* xhci_bulk_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    if (d->speed == USB_SPEED_LOW) return 0;
    struct usb_pipe* p = pipe_new(d, ep, maxp, 1);
    if (!p) return 0;
    p->type = (ep & 0x80) ? 6 : 2; p->iv = 0;                          /* Bulk IN / Bulk OUT */
    if (ep_configure(p, 0, 1) != 0) { usb_log("[KIRA USB] xHCI: Configure Endpoint (bulk) failed\n"); return 0; }
    p->used = 1;
    return p;
}

/* Точку - в начальное состояние: остановить (или вывести из Halted), пересоздать на чистом кольце. */
static void ep_reinit(struct usb_pipe* p) {
    uint32_t sl = p->dev->hc_slot;
    if (!sl) return;
    uint32_t st = ep_state(sl, p->dci);
    if (st == 2) command(0, 0, (TRB_RESET_EP << 10) | ((uint32_t)p->dci << 16) | (sl << 24), 0);
    else if (st == 1) command(0, 0, (TRB_STOP_EP << 10) | ((uint32_t)p->dci << 16) | (sl << 24), 0);
    process_events();
    ring_reset(&p->ring);
    p->pending = ~0ull; p->done = 0;
    ep_configure(p, 1, 1);
}

static int xhci_bulk_xfer(usb_hc_t* hc, usb_pipe_t* p, void* buf, unsigned len) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    if (!p || !p->used || !p->bulk || len > BULK_MAX) return USB_ERR;
    int in = p->type == 6;
    if (!in) for (unsigned i = 0; i < len; i++) p->big[i] = ((const uint8_t*)buf)[i];
    p->done = 0;
    p->pending = ring_put(&p->ring, (uint32_t)p->big_phys, (uint32_t)(p->big_phys >> 32), len,
                          (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    xs.db[p->dev->hc_slot] = p->dci;
    uint32_t deadline = usb_now_ms() + 5000;
    while (!p->done) {
        process_events();
        if (p->done) break;
        if ((int32_t)(usb_now_ms() - deadline) > 0) {                  /* устройство молчит: снять передачу */
            ep_reinit(p);
            return USB_ERR;
        }
        usb_sleep_ms(1);
    }
    p->pending = ~0ull;
    if (p->cc == CC_STALL) return USB_ERR_STALL;                       /* точка в Halted до bulk_reset */
    if (p->cc != CC_SUCCESS && p->cc != CC_SHORT) { ep_reinit(p); return USB_ERR; }
    unsigned got = p->residual <= len ? len - p->residual : 0;
    if (in) for (unsigned i = 0; i < got; i++) ((uint8_t*)buf)[i] = p->big[i];
    return (int)got;
}

static void xhci_bulk_reset(usb_hc_t* hc, usb_pipe_t* p) { X = (xhci_t*)hc->priv; if (p && p->used) ep_reinit(p); }

static void xhci_pipe_close(usb_hc_t* hc, usb_pipe_t* p) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    if (!p || !p->used) return;
    p->used = 0;
    if (p->dev->hc_slot) ep_configure(p, 1, 0);                        /* снять точку: контроллер её больше не трогает */
    p->done = 0; p->pending = ~0ull;
}

static void xhci_release(usb_hc_t* hc, usb_dev_t* d) {
    X = (xhci_t*)hc->priv;
    (void)hc;
    for (int i = 0; i < NPIPE; i++) if (xs.pipes[i].used && xs.pipes[i].dev == d) { xs.pipes[i].used = 0; xs.pipes[i].pending = ~0ull; }
    if (d->hc_slot) command(0, 0, (TRB_DISABLE_SLOT << 10) | (d->hc_slot << 24), 0);
    if (d->hc_slot && d->hc_slot <= MAXSLOT) xs.dcbaa[d->hc_slot] = 0;
    d->hc_slot = 0;
}

/* ---------------- порты и адресация ---------------- */

static uint32_t portsc_off(uint32_t port) { return 0x400 + 0x10 * (port - 1); }
#define PS_CCS (1u << 0)
#define PS_PED (1u << 1)
#define PS_PR  (1u << 4)
#define PS_PP  (1u << 9)
#define PS_CHANGES (0x7Fu << 17)
#define PS_PRESERVE ((1u << 9) | (3u << 14) | (7u << 25))   /* PP, индикатор, разрешения пробуждения */

static int port_enable(uint32_t port) {
    uint32_t off = portsc_off(port);
    uint32_t v = r32(xs.op, off);
    if (v & PS_PED) return 0;                                  /* USB 3: порт включается сам после тренировки линии */
    w32(xs.op, off, (v & PS_PRESERVE) | PS_PR);               /* USB 2: сброс порта включает его */
    for (int i = 0; i < 50; i++) {
        usb_sleep_ms(5);
        v = r32(xs.op, off);
        if (!(v & PS_PR) && (v & PS_PED)) { w32(xs.op, off, (v & PS_PRESERVE) | PS_CHANGES); return 0; }
        if (!(v & PS_CCS)) return -1;
    }
    return -1;
}

/* Новое устройство (порт уже сброшен): слот, Address Device с маршрутом и переводчиком, размер пакета EP0. */
static usb_dev_t* xhci_enumerate(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed) {
    X = (xhci_t*)hc->priv;
    uint32_t sl = 0;
    if (command(0, 0, TRB_ENABLE_SLOT << 10, &sl) != 0 || !sl || sl > MAXSLOT) {
        usb_log("[KIRA USB] xHCI: no free slot\n");
        if (sl && sl <= 255) command(0, 0, (TRB_DISABLE_SLOT << 10) | (sl << 24), 0);
        return 0;
    }
    usb_dev_t* d = usb_dev_new(hc, parent, port, speed);
    if (!d) { usb_log("[KIRA USB] too many USB devices\n"); command(0, 0, (TRB_DISABLE_SLOT << 10) | (sl << 24), 0); return 0; }
    d->hc_slot = sl;
    if (!xs.slot[sl].in) {
        xs.slot[sl].in = (uint8_t*)xhci_dma_alloc(33 * 64, &xs.slot[sl].in_phys, !xs.ac64);
        xs.slot[sl].out = (uint8_t*)xhci_dma_alloc(32 * 64, &xs.slot[sl].out_phys, !xs.ac64);
        xs.slot[sl].buf = (uint8_t*)xhci_dma_alloc(1024, &xs.slot[sl].buf_phys, !xs.ac64);
        if (!xs.slot[sl].in || !xs.slot[sl].out || !xs.slot[sl].buf || ring_init(&xs.slot[sl].ep0) != 0) {
            usb_log("[KIRA USB] xHCI: no memory\n"); usb_dev_free(d); return 0;
        }
    } else {
        for (unsigned i = 0; i < 32 * xs.csz; i++) xs.slot[sl].out[i] = 0;
        ring_reset(&xs.slot[sl].ep0);
    }
    xs.dcbaa[sl] = xs.slot[sl].out_phys;
    clear_input(sl);
    in_ctl(sl)[1] = 3u;                                                 /* слот и EP0 */
    in_slot(sl)[0] = (d->route & 0xFFFFFu) | (speed_code(speed) << 20) | (1u << 27);
    in_slot(sl)[1] = (uint32_t)d->root_port << 16;
    if (d->tt_slot) in_slot(sl)[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    volatile uint32_t* e = in_ep(sl, 1);
    e[1] = (3u << 1) | (4u << 3) | ((uint32_t)d->ep0_maxp << 16);      /* CErr 3, Control */
    e[2] = (uint32_t)xs.slot[sl].ep0.phys | 1u; e[3] = (uint32_t)(xs.slot[sl].ep0.phys >> 32);
    e[4] = 8;
    BARRIER();
    if (command((uint32_t)xs.slot[sl].in_phys, (uint32_t)(xs.slot[sl].in_phys >> 32), (TRB_ADDRESS_DEV << 10) | (sl << 24), 0) != 0) {
        usb_log("[KIRA USB] xHCI: Address Device failed\n"); usb_dev_free(d); return 0;
    }
    d->addr = (uint8_t)(ctx(xs.slot[sl].out, 0)[3] & 0xFF);
    /* FS: размер пакета EP0 бывает 8..64 - узнаём из первых 8 байт дескриптора и сообщаем контроллеру */
    if (speed == USB_SPEED_FULL) {
        uint8_t dd[8];
        usb_setup_t s = { 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, 8 };
        if (xhci_control(hc, d, &s, dd) == 8 && (dd[7] == 16 || dd[7] == 32 || dd[7] == 64) && dd[7] != d->ep0_maxp) {
            d->ep0_maxp = dd[7];
            clear_input(sl);
            in_ctl(sl)[1] = 2u;
            e = in_ep(sl, 1);
            e[1] = (3u << 1) | (4u << 3) | ((uint32_t)d->ep0_maxp << 16);
            BARRIER();
            command((uint32_t)xs.slot[sl].in_phys, (uint32_t)(xs.slot[sl].in_phys >> 32), (TRB_EVAL_CTX << 10) | (sl << 24), 0);
        }
    }
    return d;
}

static void attach_port(usb_hc_t* hc, uint32_t port) {
    if (port_enable(port) != 0) { usb_log("[KIRA USB] xHCI port reset failed\n"); return; }
    usb_sleep_ms(10);
    uint32_t v = r32(xs.op, portsc_off(port));
    uint32_t spd = (v >> 10) & 15;
    uint8_t speed = spd == 1 ? USB_SPEED_FULL : spd == 2 ? USB_SPEED_LOW : spd == 3 ? USB_SPEED_HIGH : USB_SPEED_SUPER;
    usb_dev_t* d = xhci_enumerate(hc, 0, (uint8_t)port, speed);
    if (d) usb_attach(d);
}

static void xhci_ports(usb_hc_t* hc) {
    X = (xhci_t*)hc->priv;
    process_events();
    for (uint32_t port = 1; port <= xs.nports; port++) {
        uint32_t off = portsc_off(port), v = r32(xs.op, off);
        if (v & PS_CHANGES) w32(xs.op, off, (v & PS_PRESERVE) | (v & PS_CHANGES));   /* сбросить флаги изменений */
        int present = usb_dev_on_port(hc, (uint8_t)port) != 0;
        if ((v & PS_CCS) && !present && !xs.port_seen[port]) { xs.port_seen[port] = 1; attach_port(hc, port); }
        else if (!(v & PS_CCS)) { xs.port_seen[port] = 0; if (present) usb_detach_port(hc, (uint8_t)port); }
    }
}

/* ---------------- запуск ---------------- */

static void bios_handoff(volatile uint8_t* cap) {
    uint32_t xecp = (r32(cap, 0x10) >> 16) << 2;
    for (int guard = 0; xecp && guard < 64; guard++) {
        uint32_t v = r32(cap, xecp);
        if ((v & 0xFF) == 1) {                                         /* USB Legacy Support */
            if (v & (1u << 16)) {
                w32(cap, xecp, v | (1u << 24));                        /* ОС забирает контроллер */
                for (int i = 0; i < 100 && (r32(cap, xecp) & (1u << 16)); i++) usb_sleep_ms(10);
                if (r32(cap, xecp) & (1u << 16)) { usb_log("[KIRA USB] xHCI: BIOS keeps the controller, taking it anyway\n"); w32(cap, xecp, (r32(cap, xecp) & ~(1u << 16)) | (1u << 24)); }
            } else w32(cap, xecp, v | (1u << 24));
            w32(cap, xecp + 4, (r32(cap, xecp + 4) & 0x001F1FEEu) | 0xE0000000u);   /* SMI выключить, флаги сбросить */
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        xecp += next << 2;
    }
}

int xhci_start(usb_hc_t* hc, const void* pd) {
    const pci_dev_t* p = (const pci_dev_t*)pd;
    if (started >= XHCI_MAX) { usb_log("[KIRA USB] more than 16 xHCI controllers: the rest are not used\n"); return -1; }
    if (!xall[started]) xall[started] = (xhci_t*)usb_dma_alloc(sizeof(xhci_t), &(uint64_t){ 0 }, 0);   /* обнулённые страницы */
    if (!xall[started]) { usb_log("[KIRA USB] xHCI: no memory for controller state\n"); return -1; }
    X = xall[started];
    if (p->bar[0].io || !p->bar[0].base || p->bar[0].size < 0x1000) { usb_log("[KIRA USB] xHCI without memory BAR0: skipped\n"); return -1; }
#if defined(__aarch64__)
    int iommu_rc = iommu_isolate_device(p);
    if (iommu_rc != IOMMU_OK && iommu_rc != IOMMU_E_NOCOVER) {
        usb_log("[KIRA USB] xHCI: IOMMU isolation failed; controller disabled\n");
        return -1;
    }
    if (!iommu_device_ready(p)) {
        usb_log("[KIRA USB] xHCI: requester is not isolated or safely unrestricted; controller disabled\n");
        return -1;
    }
#endif
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_INTX_OFF);
    if (p->vendor == 0x8086) {                                        /* Intel: порты, отданные EHCI, вернуть xHCI */
        pci_write32(p, 0xD8, pci_read32(p, 0xDC));
        pci_write32(p, 0xD0, pci_read32(p, 0xD4));
    }
    uint64_t sz = p->bar[0].size > 0x10000 ? 0x10000 : p->bar[0].size;
    volatile uint8_t* cap = (volatile uint8_t*)pci_map_bar(p, 0, 0, sz);
    if (!cap) { usb_log("[KIRA USB] xHCI: cannot map registers\n"); return -1; }
    for (unsigned long i = 0; i < sizeof xs; i++) ((volatile uint8_t*)X)[i] = 0;   /* десятки КБ: не через временную копию на стеке */
    xs.pci = p;
    xs.dma_failed = 0;
    xs.cap = cap;
    uint32_t caplen = cap[0], hcs1 = r32(cap, 4), hcs2 = r32(cap, 8), hcc1 = r32(cap, 0x10);
    xs.op = cap + caplen; xs.rt = cap + (r32(cap, 0x18) & ~0x1Fu); xs.db = (volatile uint32_t*)(cap + (r32(cap, 0x14) & ~3u));
    xs.maxslots = hcs1 & 0xFF; if (xs.maxslots > MAXSLOT) xs.maxslots = MAXSLOT;
    xs.nports = (hcs1 >> 24) & 0xFF;
    xs.ac64 = hcc1 & 1; xs.csz = (hcc1 & 4) ? 64 : 32;
    if ((uint64_t)(xs.rt - cap) + 0x40 > sz || (uint64_t)((volatile uint8_t*)xs.db - cap) + 4 * (MAXSLOT + 1) > sz) { usb_log("[KIRA USB] xHCI: registers outside BAR\n"); return -1; }

    bios_handoff(cap);
    w32(xs.op, 0, r32(xs.op, 0) & ~1u);                               /* стоп */
    for (int i = 0; i < 50 && !(r32(xs.op, 4) & 1u); i++) usb_sleep_ms(1);
    w32(xs.op, 0, 2u);                                                 /* HCRST */
    for (int i = 0; i < 200 && ((r32(xs.op, 0) & 2u) || (r32(xs.op, 4) & (1u << 11))); i++) usb_sleep_ms(5);
    if ((r32(xs.op, 0) & 2u) || (r32(xs.op, 4) & (1u << 11))) { usb_log("[KIRA USB] xHCI reset timeout\n"); return -1; }

    xs.dcbaa = (volatile uint64_t*)xhci_dma_alloc(2048, &xs.dcbaa_phys, !xs.ac64);
    if (!xs.dcbaa || ring_init(&xs.cmd) != 0) { usb_log("[KIRA USB] xHCI: no memory\n"); return -1; }
    uint32_t nsp = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (nsp) {                                                         /* рабочая память контроллера */
        uint64_t arr_phys;
        volatile uint64_t* arr = (volatile uint64_t*)xhci_dma_alloc(nsp * 8, &arr_phys, !xs.ac64);
        if (!arr) return -1;
        for (uint32_t i = 0; i < nsp; i++) { uint64_t ph; if (!xhci_dma_alloc(4096, &ph, !xs.ac64)) return -1; arr[i] = ph; }
        xs.dcbaa[0] = arr_phys;
    }
    xs.ev = (trb_t*)xhci_dma_alloc(EV_N * 16, &xs.ev_phys, !xs.ac64);
    xs.erst = (volatile uint64_t*)xhci_dma_alloc(64, &xs.erst_phys, !xs.ac64);
    if (!xs.ev || !xs.erst) return -1;
    xs.erst[0] = xs.ev_phys; xs.erst[1] = EV_N;
    xs.ev_deq = 0; xs.ev_cycle = 1;

    w32(xs.op, 0x38, xs.maxslots);                                     /* CONFIG */
    w64(xs.op, 0x30, xs.dcbaa_phys);                                   /* DCBAAP */
    w64(xs.op, 0x18, xs.cmd.phys | 1u);                                /* CRCR, RCS = 1 */
    w32(xs.rt, 0x20 + 0x00, 0);                                        /* IMAN: без прерываний */
    w32(xs.rt, 0x20 + 0x08, 1);                                        /* ERSTSZ */
    w64(xs.rt, 0x20 + 0x18, xs.ev_phys);                               /* ERDP */
    w64(xs.rt, 0x20 + 0x10, xs.erst_phys);                             /* ERSTBA - последним */
#if defined(__aarch64__)
    if (xs.dma_failed || !iommu_device_ready(p)) {
        pci_disable_bus_master(p);
        iommu_unmap_all(p);
        usb_log("[KIRA USB] xHCI: DMA authorization incomplete; controller remains stopped\n");
        return -1;
    }
#else
    if (xs.dma_failed) return -1;
#endif
    pci_enable(p, PCI_CMD_MASTER);                                    /* every DMA region is authorized */
    w32(xs.op, 0, 1u);                                                 /* пуск */
    for (int i = 0; i < 50 && (r32(xs.op, 4) & 1u); i++) usb_sleep_ms(1);
    if (r32(xs.op, 4) & 1u) { usb_log("[KIRA USB] xHCI does not start\n"); return -1; }
    if (hcc1 & (1u << 3))                                              /* питание портов управляется: включить */
        for (uint32_t port = 1; port <= xs.nports; port++) { uint32_t v = r32(xs.op, portsc_off(port)); if (!(v & PS_PP)) w32(xs.op, portsc_off(port), (v & PS_PRESERVE) | PS_PP); }
    usb_sleep_ms(50);                                                  /* линии USB 3 успевают натренироваться */

    started++;
    hc->name = "xHCI"; hc->priv = X;
    hc->control = xhci_control; hc->enumerate = xhci_enumerate;
    hc->intr_open = xhci_intr_open; hc->intr_poll = xhci_intr_poll;
    hc->bulk_open = xhci_bulk_open; hc->bulk_xfer = xhci_bulk_xfer; hc->bulk_reset = xhci_bulk_reset;
    hc->pipe_close = xhci_pipe_close; hc->ports = xhci_ports; hc->release = xhci_release;
    usb_log("[KIRA USB] xHCI "); usb_log_hex(p->vendor); usb_log(":"); usb_log_hex(p->device);
    usb_log(", "); usb_log_dec(xs.nports); usb_log(" ports, "); usb_log_dec(xs.maxslots); usb_log(" slots, context ");
    usb_log_dec(xs.csz); usb_log(" B"); if (nsp) { usb_log(", scratchpad "); usb_log_dec(nsp); }
    usb_log(", polled event ring\n");
    return 0;
}
