#include "usb.h"
#include "input.h"
#include "devhealth.h"

/* ---------------- устройства и HID ---------------- */

static usb_dev_t devs[USB_MAX_DEV];

typedef struct {
    int used, kind;                      /* HID_PROTO_KEYBOARD / HID_PROTO_MOUSE (любой указатель) */
    usb_dev_t* dev;
    usb_pipe_t* pipe;
    hid_kbd_t kbd;
    hid_pointer_t ptr;
    uint32_t reports;
    uint8_t last[8]; uint8_t last_len;     /* последний отчёт - для команды usb */
    uint32_t win_ms, win_keys;             /* иммунитет: нажатия в текущем окне 200 мс */
    uint8_t held, allowed;
    uint16_t nheld; uint8_t heldbuf[256];  /* нажатия задержанного устройства до решения Разума */
} hid_t;
static hid_t hids[USB_MAX_HID];
static volatile int verdict = -1;          /* решение Разума для задержанных: 1 - отключить, 0 - отпустить */
static volatile int allow_req;
/* Отключённые как подозрительные: «usb allow» подключает их заново (устройство остаётся на шине). */
static usb_dev_t* blocked[USB_MAX_HID];

void usb_hid_verdict(int block) { verdict = block ? 1 : 0; }
void usb_hid_allow_all(void) { allow_req = 1; }

static void hid_name16(const usb_dev_t* d, char* out) {
    const char* hx = "0123456789abcdef";
    const char* pre = "kbd "; int k = 0;
    while (pre[k]) { out[k] = pre[k]; k++; }
    for (int s = 12; s >= 0; s -= 4) out[k++] = hx[(d->vendor >> s) & 15];
    out[k++] = ':';
    for (int s = 12; s >= 0 && k < 15; s -= 4) out[k++] = hx[(d->product >> s) & 15];
    out[k] = 0;
}

usb_dev_t* usb_dev_new(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed) {
    for (int i = 0; i < USB_MAX_DEV; i++)
        if (!devs[i].used) {
            usb_dev_t* d = &devs[i];
            *d = (usb_dev_t){ 0 };
            d->hc = hc; d->parent = parent; d->used = 1; d->speed = speed; d->port = port;
            d->ep0_maxp = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
            if (!parent) { d->root_port = port; d->depth = 0; d->route = 0; }
            else {
                d->root_port = parent->root_port; d->depth = (uint8_t)(parent->depth + 1);
                d->route = parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * parent->depth));
                if ((speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) && parent->speed == USB_SPEED_HIGH) {
                    d->tt_slot = (uint8_t)parent->hc_slot; d->tt_port = port;         /* переводчик - этот хаб */
                } else { d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port; }  /* унаследован (FS-хаб за HS) */
            }
            return d;
        }
    return 0;
}

void usb_dev_free(usb_dev_t* d) { if (d->hc->release) d->hc->release(d->hc, d); d->used = 0; }

usb_dev_t* usb_dev_on_port(usb_hc_t* hc, uint8_t port) {
    for (int i = 0; i < USB_MAX_DEV; i++) if (devs[i].used && devs[i].hc == hc && !devs[i].parent && devs[i].port == port) return &devs[i];
    return 0;
}

static void drop_hid(usb_dev_t* d) {
    for (int i = 0; i < USB_MAX_HID; i++)
        if (hids[i].used && hids[i].dev == d) {
            if (d->hc->pipe_close) d->hc->pipe_close(d->hc, hids[i].pipe);
            hids[i].used = 0;
            if (hids[i].kind == HID_PROTO_KEYBOARD) {          /* отпустить всё, что было нажато */
                uint8_t none[8] = { 0 }, out[64];
                unsigned n = hid_kbd_report(&hids[i].kbd, none, 8, usb_now_ms(), out, sizeof out);
                for (unsigned k = 0; k < n; k++) input_push_scancode_from(INPUT_SRC_USB, out[k]);
            }
        }
}

