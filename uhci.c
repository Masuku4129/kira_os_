#include "usb.h"
#include "pci.h"
#include "iommu.h"

/* UHCI (USB 1.1, Intel): регистры в портах ввода-вывода (BAR4), расписание в памяти:
   список кадров (1024 указателя) -> очереди (QH) прерывающих каналов -> очередь управляющих передач.
   Каждую миллисекунду контроллер обходит расписание; мы только ставим дескрипторы передач (TD) и смотрим их итог.
   Порядок очередей в кадре: прерывающие каналы, управляющие передачи, массовые (флешки). Массовая передача - цепочка
   TD по размеру пакета точки (до 16 КБ за раз) с «остановкой на коротком пакете» (SPD) для чтения. */

#define USBCMD   0x00
#define USBSTS   0x02
#define USBINTR  0x04
#define FRNUM    0x06
#define FRBASE   0x08
#define SOFMOD   0x0C
#define PORTSC   0x10

#define CMD_RS 0x01
#define CMD_HCRESET 0x02
#define CMD_GRESET 0x04
#define CMD_CF 0x40
#define CMD_MAXP 0x80
#define STS_HALTED 0x20

#define PS_CCS 0x0001
#define PS_CSC 0x0002
#define PS_PE  0x0004
#define PS_PEC 0x0008
#define PS_LS  0x0100
#define PS_PR  0x0200

#define LP_T  1u
#define LP_QH 2u
#define LP_VF 4u

#define TD_SPD    (1u << 29)
#define TD_ACTIVE (1u << 23)
#define TD_STALL  (1u << 22)
#define TD_DBUF   (1u << 21)
#define TD_BABBLE (1u << 20)
#define TD_NAK    (1u << 19)
#define TD_CRC    (1u << 18)
#define TD_BITST  (1u << 17)
#define TD_ERRS   (TD_STALL | TD_DBUF | TD_BABBLE | TD_CRC | TD_BITST)
#define TD_IOC    (1u << 24)
#define TD_LS     (1u << 26)
#define TD_CERR3  (3u << 27)

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define NPIPE 64
#define NCTD  192
#define CBUF  1088          /* 64 под SETUP + 1024 данных */
#define NBTD  256           /* TD массовой передачи: 16 КБ пакетами по 64 */
#define BBUF  16384
#define QH_CTL  NPIPE
#define QH_BULK (NPIPE + 1)

typedef struct { volatile uint32_t link, elem, pad0, pad1; } qh_t;
typedef struct { volatile uint32_t link, cs, token, buf; uint32_t pad[4]; } td_t;

struct usb_pipe {
    int used;
    usb_dev_t* dev;
    qh_t* qh; td_t* td; uint32_t td_phys;
    uint8_t* buf; uint32_t buf_phys;
    uint8_t ep, toggle, bulk; uint16_t maxp;
};

typedef struct {
    uint16_t io;
    volatile uint32_t* frames;
    qh_t* qh; uint32_t qh_phys;               /* [0..NPIPE-1] - прерывающие каналы, [QH_CTL], [QH_BULK] */
    td_t* ctd; uint32_t ctd_phys;
    uint8_t* cbuf; uint32_t cbuf_phys;
    td_t* btd; uint32_t btd_phys;
    uint8_t* bbuf; uint32_t bbuf_phys;
    struct usb_pipe pipes[NPIPE];
    struct usb_pipe bpipes[NPIPE];
    uint8_t nports;
    uint8_t addr_used[128];
} uhci_t;

static uhci_t* ctl_state[8];            /* по числу найденных контроллеров */
static int nctl;

#ifdef KIRA_HOST_TEST                      /* хост-тест: порты - модель контроллера */
void outw(uint16_t p, uint16_t v); uint16_t inw(uint16_t p); void outl(uint16_t p, uint32_t v); void outb(uint16_t p, uint8_t v);
#else
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p)); }
#endif
#define BARRIER() __asm__ volatile ("" ::: "memory")

