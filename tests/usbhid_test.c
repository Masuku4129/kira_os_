// USB HID без железа: дескриптор конфигурации, клавиатура (сканкоды набора 1 через декодер PS/2, автоповтор),
// мышь и планшет по дескриптору отчёта (дескрипторы - как у usb-mouse/usb-tablet в QEMU и у мыши с номером отчёта).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbhid.h"
#include "input.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static const uint8_t tablet_rd[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x01, 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00,
    0x26,0xff,0x7f, 0x35,0x00, 0x46,0xff,0x7f, 0x75,0x10, 0x95,0x02, 0x81,0x02, 0x05,0x01, 0x09,0x38, 0x15,0x81,
    0x25,0x7f, 0x35,0x00, 0x45,0x00, 0x75,0x08, 0x95,0x01, 0x81,0x06, 0xc0, 0xc0 };
static const uint8_t mouse_rd[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x01, 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38,
    0x15,0x81, 0x25,0x7f, 0x75,0x08, 0x95,0x03, 0x81,0x06, 0xc0, 0xc0 };
/* мышь с номерами отчётов: сначала отчёт 2 (клавиши мультимедиа), потом 1 - указатель, кнопки 5 шт, X/Y по 12 бит */
static const uint8_t rid_rd[] = {
    0x05,0x0c, 0x09,0x01, 0xa1,0x01, 0x85,0x02, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x10, 0x81,0x02, 0xc0,
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x85,0x01, 0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00,
    0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x03, 0x05,0x01, 0x09,0x30, 0x09,0x31,
    0x16,0x01,0xf8, 0x26,0xff,0x07, 0x75,0x0c, 0x95,0x02, 0x81,0x06, 0xc0, 0xc0 };

static input_event_t feed_all(const uint8_t* b, unsigned n, ps2_decoder_t* d, int* count) {
    input_event_t ev = { 0 }, last = { 0 }; *count = 0;
    for (unsigned i = 0; i < n; i++) if (ps2_decoder_feed(d, b[i], &ev)) { last = ev; (*count)++; }
    return last;
}