void usb_detach(usb_dev_t* d) {
    if (!d || !d->used) return;
    for (int b = 0; b < USB_MAX_HID; b++) if (blocked[b] == d) blocked[b] = 0;
    for (int i = 0; i < USB_MAX_DEV; i++)                       /* сначала всё, что за хабом */
        if (devs[i].used && devs[i].parent == d) usb_detach(&devs[i]);
    drop_hid(d);
    hub_detach(d);
    msc_detach(d);
    hub_forget(d);
    usb_dev_free(d);
}

void usb_detach_port(usb_hc_t* hc, uint8_t port) {
    usb_dev_t* d = usb_dev_on_port(hc, port);
    if (!d) return;
    usb_log("[KIRA USB] "); usb_log(hc->name); usb_log(" port "); usb_log_dec(port); usb_log(": device removed\n");
    usb_detach(d);
}

int usb_ctl(usb_dev_t* d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void* data) {
    usb_setup_t s = { rt, req, val, idx, len };
    return d->hc->control(d->hc, d, &s, data);
}
#define ctl usb_ctl

int usb_clear_halt(usb_dev_t* d, usb_pipe_t* p, uint8_t ep) {
    int r = ctl(d, 0x02, 1, 0, ep, 0, 0);                        /* CLEAR_FEATURE(ENDPOINT_HALT) */
    if (d->hc->bulk_reset) d->hc->bulk_reset(d->hc, p);
    return r;
}

static const char* kind_name(int k) { return k == HID_PROTO_KEYBOARD ? "keyboard" : "pointer"; }

/* Путь от корня: порт 3.3.1 - корневой порт 3, порт 3 первого хаба, порт 1 второго. */
static int path_of(const usb_dev_t* d, uint8_t* out) {
    int n = 0;
    for (const usb_dev_t* p = d; p->parent && n < 7; p = p->parent) out[n++] = p->port;
    return n;                                                  /* out - от устройства к корню */
}

static void where(usb_dev_t* d) {
    usb_log("[KIRA USB] "); usb_log(d->hc->name); usb_log(" port "); usb_log_dec(d->root_port);
    uint8_t pp[8]; int n = path_of(d, pp);
    while (n--) { usb_log("."); usb_log_dec(pp[n]); }
}

static int hid_attach(usb_dev_t* d, const usb_iface_t* f) {
    const __typeof__(f->ep[0])* e = 0;
    for (int k = 0; k < f->nep; k++) if ((f->ep[k].addr & 0x80) && (f->ep[k].attr & 3) == 3) { e = &f->ep[k]; break; }
    if (!e) return -1;
    hid_t* h = 0;
    for (int k = 0; k < USB_MAX_HID; k++) if (!hids[k].used) { h = &hids[k]; break; }
    if (!h) return -1;
    *h = (hid_t){ 0 };
    h->dev = d;
    if (f->proto == HID_PROTO_KEYBOARD && f->sub == 1) {
        ctl(d, 0x21, HID_REQ_SET_PROTOCOL, 0, f->num, 0, 0);        /* загрузочный протокол: 8 байт */
        ctl(d, 0x21, HID_REQ_SET_IDLE, 0, f->num, 0, 0);            /* отчёт только при изменении */
        h->kind = HID_PROTO_KEYBOARD;
        hid_kbd_reset(&h->kbd);
    } else {
        int ok = -1;
        if (f->report_len >= 8 && f->report_len <= 1024) {
            uint8_t rd[1024];
            int rl = ctl(d, 0x81, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, f->num, f->report_len, rd);
            if (rl > 0) ok = hid_parse_pointer(rd, (unsigned)rl, &h->ptr);
        }
        if (ok == 0) {
            if (f->sub == 1) ctl(d, 0x21, HID_REQ_SET_PROTOCOL, 1, f->num, 0, 0);   /* отчётный протокол */
        } else if (f->sub == 1 && f->proto == HID_PROTO_MOUSE) {
            ctl(d, 0x21, HID_REQ_SET_PROTOCOL, 0, f->num, 0, 0);
            hid_boot_mouse_layout(&h->ptr);
            ok = 0;
        }
        if (ok != 0) { usb_log("[KIRA USB] HID interface is not a keyboard or pointer: skipped\n"); return -1; }
        ctl(d, 0x21, HID_REQ_SET_IDLE, 0, f->num, 0, 0);
        h->kind = HID_PROTO_MOUSE;
    }
    h->pipe = d->hc->intr_open(d->hc, d, e->addr, e->maxp, e->interval);
    if (!h->pipe) { usb_log("[KIRA USB] cannot open interrupt endpoint\n"); return -1; }
    h->used = 1;
    usb_log("[KIRA USB] "); usb_log(kind_name(h->kind));
    if (h->kind == HID_PROTO_MOUSE) usb_log(h->ptr.abs ? " (absolute: tablet/touch)" : " (relative)");
    usb_log(" ready\n");
    return 0;
}