static uint32_t td_phys(uhci_t* u, int i) { return u->ctd_phys + (uint32_t)i * (uint32_t)sizeof(td_t); }

/* ---------------- управляющие передачи ---------------- */

static int uhci_control(usb_hc_t* hc, usb_dev_t* d, const usb_setup_t* s, void* data) {
    uhci_t* u = (uhci_t*)hc->priv;
    int in = (s->bmRequestType & 0x80) != 0;
    unsigned len = s->wLength;
    if (len > CBUF - 64) return -1;
    const uint8_t* sp = (const uint8_t*)s;
    for (int i = 0; i < 8; i++) u->cbuf[i] = sp[i];
    if (!in && len) for (unsigned i = 0; i < len; i++) u->cbuf[64 + i] = ((const uint8_t*)data)[i];
    uint32_t ls = d->speed == USB_SPEED_LOW ? TD_LS : 0, addr = (uint32_t)d->addr << 8;
    unsigned maxp = d->ep0_maxp ? d->ep0_maxp : 8;
    int n = 0;
    td_t* t = &u->ctd[n];
    t->cs = TD_ACTIVE | ls | TD_CERR3; t->token = (7u << 21) | addr | PID_SETUP; t->buf = u->cbuf_phys;
    n++;
    int first_data = n;
    uint32_t toggle = 1;
    for (unsigned off = 0; off < len; off += maxp) {
        if (n >= NCTD - 1) return -1;
        unsigned l = len - off < maxp ? len - off : maxp;
        t = &u->ctd[n];
        t->cs = TD_ACTIVE | ls | TD_CERR3;
        t->token = ((uint32_t)(l - 1) << 21) | (toggle << 19) | addr | (in ? PID_IN : PID_OUT);
        t->buf = u->cbuf_phys + 64 + off;
        toggle ^= 1; n++;
    }
    int status = n;
    t = &u->ctd[n];
    t->cs = TD_ACTIVE | ls | TD_CERR3 | TD_IOC;
    t->token = (0x7FFu << 21) | (1u << 19) | addr | (in && len ? PID_OUT : PID_IN);
    t->buf = 0; t->link = LP_T;
    n++;
    for (int i = 0; i < n - 1; i++) u->ctd[i].link = td_phys(u, i + 1) | LP_VF;
    BARRIER();
    u->qh[QH_CTL].elem = td_phys(u, 0);

    uint32_t deadline = usb_now_ms() + 1000;
    int result = -1, redirected = 0;
    for (;;) {
        BARRIER();
        int err = 0, short_at = -1;
        for (int i = 0; i < n; i++) {
            uint32_t cs = u->ctd[i].cs;
            if (cs & TD_ACTIVE) break;
            if (cs & TD_ERRS) { err = 1; break; }
            if (in && i >= first_data && i < status) {
                unsigned want = ((u->ctd[i].token >> 21) + 1) & 0x7FF, got = ((cs & 0x7FF) + 1) & 0x7FF;
                if (got < want && short_at < 0) short_at = i;
            }
        }
        if (err) break;
        if (!(u->ctd[status].cs & TD_ACTIVE)) { result = 0; break; }
        /* короткий пакет: данных больше не будет - сразу к стадии статуса */
        if (short_at >= 0 && short_at + 1 < status && !redirected && (u->ctd[short_at + 1].cs & TD_ACTIVE)) {
            u->qh[QH_CTL].elem = td_phys(u, status);
            redirected = 1;
        }
        if ((int32_t)(usb_now_ms() - deadline) > 0) break;
        usb_sleep_ms(1);
    }
    if (result < 0) {                                 /* снять незавершённые TD, контроллер закончит текущий кадр */
        for (int i = 0; i < n; i++) u->ctd[i].cs &= ~TD_ACTIVE;
    }
    u->qh[QH_CTL].elem = LP_T;
    BARRIER();
    if (result < 0) {
        usb_sleep_ms(2);
        for (int i = 0; i < n; i++) if (u->ctd[i].cs & TD_STALL) return USB_ERR_STALL;
        return USB_ERR;
    }
    unsigned got = 0;
    for (int i = first_data; i < status; i++) {
        uint32_t cs = u->ctd[i].cs;
        if (cs & TD_ACTIVE) break;
        got += ((cs & 0x7FF) + 1) & 0x7FF;
    }
    if (in) for (unsigned i = 0; i < got && i < len; i++) ((uint8_t*)data)[i] = u->cbuf[64 + i];
    return (int)got;
}

