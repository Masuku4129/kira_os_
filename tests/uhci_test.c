// Драйвер UHCI против модели контроллера: регистры в портах, расписание в памяти (список кадров -> QH -> TD),
// каждую «миллисекунду» модель обходит расписание как настоящий контроллер. На портах - клавиатура (low-speed)
// и планшет QEMU, затем вместо планшета - FS-хаб с флешкой и LS-клавиатурой. Проверяются: сброс и пуск, сброс
// портов, адресация (в том числе за хабом), дескрипторы, настройка HID, отчёты, чередование DATA0/DATA1,
// массовые передачи (цепочки TD, остановка на коротком пакете), флешка в /usb0, отключение.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "usb.h"
#include "pci.h"
#include "input.h"
#include "vfs.h"
#include "usbdev_model.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

/* ---- память ниже 4 ГБ: адрес = «физический» ---- */
static uint8_t* arena; static size_t arena_used;
void* usb_dma_alloc(uint32_t b, uint64_t* phys, int lo) {
    (void)lo;
    size_t n = ((size_t)b + 4095) & ~(size_t)4095;
    if (arena_used + n > (16u << 20)) return 0;
    void* p = arena + arena_used; arena_used += n; memset(p, 0, n);
    *phys = (uint64_t)(uintptr_t)p; return p;
}
#define P(a) ((void*)(uintptr_t)(a))

/* ---- модель UHCI ---- */
#define IO 0xC000
static uint16_t cmd, sts, intr, frnum, port[2]; static uint32_t frbase; static uint8_t sof;
static mdev_t devm[2];
static mdev_t* extra[8]; static int nextra;        /* устройства за хабами */
static int frames_run, max_tds_frame;
static mdev_t* root_of(mdev_t* d) { while (d->parent) d = d->parent; return d; }
static mdev_t* dev_for_addr(int a) {
    for (int i = 0; i < 2; i++) if (devm[i].connected && (port[i] & 4) && devm[i].addr == a) return &devm[i];
    for (int i = 0; i < nextra; i++) {
        mdev_t* d = extra[i]; mdev_t* r = root_of(d);
        if (d->addr == a && mdev_behind_ok(d) && r >= devm && r < devm + 2 && (port[r - devm] & 4)) return d;
    }
    return 0;
}

static int run_td(volatile uint32_t* td) {          /* 0 - сделано, 1 - NAK, -1 - ошибка */
    uint32_t cs = td[1], tok = td[2], buf = td[3];
    int pid = tok & 0xFF, addr = (tok >> 8) & 0x7F, ep = (tok >> 15) & 15, tog = (tok >> 19) & 1;
    unsigned maxlen = ((tok >> 21) + 1) & 0x7FF;
    mdev_t* d = dev_for_addr(addr);
    if (!d) { td[1] = (cs & ~(1u << 23) & ~(3u << 27)) | (1u << 18); return -1; }           /* таймаут */
    if (d->low_speed != !!(cs & (1u << 26))) { td[1] = (cs & ~(1u << 23)) | (1u << 18); return -1; }
    int r;
    if (pid == 0x2D) { CHECK(maxlen == 8 && tog == 0); mdev_setup(d, (const uint8_t*)P(buf)); r = 0; }
    else if (pid == 0x69) { uint8_t tmp[1100]; r = mdev_in(d, ep, tmp, maxlen, tog); if (r > 0) memcpy(P(buf), tmp, (size_t)r); }
    else if (pid == 0xE1) { r = mdev_out(d, ep, (const uint8_t*)P(buf), maxlen, ep ? tog : -1); if (r == 0) r = (int)maxlen; }
    else r = M_STALL;
    if (r == M_NAK) { td[1] = cs | (1u << 19); return 1; }
    if (r == M_STALL) { td[1] = (cs & ~(1u << 23)) | (1u << 22); return -1; }
    td[1] = (cs & ~(1u << 23) & ~0x7FFu & ~(1u << 19)) | ((uint32_t)(r - 1) & 0x7FF);
    if (pid == 0x69 && (unsigned)r < maxlen && (cs & (1u << 29))) return 2;   /* короткий пакет при SPD */
    return 0;
}

static void run_frame(void) {
    if (!(cmd & 1)) return;
    volatile uint32_t* fl = (volatile uint32_t*)P(frbase);
    uint32_t ptr = fl[frnum & 1023];
    for (int guard = 0; guard < 300 && !(ptr & 1); guard++) {
        CHECK(ptr & 2);                                 /* в нашем расписании кадр указывает на QH */
        volatile uint32_t* qh = (volatile uint32_t*)P(ptr & ~15u);
        int tds = 0;
        for (int g2 = 0; g2 < 300; g2++) {
            uint32_t e = qh[1];
            if (e & 1) break;
            volatile uint32_t* td = (volatile uint32_t*)P(e & ~15u);
            if (!(td[1] & (1u << 23))) break;          /* неактивный TD - очередь стоит */
            int r = run_td(td); tds++;
            if (r == 2) break;                          /* короткий пакет с SPD: элемент очереди не продвигается */
            if (r != 0) break;                          /* NAK - ждём следующего кадра; ошибка - очередь стоит */
            qh[1] = td[0];
            if (!(td[0] & 4)) break;                    /* без Vf - к следующей очереди */
        }
        if (tds > max_tds_frame) max_tds_frame = tds;
        ptr = qh[0];
    }
    frnum = (uint16_t)((frnum + 1) & 0x7FF);
    frames_run++;
}