int usb_attach(usb_dev_t* d) {
    uint8_t dd[18];
    if (ctl(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, 18, dd) < 18 || dd[1] != USB_DT_DEVICE) {
        usb_log("[KIRA USB] device descriptor read failed\n"); return -1;
    }
    d->vendor = (uint16_t)(dd[8] | dd[9] << 8); d->product = (uint16_t)(dd[10] | dd[11] << 8);
    d->dev_class = dd[4]; d->dev_proto = dd[6];
    where(d);
    usb_log(": device "); usb_log_hex(d->vendor); usb_log(":"); usb_log_hex(d->product);
    usb_log(d->speed == USB_SPEED_LOW ? " low-speed" : d->speed == USB_SPEED_FULL ? " full-speed" : d->speed == USB_SPEED_HIGH ? " high-speed" : " super-speed");
    usb_log(", class "); usb_log_dec(dd[4]); usb_log("\n");

    uint8_t cfg[512];
    if (ctl(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, 9, cfg) < 9) { usb_log("[KIRA USB] config descriptor read failed\n"); return -1; }
    unsigned total = (unsigned)cfg[2] | (unsigned)cfg[3] << 8;
    if (total > sizeof cfg) total = sizeof cfg;
    int got = ctl(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, (uint16_t)total, cfg);
    if (got < 9) { usb_log("[KIRA USB] config descriptor read failed\n"); return -1; }
    usb_iface_t fs[8]; uint8_t cv;
    int n = usb_parse_ifaces(cfg, (unsigned)got, &cv, fs, 8);
    if (n <= 0) { usb_log("[KIRA USB] no interfaces: device left unused\n"); return -1; }
    if (ctl(d, 0x00, USB_REQ_SET_CONFIGURATION, cv, 0, 0, 0) < 0) { usb_log("[KIRA USB] SET_CONFIGURATION failed\n"); return -1; }

    int used = 0;
    for (int i = 0; i < n; i++) {
        const usb_iface_t* f = &fs[i];
        int r = -1;
        if (f->cls == 9 || dd[4] == 9) r = hub_attach(d, f);
        else if (f->cls == 3) r = hid_attach(d, f);
        else if (f->cls == 8) r = msc_attach(d, f);
        if (r == 0) used++;
        if (f->cls == 9 || dd[4] == 9) break;                           /* у хаба один интерфейс */
    }
    if (!used) usb_log("[KIRA USB] no supported interface (HID, hub, storage): device left unused\n");
    return used ? 0 : -1;
}

