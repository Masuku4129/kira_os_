/* virtio-input: клавиатура, мышь и планшет (абсолютное положение) по PCI - ввод QEMU virt на ARM64 (Limbo ARM) и
   любой машины с virtio. События evdev (тип, код, значение) переводятся в общий слой ввода: клавиши - в сканкоды
   набора 1 (тот же декодер, что у PS/2 и USB), указатель - в mouse_event_t. Очередь событий: все буферы отданы
   устройству заранее, поток драйвера забирает заполненные и сразу возвращает их. Прерывание MSI-X (на ARM - GICv2m/ITS)
   будит поток; если хотя бы у одного устройства его нет - опрос каждый тик. */
#include "virtio.h"
#include "virtio_input.h"
#include "input.h"
#include "sched.h"
#include "uart.h"
#include "msi.h"

#define VIRTIO_INPUT_DEVICE 0x1052          /* 0x1040 + 18 */
#define MAX_DEV 4
#define NBUF 64

enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3 };
enum { CFG_SELECT = 0, CFG_SUBSEL = 1, CFG_SIZE = 2, CFG_DATA = 8, CFG_ID_NAME = 1, CFG_EV_BITS = 0x11, CFG_ABS_INFO = 0x12 };

typedef struct { uint16_t type, code; uint32_t value; } vev_t;

typedef struct {
    virtio_dev_t dev;
    virtq_t q;
    vev_t* ev; uint64_t ev_phys;
    int32_t amin[2], amax[2];               /* пределы осей X/Y планшета */
    int has_abs;
    /* накопленное до EV_SYN */
    int dx, dy, ax, ay, moved, abs_moved;
    uint8_t buttons;
    char name[32];
} vin_t;

static vin_t devs[MAX_DEV];
static int ndev, nirq;
static irqwait_t iw = IRQWAIT_INIT;
static void irq(void* ctx) { (void)ctx; irqwait_fire(&iw); }

/* Код клавиши evdev -> сканкод набора 1 (0 - клавиши нет в наборе). Коды 1..88 совпадают с набором 1;
   расширенные (стрелки, правые Ctrl/Alt и т.п.) - с префиксом E0. */
static uint16_t evdev_to_set1(uint16_t code) {
    if (code >= 1 && code <= 83) return code;
    switch (code) {
    case 87: return 0x57; case 88: return 0x58;                     /* F11, F12 */
    case 96: return 0xE01C; case 97: return 0xE01D; case 98: return 0xE035; case 100: return 0xE038;
    case 102: return 0xE047; case 103: return 0xE048; case 104: return 0xE049; case 105: return 0xE04B;
    case 106: return 0xE04D; case 107: return 0xE04F; case 108: return 0xE050; case 109: return 0xE051;
    case 110: return 0xE052; case 111: return 0xE053; case 125: return 0xE05B; case 126: return 0xE05C;
    }
    return 0;
}

static void key(uint16_t code, uint32_t value) {
    uint16_t sc = evdev_to_set1(code);
    if (!sc) return;
    uint8_t mk = (uint8_t)sc;
    if (value == 0) mk |= 0x80;                                     /* отпускание */
    if (sc >> 8) input_push_scancode_from(INPUT_SRC_VIRTIO, 0xE0);
    input_push_scancode_from(INPUT_SRC_VIRTIO, mk);                 /* 1 - нажатие, 2 - автоповтор (то же нажатие) */
}

static uint16_t scale_axis(const vin_t* d, int a, int v) {
    int32_t lo = d->amin[a], hi = d->amax[a];
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint16_t)((int64_t)(v - lo) * 32767 / (hi - lo));
}

static void event(vin_t* d, const vev_t* e) {
    switch (e->type) {
    case EV_KEY:
        if (e->code >= 0x110 && e->code <= 0x112) {                 /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE */
            uint8_t bit = (uint8_t)(e->code == 0x110 ? 1 : e->code == 0x111 ? 2 : 4);
            d->buttons = (uint8_t)(e->value ? d->buttons | bit : d->buttons & ~bit);
            d->moved = 1;
        } else if (e->code == 0x14A) {                              /* BTN_TOUCH: касание = левая кнопка */
            d->buttons = (uint8_t)(e->value ? d->buttons | 1 : d->buttons & ~1);
            d->moved = 1;
        } else if (e->code < 0x100) key(e->code, e->value);
        break;
    case EV_REL:
        if (e->code == 0) { d->dx += (int32_t)e->value; d->moved = 1; }
        else if (e->code == 1) { d->dy += (int32_t)e->value; d->moved = 1; }
        break;
    case EV_ABS:
        if (e->code == 0) { d->ax = (int32_t)e->value; d->abs_moved = 1; }
        else if (e->code == 1) { d->ay = (int32_t)e->value; d->abs_moved = 1; }
        break;
    case EV_SYN:
        if (d->abs_moved) {
            mouse_event_t m = { 0, 0, d->buttons, 1, scale_axis(d, 0, d->ax), scale_axis(d, 1, d->ay) };
            input_push_mouse(&m);
        } else if (d->moved) {
            int dx = d->dx, dy = d->dy;
            if (dx > 32767) dx = 32767;
            if (dx < -32768) dx = -32768;
            if (dy > 32767) dy = 32767;
            if (dy < -32768) dy = -32768;
            mouse_event_t m = { (int16_t)dx, (int16_t)dy, d->buttons, 0, 0, 0 };
            input_push_mouse(&m);
        }
        d->dx = d->dy = d->moved = d->abs_moved = 0;
        break;
    }
}