/* ---------------- прерывающие каналы ---------------- */

static void arm(struct usb_pipe* p) {
    uint32_t ls = p->dev->speed == USB_SPEED_LOW ? TD_LS : 0;
    p->td->link = LP_T;
    p->td->buf = p->buf_phys;
    p->td->token = ((uint32_t)(p->maxp - 1) << 21) | ((uint32_t)p->toggle << 19) | ((uint32_t)(p->ep & 15) << 15) | ((uint32_t)p->dev->addr << 8) | PID_IN;
    BARRIER();
    p->td->cs = TD_ACTIVE | ls | TD_CERR3;
    BARRIER();
    p->qh->elem = p->td_phys;
}

static usb_pipe_t* uhci_intr_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp, uint8_t interval) {
    (void)interval;                                  /* очередь обходится каждый кадр (1 мс): чаще, чем просит устройство */
    uhci_t* u = (uhci_t*)hc->priv;
    if (maxp == 0 || maxp > 64) return 0;
    for (int i = 0; i < NPIPE; i++) {
        struct usb_pipe* p = &u->pipes[i];
        if (p->used) continue;
        p->used = 1; p->bulk = 0; p->dev = d; p->ep = ep; p->maxp = maxp; p->toggle = 0;
        arm(p);
        return p;
    }
    return 0;
}

static int uhci_intr_poll(usb_hc_t* hc, usb_pipe_t* p, uint8_t* buf, unsigned cap) {
    (void)hc;
    BARRIER();
    uint32_t cs = p->td->cs;
    if (cs & TD_ACTIVE) return 0;
    if (cs & (TD_STALL | TD_CRC | TD_BITST | TD_BABBLE)) return -1;   /* нет ответа три раза подряд, STALL */
    if (cs & TD_DBUF) { arm(p); return 0; }
    unsigned n = ((cs & 0x7FF) + 1) & 0x7FF;
    if (n > cap) n = cap;
    for (unsigned i = 0; i < n; i++) buf[i] = p->buf[i];
    p->toggle ^= 1;
    arm(p);
    return (int)n;
}

/* ---------------- массовые каналы ---------------- */

static uint32_t btd_phys(uhci_t* u, int i) { return u->btd_phys + (uint32_t)i * (uint32_t)sizeof(td_t); }

static usb_pipe_t* uhci_bulk_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp) {
    uhci_t* u = (uhci_t*)hc->priv;
    if (d->speed == USB_SPEED_LOW || maxp == 0 || maxp > 64 || (BBUF + maxp - 1) / maxp > NBTD) return 0;
    for (int i = 0; i < NPIPE; i++) {
        struct usb_pipe* p = &u->bpipes[i];
        if (p->used) continue;
        p->used = 1; p->bulk = 1; p->dev = d; p->ep = ep; p->maxp = maxp; p->toggle = 0;
        return p;
    }
    return 0;
}