void usb_hid_poll(uint32_t now) {
    uint8_t buf[64], out[64];
    for (int i = 0; i < USB_MAX_HID; i++) {
        hid_t* h = &hids[i];
        if (!h->used) continue;
        usb_hc_t* hc = h->dev->hc;
        for (int guard = 0; guard < 8; guard++) {                 /* несколько отчётов за шаг, но не бесконечно */
            int r = hc->intr_poll(hc, h->pipe, buf, sizeof buf);
            if (r < 0) {                                           /* конечная точка сломалась: устройство ушло */
                usb_log("[KIRA USB] "); usb_log(kind_name(h->kind)); usb_log(" stopped responding\n");
                { char nm[16]; hid_name16(h->dev, nm); devhealth_error(nm); }
                usb_dev_t* dv = h->dev;
                if (dv->parent) drop_hid(dv); else usb_detach(dv);   /* за хабом отключение увидит сам хаб */
                break;
            }
            if (r == 0) break;
            h->reports++;
            h->last_len = (uint8_t)(r > 8 ? 8 : r);
            for (int k = 0; k < h->last_len; k++) h->last[k] = buf[k];
            if (h->kind == HID_PROTO_KEYBOARD) {
                unsigned n = hid_kbd_report(&h->kbd, buf, (unsigned)r, now, out, sizeof out);
                unsigned makes = 0;
                for (unsigned k = 0; k < n; k++) if (out[k] != 0xE0 && !(out[k] & 0x80)) makes++;
                if (now - h->win_ms >= 200) { h->win_ms = now; h->win_keys = 0; }
                h->win_keys += makes;
                if (!h->held && !h->allowed && h->win_keys > USB_HID_BURST) {    /* рефлекс: быстрее человека */
                    h->held = 1; h->nheld = 0;
                    char nm[16]; hid_name16(h->dev, nm);
                    devhealth_hid_suspect(nm, h->win_keys * 5);
                    usb_log("[KIRA USB] keyboard "); usb_log(nm); usb_log(" types faster than a human: input held for the mind's decision\n");
                }
                for (unsigned k = 0; k < n; k++) {
                    if (!h->held) input_push_scancode_from(INPUT_SRC_USB, out[k]);
                    else if (h->nheld < sizeof h->heldbuf) h->heldbuf[h->nheld++] = out[k];
                }
            } else {
                mouse_event_t ev;
                if (hid_pointer_decode(&h->ptr, buf, (unsigned)r, &ev)) input_push_mouse(&ev);
            }
        }
        if (h->used && h->kind == HID_PROTO_KEYBOARD) {
            unsigned n = hid_kbd_tick(&h->kbd, now, out, sizeof out);
            for (unsigned k = 0; k < n; k++) if (!h->held) input_push_scancode_from(INPUT_SRC_USB, out[k]);
        }
    }
    int v = verdict;
    if (v >= 0) {                                               /* решение Разума по задержанным */
        verdict = -1;
        for (int i = 0; i < USB_MAX_HID; i++) {
            hid_t* h = &hids[i];
            if (!h->used || !h->held) continue;
            if (v == 0) {                                       /* отпустить: накопленное - в систему */
                h->held = 0; h->allowed = 1;
                for (unsigned k = 0; k < h->nheld; k++) input_push_scancode_from(INPUT_SRC_USB, h->heldbuf[k]);
                h->nheld = 0;
            } else {
                usb_dev_t* d = h->dev;
                for (int b = 0; b < USB_MAX_HID; b++) if (!blocked[b]) { blocked[b] = d; break; }
                usb_log("[KIRA USB] suspicious keyboard disabled by the mind ('usb allow' to re-enable)\n");
                drop_hid(d);
            }
        }
    }
    if (allow_req) {                                            /* «usb allow»: отключённые - заново, без рефлекса */
        allow_req = 0;
        for (int b = 0; b < USB_MAX_HID; b++) {
            usb_dev_t* d = blocked[b];
            blocked[b] = 0;
            if (!d || !d->used) continue;
            uint8_t cfg[512]; usb_iface_t fs[8]; uint8_t cv;
            int got = ctl(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, sizeof cfg, cfg);
            int nf = got >= 9 ? usb_parse_ifaces(cfg, (unsigned)got, &cv, fs, 8) : 0;
            for (int f = 0; f < nf; f++)
                if (fs[f].cls == 3 && hid_attach(d, &fs[f]) == 0)
                    for (int k = 0; k < USB_MAX_HID; k++) if (hids[k].used && hids[k].dev == d) hids[k].allowed = 1;
        }
        for (int i = 0; i < USB_MAX_HID; i++) if (hids[i].used && hids[i].held) { verdict = 0; break; }
    }
}

unsigned usb_hid_count(void) { unsigned n = 0; for (int i = 0; i < USB_MAX_HID; i++) n += hids[i].used != 0; return n; }

#ifdef KIRA_HOST_TEST
int usb_call(int (*fn)(void*), void* arg) { return fn(arg); }
#endif

#ifndef KIRA_HOST_TEST
/* ---------------- ядро: контроллеры на PCI и поток ---------------- */
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "uart.h"
#include "spinlock.h"
#include "cpu.h"

#define USB_MAX_HC 32
static usb_hc_t hcs[USB_MAX_HC];
static int nhc;
static const pci_dev_t* hc_pci[USB_MAX_HC];