int main(void) {
    /* ---- дескриптор конфигурации: клавиатура + планшет, лишние байты и обрыв ---- */
    uint8_t cfg[] = {
        9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
        9, 4, 0, 0, 1, 3, 1, 1, 0,   9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0,   7, 5, 0x81, 3, 8, 0, 10,
        9, 4, 1, 0, 1, 3, 0, 0, 0,   9, 0x21, 0x01, 1, 0, 1, 0x22, 74, 0,   7, 5, 0x82, 3, 6, 0, 4 };
    usb_hid_if_t hi[4]; uint8_t cv;
    CHECK(usb_parse_config(cfg, sizeof cfg, &cv, hi, 4) == 2 && cv == 1);
    CHECK(hi[0].iface == 0 && hi[0].subclass == 1 && hi[0].protocol == 1 && hi[0].ep == 0x81 && hi[0].maxp == 8 && hi[0].interval == 10 && hi[0].report_len == 63);
    CHECK(hi[1].iface == 1 && hi[1].protocol == 0 && hi[1].ep == 0x82 && hi[1].maxp == 6 && hi[1].report_len == 74);
    uint8_t bad[sizeof cfg]; memcpy(bad, cfg, sizeof cfg); bad[18] = 200;          /* испорченная длина */
    CHECK(usb_parse_config(bad, sizeof bad, &cv, hi, 4) <= 1);
    CHECK(usb_parse_config(cfg, 20, &cv, hi, 4) == 0);                               /* обрыв до конечной точки */
    uint8_t rnd[256]; srand(7);
    for (int t = 0; t < 20000; t++) { for (int i = 0; i < 256; i++) rnd[i] = (uint8_t)rand(); rnd[1] = 2; usb_parse_config(rnd, (unsigned)(rand() % 257), &cv, hi, 4); }

    /* ---- клавиатура ---- */
    hid_kbd_t k; hid_kbd_reset(&k);
    ps2_decoder_t d; ps2_decoder_reset(&d);
    uint8_t out[64]; unsigned n; int cnt;
    uint8_t r1[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };                             /* Shift + a */
    n = hid_kbd_report(&k, r1, 8, 0, out, sizeof out);
    input_event_t ev = feed_all(out, n, &d, &cnt);
    CHECK(cnt == 2 && ev.type == INPUT_KEY_DOWN && ev.ch == 'A');
    uint8_t r2[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    n = hid_kbd_report(&k, r2, 8, 10, out, sizeof out);
    ev = feed_all(out, n, &d, &cnt);
    CHECK(cnt == 2 && !(d.mods & INPUT_MOD_SHIFT));
    uint8_t r3[8] = { 0, 0, 0x52, 0, 0, 0, 0, 0 };                                /* стрелка вверх: E0 48 */
    n = hid_kbd_report(&k, r3, 8, 20, out, sizeof out);
    CHECK(n == 2 && out[0] == 0xE0 && out[1] == 0x48);
    ev = feed_all(out, n, &d, &cnt); CHECK(ev.key == KEY_UP);
    CHECK(hid_kbd_tick(&k, 300, out, sizeof out) == 0);                           /* до 500 мс - без повтора */
    n = hid_kbd_tick(&k, 525, out, sizeof out); CHECK(n == 2 && out[1] == 0x48);
    CHECK(hid_kbd_tick(&k, 530, out, sizeof out) == 0);
    CHECK(hid_kbd_tick(&k, 560, out, sizeof out) == 2);
    n = hid_kbd_report(&k, r2, 8, 570, out, sizeof out);                          /* отпустили - повтор прекращается */
    CHECK(n == 2 && out[1] == 0xC8 && hid_kbd_tick(&k, 2000, out, sizeof out) == 0);
    uint8_t ovf[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
    CHECK(hid_kbd_report(&k, ovf, 8, 600, out, sizeof out) == 0);
    uint8_t f11[8] = { 0, 0, 0x44, 0, 0, 0, 0, 0 };
    n = hid_kbd_report(&k, f11, 8, 700, out, sizeof out); ev = feed_all(out, n, &d, &cnt); CHECK(ev.key == KEY_F11);
    uint8_t ent[8] = { 0, 0, 0x28, 0x2A, 0, 0, 0, 0 };                            /* две клавиши сразу: Enter и Backspace */
    n = hid_kbd_report(&k, ent, 8, 800, out, sizeof out);
    ev = feed_all(out, n, &d, &cnt); CHECK(cnt == 3);
    for (unsigned u = 4; u < 0x66; u++) if (u != 0x46 && u != 0x48) CHECK(hid_usage_to_set1((uint8_t)u) != 0);
    uint8_t cyr[8] = { 0, 0, 0x2C, 0, 0, 0, 0, 0 };                               /* пробел */
    hid_kbd_reset(&k); n = hid_kbd_report(&k, cyr, 8, 0, out, sizeof out); ev = feed_all(out, n, &d, &cnt); CHECK(ev.ch == ' ');

    /* ---- планшет QEMU: абсолютные X/Y 0..32767 ---- */
    hid_pointer_t p;
    CHECK(hid_parse_pointer(tablet_rd, sizeof tablet_rd, &p) == 0);
    CHECK(p.abs && p.report_id == 0 && p.x.off == 8 && p.x.bits == 16 && p.y.off == 24 && p.wheel.present && p.btn_count == 3 && p.btn_off == 0);
    uint8_t tr[6] = { 0x01, 0xFF, 0x3F, 0xFF, 0x7F, 0 };
    mouse_event_t me;
    CHECK(hid_pointer_decode(&p, tr, 6, &me) == 1 && me.abs && me.buttons == 1 && me.ax == 16383 && me.ay == 32767);
    /* ---- мышь QEMU: относительные ---- */
    CHECK(hid_parse_pointer(mouse_rd, sizeof mouse_rd, &p) == 0 && !p.abs && p.x.sign && p.x.off == 8 && p.y.off == 16);
    uint8_t mr[4] = { 0x02, 0xFB, 0x07, 0x00 };
    CHECK(hid_pointer_decode(&p, mr, 4, &me) == 1 && !me.abs && me.dx == -5 && me.dy == 7 && me.buttons == 2);
    /* ---- загрузочная мышь ---- */
    hid_boot_mouse_layout(&p);
    uint8_t br[3] = { 0x05, 0x10, 0xF0 };
    CHECK(hid_pointer_decode(&p, br, 3, &me) == 1 && me.dx == 16 && me.dy == -16 && me.buttons == 5);
    /* ---- номера отчётов, 12-битные поля ---- */
    CHECK(hid_parse_pointer(rid_rd, sizeof rid_rd, &p) == 0 && p.report_id == 1 && p.btn_count == 5 && p.x.off == 8 && p.x.bits == 12 && p.y.off == 20);
    uint8_t rr[5] = { 1, 0x01, 0xFE, 0x3F, 0x00 };                               /* X = 0xFFE = -2, Y = 0x003 = 3 */
    CHECK(hid_pointer_decode(&p, rr, 5, &me) == 1 && me.dx == -2 && me.dy == 3 && me.buttons == 1);
    uint8_t other[3] = { 2, 0xFF, 0xFF };
    CHECK(hid_pointer_decode(&p, other, 3, &me) == 0);                           /* чужой номер отчёта */
    CHECK(hid_parse_pointer(cfg, sizeof cfg, &p) != 0);                         /* не дескриптор отчёта */
    for (int t = 0; t < 20000; t++) { unsigned L = (unsigned)(rand() % 200); for (unsigned i = 0; i < L; i++) rnd[i] = (uint8_t)rand();
        if (hid_parse_pointer(rnd, L, &p) == 0) hid_pointer_decode(&p, rnd, 16, &me); }
    printf("usbhid: config descriptor (incl. corrupt/fuzz), keyboard -> set 1 (modifiers, E0, rollover, autorepeat), tablet, mouse, boot mouse, report IDs, fuzz passed\n");
    return 0;
}