static int uhci_bulk_xfer(usb_hc_t* hc, usb_pipe_t* p, void* buf, unsigned len) {
    uhci_t* u = (uhci_t*)hc->priv;
    if (!p || !p->used || !p->bulk || len > BBUF) return USB_ERR;
    int in = (p->ep & 0x80) != 0;
    unsigned maxp = p->maxp;
    int n = len ? (int)((len + maxp - 1) / maxp) : 1;
    if (n > NBTD) return USB_ERR;
    if (!in) for (unsigned i = 0; i < len; i++) u->bbuf[i] = ((const uint8_t*)buf)[i];
    uint32_t tog = p->toggle, addr = (uint32_t)p->dev->addr << 8, epn = (uint32_t)(p->ep & 15) << 15;
    for (int i = 0; i < n; i++) {
        unsigned off = (unsigned)i * maxp, l = len - off < maxp ? len - off : maxp;
        td_t* t = &u->btd[i];
        t->link = i + 1 < n ? btd_phys(u, i + 1) | LP_VF : LP_T;
        t->buf = u->bbuf_phys + off;
        t->token = ((uint32_t)((l ? l : 0x800) - 1) << 21 & (0x7FFu << 21)) | (tog << 19) | epn | addr | (in ? PID_IN : PID_OUT);
        t->cs = TD_ACTIVE | TD_CERR3 | (in ? TD_SPD : 0);
        tog ^= 1;
    }
    BARRIER();
    u->qh[QH_BULK].elem = btd_phys(u, 0);
    uint32_t deadline = usb_now_ms() + 5000;
    int done = 0, err = 0, finished = 0;
    unsigned got = 0;
    for (;;) {
        BARRIER();
        while (done < n) {
            uint32_t cs = u->btd[done].cs;
            if (cs & TD_ACTIVE) break;
            if (cs & TD_ERRS) { err = (cs & TD_STALL) ? USB_ERR_STALL : USB_ERR; break; }
            unsigned want = ((u->btd[done].token >> 21) + 1) & 0x7FF, l = ((cs & 0x7FF) + 1) & 0x7FF;
            got += l; done++;
            if (l < want) { finished = 1; break; }            /* короткий пакет: данных больше нет */
        }
        if (err || finished || done == n) break;
        if ((int32_t)(usb_now_ms() - deadline) > 0) { err = USB_ERR; break; }
        usb_sleep_ms(1);
    }
    for (int i = done; i < n; i++) u->btd[i].cs &= ~TD_ACTIVE;
    u->qh[QH_BULK].elem = LP_T;
    BARRIER();
    if (err || finished) usb_sleep_ms(2);                        /* контроллер мог ещё стоять на этой очереди в кадре */
    p->toggle = (uint8_t)((p->toggle + (unsigned)done) & 1);
    if (err) return err;
    if (in) for (unsigned i = 0; i < got && i < len; i++) ((uint8_t*)buf)[i] = u->bbuf[i];
    return (int)got;
}

static void uhci_bulk_reset(usb_hc_t* hc, usb_pipe_t* p) { (void)hc; if (p) p->toggle = 0; }

static void uhci_pipe_close(usb_hc_t* hc, usb_pipe_t* p) {
    (void)hc;
    if (!p) return;
    if (!p->bulk) { p->qh->elem = LP_T; BARRIER(); }
    p->used = 0;
}

static void uhci_release(usb_hc_t* hc, usb_dev_t* d) {
    uhci_t* u = (uhci_t*)hc->priv;
    for (int i = 0; i < NPIPE; i++) {                            /* каналы, которые драйвер класса не закрыл */
        if (u->pipes[i].used && u->pipes[i].dev == d) uhci_pipe_close(hc, &u->pipes[i]);
        if (u->bpipes[i].used && u->bpipes[i].dev == d) u->bpipes[i].used = 0;
    }
    if (d->addr && d->addr < 128) u->addr_used[d->addr] = 0;
    d->addr = 0;
}

/* ---------------- порты ---------------- */

static uint16_t port_reg(int port) { return (uint16_t)(PORTSC + (port - 1) * 2); }