static uint32_t now;
void usb_sleep_ms(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { now++; run_frame(); } }
uint32_t usb_now_ms(void) { return now; }
static char logbuf[16384];
void usb_log(const char* s) { strncat(logbuf, s, sizeof logbuf - strlen(logbuf) - 1); }
void usb_log_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); usb_log(b); }
void usb_log_hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); usb_log(b); }

static uint16_t legsup;
void pci_enable(const pci_dev_t* d, uint16_t b) { (void)d; (void)b; }
void pci_write16(const pci_dev_t* d, uint16_t off, uint16_t v) { (void)d; if (off == 0xC0) legsup = v; }

static void port_write(int i, uint16_t v) {
    if (v & 0x200) { port[i] = (uint16_t)((port[i] | 0x200) & ~4); return; }
    if (port[i] & 0x200) {                              /* конец сброса */
        port[i] &= (uint16_t)~0x200;
        devm[i].addr = 0;
    }
    port[i] &= (uint16_t)~(v & 0x000A);                 /* W1C: CSC, PEC */
    if ((v & 4) && devm[i].connected) port[i] |= 4; else if (!(v & 4)) port[i] &= (uint16_t)~4;
}
void outw(uint16_t p, uint16_t v) {
    switch (p - IO) {
    case 0x00: if (v & 2) { cmd = 0; } else cmd = v; sts = (uint16_t)((sts & ~0x20) | ((cmd & 1) ? 0 : 0x20)); break;
    case 0x02: sts &= (uint16_t)~v; if (!(cmd & 1)) sts |= 0x20; break;
    case 0x04: intr = v; break;
    case 0x06: frnum = v; break;
    case 0x10: port_write(0, v); break;
    case 0x12: port_write(1, v); break;
    default: break;
    }
}
uint16_t inw(uint16_t p) {
    switch (p - IO) {
    case 0x00: return cmd;
    case 0x02: return sts;
    case 0x10: case 0x12: { int i = (p - IO - 0x10) / 2; uint16_t v = (uint16_t)(port[i] | 0x80 | (devm[i].connected ? 1 : 0) | (devm[i].low_speed ? 0x100 : 0)); return v; }
    default: return 0xFFFF;                             /* портов 3+ нет */
    }
}
void outl(uint16_t p, uint32_t v) { if (p - IO == 0x08) frbase = v; }
void outb(uint16_t p, uint8_t v) { if (p - IO == 0x0C) sof = v; }