void* platform_dma_pool(uint32_t bytes, uint64_t* phys, int below_4g) __attribute__((weak));
void* usb_dma_alloc(uint32_t bytes, uint64_t* phys, int below_4g) {
    if (platform_dma_pool) { void* q = platform_dma_pool(bytes, phys, below_4g); if (q) return q; }
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t pa = pmm_alloc_contiguous(pages);
    if (!pa) return 0;
    if (below_4g && pa + (uint64_t)pages * 4096 > 0x100000000ULL) { pmm_free_contiguous(pa, pages); return 0; }
    for (uint32_t i = 0; i < pages; i++) {                    /* устройство и процессор видят один адрес */
        uint64_t t;
        if (vmm_translate(pa + (uint64_t)i * 4096, &t) != 0 || t != pa + (uint64_t)i * 4096) { pmm_free_contiguous(pa, pages); return 0; }
    }
    uint8_t* p = (uint8_t*)(uintptr_t)pa;
    for (uint64_t i = 0; i < (uint64_t)pages * 4096; i++) p[i] = 0;
    *phys = pa;
    return p;
}

void usb_sleep_ms(uint32_t ms) { sched_sleep_ticks((uint32_t)(((uint64_t)ms * TIMER_HZ + 999) / 1000) + 1); }
uint32_t usb_now_ms(void) { return (uint32_t)clock_ms(); }
void usb_log(const char* s) { uart_print(s); }
void usb_log_dec(uint64_t v) { uart_print_dec(v); }
void usb_log_hex(uint64_t v) { uart_print_hex(v); }

static spinlock_t qlk = SPINLOCK_INIT;
static thread_t* usb_thr;
static int usb_idle;
typedef struct call { int (*fn)(void*); void* arg; volatile int done; int rc; thread_t* waiter; struct call* next; } call_t;
static call_t* qhead;
static call_t** qtail = &qhead;

int usb_call(int (*fn)(void*), void* arg) {
    if (!usb_thr || sched_current() == usb_thr) return fn(arg);
    call_t c = { fn, arg, 0, 0, (thread_t*)sched_current(), 0 };
    uint64_t f = spin_lock_irqsave(&qlk);
    *qtail = &c; qtail = &c.next;
    if (usb_idle) { usb_idle = 0; sched_wake_waiter(usb_thr); }
    while (!c.done) {
        sched_wait_and_unlock(&qlk, ~0ULL);
        irq_restore(f);
        f = spin_lock_irqsave(&qlk);
    }
    spin_unlock_irqrestore(&qlk, f);
    return c.rc;
}

static void run_calls(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&qlk);
        call_t* c = qhead;
        if (c) { qhead = c->next; if (!qhead) qtail = &qhead; }
        spin_unlock_irqrestore(&qlk, f);
        if (!c) return;
        int rc = c->fn(c->arg);
        f = spin_lock_irqsave(&qlk);
        c->rc = rc; c->done = 1;
        sched_wake_waiter(c->waiter);
        spin_unlock_irqrestore(&qlk, f);
    }
}