static int port_reset(uhci_t* u, int port) {
    uint16_t r = (uint16_t)(u->io + port_reg(port));
    outw(r, PS_PR);
    usb_sleep_ms(50);
    outw(r, 0);
    usb_sleep_ms(2);
    for (int tries = 0; tries < 16; tries++) {
        uint16_t v = inw(r);
        if (!(v & PS_CCS)) return -1;
        if (v & (PS_PEC | PS_CSC)) { outw(r, (uint16_t)((v & PS_PE) | PS_PEC | PS_CSC)); continue; }
        if (v & PS_PE) return 0;
        outw(r, PS_PE);
        usb_sleep_ms(10);
    }
    return -1;
}

/* Новое устройство (после сброса порта): размер пакета EP0 и адрес. */
static usb_dev_t* uhci_enumerate(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed) {
    uhci_t* u = (uhci_t*)hc->priv;
    if (speed != USB_SPEED_LOW && speed != USB_SPEED_FULL) speed = USB_SPEED_FULL;
    usb_dev_t* d = usb_dev_new(hc, parent, port, speed);
    if (!d) { usb_log("[KIRA USB] too many USB devices\n"); return 0; }
    d->addr = 0; d->ep0_maxp = 8;
    uint8_t dd[8];
    usb_setup_t s = { 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, 8 };
    int r = -1;
    for (int tries = 0; tries < 3 && r < 8; tries++) { r = uhci_control(hc, d, &s, dd); if (r < 8) usb_sleep_ms(20); }
    if (r < 8 || (dd[7] != 8 && dd[7] != 16 && dd[7] != 32 && dd[7] != 64)) { usb_log("[KIRA USB] UHCI: device does not answer\n"); usb_dev_free(d); return 0; }
    d->ep0_maxp = dd[7];
    uint8_t a = 0;
    for (int i = 1; i < 128; i++) if (!u->addr_used[i]) { a = (uint8_t)i; break; }
    if (!a) { usb_dev_free(d); return 0; }
    usb_setup_t sa = { 0x00, USB_REQ_SET_ADDRESS, a, 0, 0 };
    if (uhci_control(hc, d, &sa, 0) < 0) { usb_log("[KIRA USB] UHCI: SET_ADDRESS failed\n"); usb_dev_free(d); return 0; }
    usb_sleep_ms(10);
    u->addr_used[a] = 1;
    d->addr = a;
    return d;
}

static void attach_port(usb_hc_t* hc, uhci_t* u, int port) {
    if (port_reset(u, port) != 0) { usb_log("[KIRA USB] UHCI port reset failed\n"); return; }
    usb_sleep_ms(10);
    uint16_t v = inw((uint16_t)(u->io + port_reg(port)));
    usb_dev_t* d = uhci_enumerate(hc, 0, (uint8_t)port, (v & PS_LS) ? USB_SPEED_LOW : USB_SPEED_FULL);
    if (d) usb_attach(d);                            /* устройство остаётся занятым, даже без драйвера: не перебираем его снова */
}

static void uhci_ports(usb_hc_t* hc) {
    uhci_t* u = (uhci_t*)hc->priv;
    for (int port = 1; port <= u->nports; port++) {
        uint16_t r = (uint16_t)(u->io + port_reg(port));
        uint16_t v = inw(r);
        int present = usb_dev_on_port(hc, (uint8_t)port) != 0;
        if (v & PS_CSC) outw(r, (uint16_t)((v & PS_PE) | PS_CSC));
        if ((v & PS_CCS) && !present) attach_port(hc, u, port);
        else if (!(v & PS_CCS) && present) usb_detach_port(hc, (uint8_t)port);
        else if ((v & PS_CSC) && present) {          /* переподключили быстрее, чем мы опросили */
            usb_detach_port(hc, (uint8_t)port);
            attach_port(hc, u, port);
        }
    }
}

/* ---------------- запуск ---------------- */

