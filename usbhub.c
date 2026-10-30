#include "usb.h"

/* Хабы USB 2.0 (и USB 2.0-часть хабов USB 3): питание портов, подключение и отключение по точке «изменения»,
   сброс порта, скорость, адресация устройства через контроллер (для xHCI - маршрут и переводчик транзакций).
   Глубина - до 5 уровней хабов (предел USB). */

#define HUB_PORTS 15
#define PS_CONNECTION  (1u << 0)
#define PS_ENABLE      (1u << 1)
#define PS_RESET       (1u << 4)
#define PS_POWER       (1u << 8)
#define PS_LOW_SPEED   (1u << 9)
#define PS_HIGH_SPEED  (1u << 10)
#define C_CONNECTION   (1u << 16)
#define C_RESET        (1u << 20)

enum { F_PORT_RESET = 4, F_PORT_POWER = 8, F_C_CONNECTION = 16, F_C_ENABLE = 17, F_C_SUSPEND = 18, F_C_OVER = 19, F_C_RESET = 20 };

typedef struct {
    int used;
    usb_dev_t* dev;
    usb_pipe_t* pipe;
    uint8_t nports, scan;
    usb_dev_t* child[HUB_PORTS + 1];
} hub_t;
static hub_t hubs[USB_MAX_HUB];

int hub_count(void) { int n = 0; for (int i = 0; i < USB_MAX_HUB; i++) n += hubs[i].used; return n; }

static int port_status(hub_t* h, int port, uint32_t* st) {
    uint8_t b[4];
    if (usb_ctl(h->dev, 0xA3, 0, 0, (uint16_t)port, 4, b) < 4) return -1;
    *st = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}
static int set_feature(hub_t* h, int port, int f) { return usb_ctl(h->dev, 0x23, 3, (uint16_t)f, (uint16_t)port, 0, 0); }
static int clear_feature(hub_t* h, int port, int f) { return usb_ctl(h->dev, 0x23, 1, (uint16_t)f, (uint16_t)port, 0, 0); }

static void child_gone(hub_t* h, int port) {
    usb_dev_t* c = h->child[port];
    if (!c) return;
    h->child[port] = 0;
    usb_log("[KIRA USB] hub port "); usb_log_dec((uint64_t)port); usb_log(": device removed\n");
    if (c->used) usb_detach(c);
}

static void port_event(hub_t* h, int port) {
    uint32_t st;
    if (port_status(h, port, &st) != 0) return;
    if (st & C_CONNECTION) clear_feature(h, port, F_C_CONNECTION);
    if (st & (1u << 17)) clear_feature(h, port, F_C_ENABLE);
    if (st & (1u << 18)) clear_feature(h, port, F_C_SUSPEND);
    if (st & (1u << 19)) clear_feature(h, port, F_C_OVER);
    if (st & C_RESET) clear_feature(h, port, F_C_RESET);
    int connected = (st & PS_CONNECTION) != 0;
    if (h->child[port] && (!connected || (st & C_CONNECTION))) child_gone(h, port);   /* вынули (или успели заменить) */
    if (!connected || h->child[port]) return;
    if (h->dev->depth >= 5) { usb_log("[KIRA USB] too many hubs in a chain (USB allows 5)\n"); return; }
    usb_sleep_ms(100);                                                 /* контакт успокаивается */
    if (set_feature(h, port, F_PORT_RESET) < 0) return;
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        usb_sleep_ms(10);
        if (port_status(h, port, &st) != 0) return;
        if ((st & C_RESET) && !(st & PS_RESET)) { ok = 1; break; }
    }
    clear_feature(h, port, F_C_RESET);
    if (!ok || !(st & PS_CONNECTION) || !(st & PS_ENABLE)) { usb_log("[KIRA USB] hub port reset failed\n"); return; }
    usb_sleep_ms(10);                                                  /* восстановление после сброса */
    uint8_t speed = (st & PS_LOW_SPEED) ? USB_SPEED_LOW : (st & PS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    if (h->dev->speed != USB_SPEED_HIGH && speed == USB_SPEED_HIGH) speed = USB_SPEED_FULL;
    usb_dev_t* c = h->dev->hc->enumerate(h->dev->hc, h->dev, (uint8_t)port, speed);
    if (!c) { usb_log("[KIRA USB] device behind hub did not take an address\n"); return; }
    h->child[port] = c;
    usb_attach(c);                                                     /* устройство остаётся занятым даже без драйвера */
}