static void usb_main(void* arg) {
    (void)arg;
    usb_thr = (thread_t*)sched_current();
    if (pci_count() == 0) pci_init();
    for (int i = 0; i < pci_count() && nhc < USB_MAX_HC; i++) {
        const pci_dev_t* p = pci_get(i);
        if (p->class_code != 0x0C || p->subclass != 0x03) continue;
        int rc = -1;
        if (p->prog_if == 0x30) rc = xhci_start(&hcs[nhc], p);
#if defined(__x86_64__)
        else if (p->prog_if == 0x00) rc = uhci_start(&hcs[nhc], p);
#endif
        else {
            uart_print("[KIRA USB] ");
            uart_print(p->prog_if == 0x20 ? "EHCI" : p->prog_if == 0x10 ? "OHCI" : p->prog_if == 0x00 ? "UHCI (needs x86 port I/O)" : "unknown");
            uart_print(" controller at PCI "); uart_print_dec(p->bus); uart_print(":"); uart_print_dec(p->dev);
            uart_print(" - no driver yet (UHCI and xHCI are supported)\n");
            continue;
        }
        if (rc == 0) hc_pci[nhc++] = p;
    }
    if (!nhc) { uart_print("[KIRA USB] No supported USB controller: keyboard and mouse stay on PS/2.\n"); usb_thr = 0; sched_exit(); }
    uint32_t last_ports = 0;
    for (int k = 0; k < nhc; k++) hcs[k].ports(&hcs[k]);
    for (;;) {
        run_calls();
        uint32_t now = usb_now_ms();
        usb_hid_poll(now);
        hub_poll();
        msc_poll();
        if (now - last_ports >= 100) { last_ports = now; for (int k = 0; k < nhc; k++) hcs[k].ports(&hcs[k]); }
        /* есть устройства ввода или хабы - опрос каждые ~4 мс; нет - только порты. Запросы будят поток сразу. */
        uint64_t ticks = (usb_hid_count() || hub_count()) ? (TIMER_HZ >= 500 ? 2 : 1) : (uint64_t)TIMER_HZ / 20 + 1;
        uint64_t f = spin_lock_irqsave(&qlk);
        if (!qhead) { usb_idle = 1; sched_wait_and_unlock(&qlk, ticks); irq_restore(f); }
        else spin_unlock_irqrestore(&qlk, f);
        usb_idle = 0;
    }
}

void usb_init(void) {
    if (!sched_create("usb", usb_main, 0, PRIO_CRITICAL)) uart_print("[KIRA USB] cannot start usb thread.\n");
}

void usb_print(void) {
    if (!nhc) { uart_print("USB: контроллеров нет (или ещё идёт запуск)\n"); return; }
    for (int k = 0; k < nhc; k++) {
        uart_print("контроллер "); uart_print(hcs[k].name); uart_print(" (PCI ");
        uart_print_dec(hc_pci[k]->bus); uart_print(":"); uart_print_dec(hc_pci[k]->dev); uart_print(")\n");
        for (int i = 0; i < USB_MAX_DEV; i++) {
            const usb_dev_t* d = &devs[i];
            if (!d->used || d->hc != &hcs[k]) continue;
            uart_print("  порт "); uart_print_dec(d->root_port);
            { uint8_t pp[8]; int n = path_of(d, pp); while (n--) { uart_print("."); uart_print_dec(pp[n]); } }
            uart_print(": "); uart_print_hex(d->vendor); uart_print(":");
            uart_print_hex(d->product); uart_print(", адрес "); uart_print_dec(d->addr);
            if (d->is_hub) { uart_print(", хаб на "); uart_print_dec(d->hub_nports); uart_print(" портов"); }
            for (int h = 0; h < USB_MAX_HID; h++)
                if (hids[h].used && hids[h].dev == d) {
                    uart_print(hids[h].kind == HID_PROTO_KEYBOARD ? ", клавиатура" : hids[h].ptr.abs ? ", планшет" : ", мышь");
                    uart_print(" (отчётов "); uart_print_dec(hids[h].reports); uart_print(")");
                    if (hids[h].kind != HID_PROTO_KEYBOARD) {                     /* как разобран отчёт: для проверки чужих устройств */
                        const hid_pointer_t* q = &hids[h].ptr;
                        uart_print("\n    разбор: "); uart_print(q->abs ? "абсолютные" : "относительные");
                        uart_print(" X бит "); uart_print_dec(q->x.off); uart_print("+"); uart_print_dec(q->x.bits);
                        uart_print(", Y бит "); uart_print_dec(q->y.off); uart_print("+"); uart_print_dec(q->y.bits);
                        uart_print(", кнопок "); uart_print_dec(q->btn_count);
                        if (q->report_id) { uart_print(", номер отчёта "); uart_print_dec(q->report_id); }
                    }
                    if (hids[h].last_len) {
                        uart_print("\n    последний отчёт:");
                        for (int k = 0; k < hids[h].last_len; k++) { uart_putchar(' '); uart_putchar("0123456789ABCDEF"[hids[h].last[k] >> 4]); uart_putchar("0123456789ABCDEF"[hids[h].last[k] & 15]); }
                    }
                }
            uart_print("\n");
        }
    }
    msc_print(uart_print);
}
#endif