static void poll_main(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t mark = irqwait_mark(&iw);
        for (int i = 0; i < ndev; i++) {
            vin_t* d = &devs[i];
            uint16_t id; uint32_t len; int got = 0;
            while (virtq_take(&d->q, &id, &len)) {
                if (id < NBUF &&
                    virtio_dma_sync_for_cpu(d->ev + id, d->ev_phys + id * sizeof(vev_t),
                                            sizeof(vev_t)) == 0) {
                    if (len >= sizeof(vev_t)) event(d, &d->ev[id]);
                    if (virtio_dma_sync_for_device(d->ev + id,
                            d->ev_phys + id * sizeof(vev_t), sizeof(vev_t)) == 0)
                        virtq_post(&d->q, id, d->ev_phys + id * sizeof(vev_t), sizeof(vev_t), 1);
                }
                got = 1;
            }
            if (got) virtq_kick(&d->q);
        }
        if (nirq == ndev) irqwait_wait(&iw, mark, 100);        /* все на прерываниях: сон до события */
        else sched_sleep_ticks(1);
    }
}

static uint8_t cfg_query(vin_t* d, uint8_t sel, uint8_t sub) {
    MMIO_W8(d->dev.devcfg + CFG_SELECT, sel);
    MMIO_W8(d->dev.devcfg + CFG_SUBSEL, sub);
    return MMIO_R8(d->dev.devcfg + CFG_SIZE);
}
static uint32_t cfg_u32(vin_t* d, unsigned off) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | MMIO_R8(d->dev.devcfg + CFG_DATA + off + (unsigned)i);
    return v;
}

static void detach(vin_t* d) {
    if (!d) return;
    if (d->dev.common) {
        virtio_fail(&d->dev);
        if (virtio_reset_device(&d->dev) != 0) return;
    }
    if (d->ev && virtio_dma_free(d->ev, d->ev_phys, NBUF * sizeof(vev_t)) == 0) {
        d->ev = 0; d->ev_phys = 0;
    }
    if (d->q.mem) (void)virtio_queue_release(&d->q);
    if (!d->ev && !d->q.mem) {
        d->dev = (virtio_dev_t){0};
        d->q = (virtq_t){0};
    }
}

static int attach(vin_t* d, const pci_dev_t* p, int* has_irq) {
    if (!d || d->dev.common || d->q.mem || d->ev) return -3;
    if (virtio_attach(&d->dev, p) || !d->dev.devcfg || virtio_begin(&d->dev, 0)) {
        detach(d);
        return -1;
    }
    uint64_t ma; uint32_t md;
    *has_irq = msi_alloc(p, irq, 0, &ma, &md) == 0 && pci_msix_setup(p, 0, ma, md) == 0;
    if (*has_irq) d->dev.msix_vec = 0;
    if (virtio_queue_setup(&d->dev, &d->q, 0, NBUF)) { detach(d); return -1; }
    unsigned n = cfg_query(d, CFG_ID_NAME, 0);
    if (n > sizeof d->name - 1) n = sizeof d->name - 1;
    for (unsigned i = 0; i < n; i++) d->name[i] = (char)MMIO_R8(d->dev.devcfg + CFG_DATA + i);
    d->name[n] = 0;
    d->has_abs = cfg_query(d, CFG_EV_BITS, EV_ABS) > 0;
    for (int a = 0; a < 2 && d->has_abs; a++) {
        if (cfg_query(d, CFG_ABS_INFO, (uint8_t)a) >= 8) { d->amin[a] = (int32_t)cfg_u32(d, 0); d->amax[a] = (int32_t)cfg_u32(d, 4); }
        else { d->amin[a] = 0; d->amax[a] = 32767; }
    }
    d->ev = (vev_t*)virtio_dma_alloc(NBUF * sizeof(vev_t), &d->ev_phys);
    if (!d->ev) { detach(d); return -2; }
    uint16_t nb = d->q.size < NBUF ? d->q.size : NBUF;
    for (uint16_t i = 0; i < nb; i++) virtq_post(&d->q, i, d->ev_phys + i * sizeof(vev_t), sizeof(vev_t), 1);
    virtio_driver_ok(&d->dev);
    virtq_kick(&d->q);
    return 0;
}

int virtio_input_init(void) {
    for (int i = 0; i < pci_count() && ndev < MAX_DEV; i++) {
        const pci_dev_t* p = pci_get(i);
        if (p->vendor != VIRTIO_VENDOR || p->device != VIRTIO_INPUT_DEVICE) continue;
        vin_t* d = &devs[ndev];
        int hi = 0;
        if (attach(d, p, &hi) != 0) {
            uart_print("[KIRA INPUT] virtio-input device did not start.\n");
            if (d->dev.common || d->q.mem || d->ev) break; /* reset unconfirmed: retain and never reuse this slot */
            continue;
        }
        uart_print("[KIRA INPUT] virtio-input: "); uart_print(d->name[0] ? d->name : "(no name)");
        uart_print(d->has_abs ? " (absolute pointer)" : "");
        uart_print(hi ? ", MSI-X interrupt\n" : ", polled\n");
        ndev++; nirq += hi;
    }
    if (!ndev) return 0;
    if (!sched_create("vinput", poll_main, 0, PRIO_CRITICAL)) {
        uart_print("[KIRA INPUT] cannot create virtio-input thread: input from these devices is off.\n");
        for (int i = 0; i < ndev; i++) detach(&devs[i]);
        ndev = 0; nirq = 0;
        return 0;
    }
    return ndev;
}