int uhci_start(usb_hc_t* hc, const void* pd) {
    const pci_dev_t* p = (const pci_dev_t*)pd;
    if (nctl >= 8) { usb_log("[KIRA USB] more than 8 UHCI controllers: the rest are not used\n"); return -1; }
    if (!p->bar[4].io || !p->bar[4].base) { usb_log("[KIRA USB] UHCI without I/O BAR4: skipped\n"); return -1; }
    if (!ctl_state[nctl]) ctl_state[nctl] = (uhci_t*)usb_dma_alloc(sizeof(uhci_t), &(uint64_t){ 0 }, 0);
    if (!ctl_state[nctl]) { usb_log("[KIRA USB] UHCI: no memory for controller state\n"); return -1; }
    uhci_t* u = ctl_state[nctl];
    for (unsigned long i = 0; i < sizeof *u; i++) ((volatile uint8_t*)u)[i] = 0;
    u->io = (uint16_t)p->bar[4].base;
#if !defined(KIRA_HOST_TEST) && (defined(__aarch64__) || defined(__x86_64__))
    int iommu_rc = iommu_isolate_device(p);
    if (iommu_rc != IOMMU_OK && iommu_rc != IOMMU_E_NONE &&
        iommu_rc != IOMMU_E_NOCOVER) {
        usb_log("[KIRA USB] UHCI: IOMMU isolation failed; controller disabled\n");
        return -1;
    }
    if (!iommu_device_ready(p)) {
        usb_log("[KIRA USB] UHCI: requester is not isolated or safely unrestricted\n");
        return -1;
    }
#endif
    pci_enable(p, PCI_CMD_IO | PCI_CMD_INTX_OFF);
    pci_write16(p, 0xC0, 0x8F00);                     /* LEGSUP: BIOS больше не перехватывает USB (SMI, эмуляция PS/2) */

    uint64_t phys;
    uint8_t* mem = (uint8_t*)usb_dma_alloc(4096 * 13, &phys, 1);
    if (!mem) { usb_log("[KIRA USB] UHCI: no DMA memory below 4 GB\n"); return -1; }
#if !defined(KIRA_HOST_TEST) && (defined(__aarch64__) || defined(__x86_64__))
    if (iommu_device_isolated(p) &&
        iommu_map(p, phys, phys, 4096 * 13, IOMMU_R | IOMMU_W) != IOMMU_OK) {
        iommu_unmap_all(p);
        usb_log("[KIRA USB] UHCI: IOMMU DMA mapping failed; controller disabled\n");
        return -1;
    }
#endif
    u->frames = (volatile uint32_t*)mem;                              /* страница 0: кадры */
    u->qh = (qh_t*)(mem + 4096); u->qh_phys = (uint32_t)phys + 4096;   /* страница 1: очереди и каналы */
    u->ctd = (td_t*)(mem + 8192); u->ctd_phys = (uint32_t)phys + 8192; /* страницы 2-3: TD управляющих передач */
    u->cbuf = mem + 4096 * 4; u->cbuf_phys = (uint32_t)phys + 4096 * 4; /* страница 4: SETUP и данные */
    u->btd = (td_t*)(mem + 4096 * 5); u->btd_phys = (uint32_t)phys + 4096 * 5;  /* страницы 5-6: TD массовых передач */
    u->bbuf = mem + 4096 * 7; u->bbuf_phys = (uint32_t)phys + 4096 * 7;        /* страницы 7-10: их данные */
    /* каналы: очереди - в странице 1, TD и буферы - в страницах 11 и 12 */
    for (int i = 0; i < NPIPE; i++) {
        struct usb_pipe* pp = &u->pipes[i];
        pp->qh = &u->qh[i];
        pp->td = (td_t*)(mem + 4096 * 11 + i * 32); pp->td_phys = (uint32_t)phys + 4096 * 11 + (uint32_t)i * 32;   /* страница 11: TD каналов */
        pp->buf = mem + 4096 * 12 + i * 64; pp->buf_phys = (uint32_t)phys + 4096 * 12 + (uint32_t)i * 64;          /* страница 12: их данные */
    }
    for (int i = 0; i <= QH_BULK; i++) {
        u->qh[i].elem = LP_T;
        u->qh[i].link = i < QH_BULK ? (u->qh_phys + (uint32_t)(i + 1) * 16) | LP_QH : LP_T;
    }
    for (int i = 0; i < 1024; i++) u->frames[i] = u->qh_phys | LP_QH;

    outw((uint16_t)(u->io + USBCMD), 0);
    for (int i = 0; i < 20 && !(inw((uint16_t)(u->io + USBSTS)) & STS_HALTED); i++) usb_sleep_ms(1);
    outw((uint16_t)(u->io + USBCMD), CMD_GRESET);
    usb_sleep_ms(20);
    outw((uint16_t)(u->io + USBCMD), 0);
    usb_sleep_ms(10);
    outw((uint16_t)(u->io + USBCMD), CMD_HCRESET);
    for (int i = 0; i < 100 && (inw((uint16_t)(u->io + USBCMD)) & CMD_HCRESET); i++) usb_sleep_ms(1);
    if (inw((uint16_t)(u->io + USBCMD)) & CMD_HCRESET) {
#if !defined(KIRA_HOST_TEST) && (defined(__aarch64__) || defined(__x86_64__))
        iommu_unmap_all(p);
#endif
        usb_log("[KIRA USB] UHCI reset timeout\n");
        return -1;
    }
    outw((uint16_t)(u->io + USBINTR), 0);
    outw((uint16_t)(u->io + FRNUM), 0);
    outl((uint16_t)(u->io + FRBASE), (uint32_t)phys);
    outb((uint16_t)(u->io + SOFMOD), 0x40);
    outw((uint16_t)(u->io + USBSTS), 0xFFFF);
    pci_enable(p, PCI_CMD_MASTER);
    if (!(pci_read16(p, 0x04) & PCI_CMD_MASTER)) {
        if (pci_disable_bus_master(p)) {
#if !defined(KIRA_HOST_TEST) && (defined(__aarch64__) || defined(__x86_64__))
            (void)iommu_unmap_all(p);
#endif
        } /* otherwise keep DMA backing pages pinned */
        usb_log("[KIRA USB] UHCI: PCI bus mastering was not granted\n");
        return -1;
    }
    outw((uint16_t)(u->io + USBCMD), CMD_RS | CMD_CF | CMD_MAXP);
    usb_sleep_ms(5);
    if (inw((uint16_t)(u->io + USBSTS)) & STS_HALTED) {
        if (pci_disable_bus_master(p)) {
#if !defined(KIRA_HOST_TEST) && (defined(__aarch64__) || defined(__x86_64__))
            (void)iommu_unmap_all(p);
#endif
        } /* otherwise retain all DMA mappings */
        usb_log("[KIRA USB] UHCI does not start; DMA disabled\n");
        return -1;
    }

    /* портов у UHCI два; у некоторых больше - признак порта: бит 7 читается как 1 */
    u->nports = 2;
    for (int port = 3; port <= 7; port++) {
        uint16_t v = inw((uint16_t)(u->io + port_reg(port)));
        if (v == 0xFFFF || !(v & 0x80)) break;
        u->nports = (uint8_t)port;
    }
    hc->name = "UHCI"; hc->priv = u;
    hc->control = uhci_control; hc->enumerate = uhci_enumerate;
    hc->intr_open = uhci_intr_open; hc->intr_poll = uhci_intr_poll;
    hc->bulk_open = uhci_bulk_open; hc->bulk_xfer = uhci_bulk_xfer; hc->bulk_reset = uhci_bulk_reset;
    hc->pipe_close = uhci_pipe_close; hc->ports = uhci_ports; hc->release = uhci_release;
    nctl++;
    usb_log("[KIRA USB] UHCI at I/O "); usb_log_hex(u->io); usb_log(", "); usb_log_dec(u->nports); usb_log(" ports, polled schedule\n");
    return 0;
}