int main(void) {
    arena = mmap((void*)0x10000000, 16u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(arena != MAP_FAILED && (uintptr_t)arena + (16u << 20) < 0x100000000ull);
    input_init();
    mdev_kbd(&devm[0]); mdev_tablet(&devm[1]);
    port[0] = 2; port[1] = 2;                           /* CSC: устройства уже подключены */
    pci_dev_t pd = { 0 }; pd.bar[4].io = 1; pd.bar[4].base = IO; pd.bar[4].size = 32;
    usb_hc_t hc;
    CHECK(uhci_start(&hc, &pd) == 0);
    CHECK(legsup == 0x8F00 && (cmd & 1) && frbase && sof == 0x40 && strstr(logbuf, "2 ports"));
    hc.ports(&hc);
    CHECK(devm[0].addr != 0 && devm[1].addr != 0 && devm[0].addr != devm[1].addr);
    CHECK(devm[0].configured == 1 && devm[0].protocol == 0 && devm[0].idle && devm[1].configured == 1);
    CHECK(usb_hid_count() == 2);
    CHECK(strstr(logbuf, "low-speed") && strstr(logbuf, "keyboard ready") && strstr(logbuf, "absolute"));

    /* отчёты: «ok» с клавиатуры, касание планшета */
    uint8_t r_o[8] = { 0, 0, 0x12 }, r_k[8] = { 0, 0, 0x0E }, r0[8] = { 0 };
    mdev_report(&devm[0], r_o, 8); mdev_report(&devm[0], r0, 8); mdev_report(&devm[0], r_k, 8); mdev_report(&devm[0], r0, 8);
    uint8_t t1[6] = { 1, 0x00, 0x40, 0x00, 0x20, 0 };
    mdev_report(&devm[1], t1, 6);
    for (int i = 0; i < 40; i++) { usb_sleep_ms(2); usb_hid_poll(now); }
    char typed[8] = { 0 }; int nt = 0; input_event_t ev;
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch && nt < 7) typed[nt++] = (char)ev.ch;
    CHECK(!strcmp(typed, "ok"));
    mouse_event_t me;
    CHECK(input_pop_mouse(&me) && me.abs && me.buttons == 1 && me.ax == 16384 && me.ay == 8192);
    CHECK(devm[0].bad_toggle == 0 && devm[1].bad_toggle == 0);

    /* клавиатуру выдернули: порт без устройства, опрос портов снимает её */
    devm[0].connected = 0; port[0] = (uint16_t)((port[0] & ~4) | 2);
    hc.ports(&hc);
    CHECK(usb_hid_count() == 1 && strstr(logbuf, "device removed"));
    /* вставили снова - новая настройка с нуля */
    mdev_kbd(&devm[0]); port[0] |= 2;
    hc.ports(&hc);
    CHECK(usb_hid_count() == 2 && devm[0].configured == 1);

    /* планшет вынули, на его место - FS-хаб: порт 1 - флешка, порт 2 - LS-клавиатура */
    devm[1].connected = 0; port[1] = (uint16_t)((port[1] & ~4) | 2);
    hc.ports(&hc);
    CHECK(usb_hid_count() == 1);
    static mdev_t stick, kbd2;
    uint8_t* img = mstick_image();
    mdev_hub(&devm[1], 0); mdev_stick(&stick, img, MSTICK_SECTORS, 0); mdev_kbd(&kbd2);
    stick.notready = 2;
    extra[nextra++] = &stick; extra[nextra++] = &kbd2;
    mhub_plug(&devm[1], 1, &stick); mhub_plug(&devm[1], 2, &kbd2);
    port[1] |= 2;
    hc.ports(&hc);
    for (int i = 0; i < 20; i++) { usb_sleep_ms(4); usb_hid_poll(now); hub_poll(); msc_poll(); }
    if (!strstr(logbuf, "mounted at /usb0")) printf("%s", logbuf);
    CHECK(hub_count() == 1 && msc_count() == 1 && usb_hid_count() == 2 && strstr(logbuf, "port 2.2: device"));
    CHECK(stick.notready == 0 && stick.cbws > 6);                       /* TUR -> REQUEST SENSE (короткий ответ) -> повтор */
    CHECK(stick.addr && kbd2.addr && stick.addr != kbd2.addr && stick.addr != devm[1].addr && stick.addr != devm[0].addr);
    char buf[32] = { 0 };
    CHECK(vfs_read("/usb0/hello.txt", 0, buf, sizeof buf) == 12 && !strcmp(buf, "privet, kira"));
    static uint8_t big[40000], back[40000];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 13 + 5);
    CHECK(vfs_write("/usb0/big.bin", big, sizeof big) == 0);
    CHECK(vfs_read("/usb0/big.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big));
    CHECK(vfs_read("/usb0/big.bin", 39990, back, 100) == 10);           /* короткое чтение в конце файла */
    /* устройство отдало меньше, чем обещало: короткий пакет посреди цепочки TD - остановка (SPD), CSW читается сразу */
    int br0 = stick.bot_resets; uint32_t t0 = now;
    stick.short_read_once = 1; memset(back, 0, sizeof back);
    CHECK(vfs_read("/usb0/big.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big));
    CHECK(stick.short_read_once == 0 && stick.bot_resets == br0 && now - t0 < 1000);
    CHECK(max_tds_frame >= 64 && stick.bad_toggle == 0 && devm[1].bad_toggle == 0);   /* цепочки TD, DATA0/1 массовых точек */
    uint8_t r_z[8] = { 0, 0, 0x1D };
    mdev_report(&kbd2, r_z, 8); mdev_report(&kbd2, r0, 8);
    for (int i = 0; i < 20; i++) { usb_sleep_ms(2); usb_hid_poll(now); }
    nt = 0; memset(typed, 0, sizeof typed);
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch && nt < 7) typed[nt++] = (char)ev.ch;
    CHECK(!strcmp(typed, "z"));
    /* флешку вынули из хаба */
    mhub_unplug(&devm[1], 1);
    for (int i = 0; i < 5; i++) { usb_sleep_ms(4); hub_poll(); msc_poll(); }
    CHECK(msc_count() == 0 && vfs_read("/usb0/hello.txt", 0, buf, 12) < 0 && strstr(logbuf, "storage removed"));
    printf("uhci: reset/run, port reset, low/full speed, addressing, descriptors, HID setup, reports, DATA0/1, unplug/replug, "
           "FS hub, LS keyboard behind hub, flash stick bulk TD chains (%d TDs/frame) + short packet stop, /usb0 read/write, removal passed (%d frames)\n", max_tds_frame, frames_run);
    return 0;
}