int hub_attach(usb_dev_t* d, const usb_iface_t* f) {
    const __typeof__(f->ep[0])* e = 0;
    for (int k = 0; k < f->nep; k++) if ((f->ep[k].addr & 0x80) && (f->ep[k].attr & 3) == 3) { e = &f->ep[k]; break; }
    if (!e) return -1;
    if (d->speed == USB_SPEED_SUPER) { usb_log("[KIRA USB] USB 3 hub: only its USB 2 part is used for now\n"); return -1; }
    hub_t* h = 0;
    for (int i = 0; i < USB_MAX_HUB; i++) if (!hubs[i].used) { h = &hubs[i]; break; }
    if (!h) { usb_log("[KIRA USB] too many hubs\n"); return -1; }
    uint8_t hd[16];
    int n = usb_ctl(d, 0xA0, 6, 0x2900, 0, sizeof hd, hd);
    if (n < 7 || hd[1] != 0x29 || !hd[2]) { usb_log("[KIRA USB] hub descriptor read failed\n"); return -1; }
    *h = (hub_t){ 0 };
    h->dev = d;
    h->nports = hd[2] > HUB_PORTS ? HUB_PORTS : hd[2];
    uint16_t chars = (uint16_t)(hd[3] | hd[4] << 8);
    d->is_hub = 1; d->hub_nports = h->nports;
    d->hub_ttt = (uint8_t)((chars >> 5) & 3);
    d->hub_mtt = d->speed == USB_SPEED_HIGH && d->dev_proto == 2;
    h->pipe = d->hc->intr_open(d->hc, d, e->addr, e->maxp, e->interval);   /* xHCI здесь же узнаёт, что это хаб */
    if (!h->pipe) { usb_log("[KIRA USB] hub: cannot open status endpoint\n"); d->is_hub = 0; return -1; }
    for (int p = 1; p <= h->nports; p++) set_feature(h, p, F_PORT_POWER);
    usb_sleep_ms((uint32_t)hd[5] * 2 + 20);                            /* питание портов установилось */
    h->used = 1;
    h->scan = 1;                                                       /* уже подключённые - сразу */
    usb_log("[KIRA USB] hub with "); usb_log_dec(h->nports); usb_log(" ports ready\n");
    return 0;
}

void hub_poll(void) {
    for (int i = 0; i < USB_MAX_HUB; i++) {
        hub_t* h = &hubs[i];
        if (!h->used) continue;
        uint8_t bits[4] = { 0 };
        int r = h->dev->hc->intr_poll(h->dev->hc, h->pipe, bits, sizeof bits);
        if (r < 0) {                                                   /* хаб перестал отвечать: всё за ним отключено */
            usb_log("[KIRA USB] hub stopped responding\n");
            usb_detach(h->dev);                                        /* родительский хаб забудет его (hub_forget) */
            continue;
        }
        for (int p = 1; p <= h->nports && h->used; p++)
            if (h->scan || (r > 0 && (bits[p / 8] & (1u << (p % 8))))) port_event(h, p);
        h->scan = 0;
    }
}

void hub_detach(usb_dev_t* d) {
    for (int i = 0; i < USB_MAX_HUB; i++)
        if (hubs[i].used && hubs[i].dev == d) {
            for (int p = 1; p <= hubs[i].nports; p++) child_gone(&hubs[i], p);
            if (d->hc->pipe_close) d->hc->pipe_close(d->hc, hubs[i].pipe);
            hubs[i].used = 0;
        }
}

/* Устройство отключено: родительский хаб больше не ссылается на него (ячейка устройства может уйти другому). */
void hub_forget(usb_dev_t* d) {
    for (int i = 0; i < USB_MAX_HUB; i++)
        if (hubs[i].used) for (int p = 1; p <= HUB_PORTS; p++) if (hubs[i].child[p] == d) hubs[i].child[p] = 0;
}
