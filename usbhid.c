#include "usbhid.h"

/* ---------------- дескриптор конфигурации ---------------- */

int usb_parse_config(const uint8_t* b, unsigned len, uint8_t* cfg, usb_hid_if_t* out, int max) {
    int n = 0;
    *cfg = 0;
    if (len < 9 || b[1] != USB_DT_CONFIG) return 0;
    unsigned total = (unsigned)b[2] | (unsigned)b[3] << 8;
    if (total < len) len = total;
    *cfg = b[5];
    usb_hid_if_t cur; int in_hid = 0, have_ep = 0;
    for (unsigned p = 0; p + 2 <= len; ) {
        unsigned dl = b[p];
        if (dl < 2 || p + dl > len) break;
        uint8_t type = b[p + 1];
        if (type == USB_DT_INTERFACE && dl >= 9) {
            if (in_hid && have_ep && n < max) out[n++] = cur;
            in_hid = b[p + 5] == 3;
            have_ep = 0;
            cur = (usb_hid_if_t){ b[p + 2], b[p + 6], b[p + 7], 0, 0, 0, 0 };
        } else if (type == USB_DT_HID && dl >= 9 && in_hid) {
            for (unsigned k = 0; k < b[p + 5] && 6 + k * 3 + 2 < dl; k++)
                if (b[p + 6 + k * 3] == USB_DT_REPORT) cur.report_len = (uint16_t)(b[p + 7 + k * 3] | b[p + 8 + k * 3] << 8);
        } else if (type == USB_DT_ENDPOINT && dl >= 7 && in_hid && !have_ep) {
            uint8_t addr = b[p + 2], attr = b[p + 3];
            if ((addr & 0x80) && (attr & 3) == 3) {                       /* прерывающая IN */
                cur.ep = addr; cur.maxp = (uint16_t)((b[p + 4] | b[p + 5] << 8) & 0x7FF); cur.interval = b[p + 6];
                have_ep = 1;
            }
        }
        p += dl;
    }
    if (in_hid && have_ep && n < max) out[n++] = cur;
    return n;
}

int usb_parse_ifaces(const uint8_t* b, unsigned len, uint8_t* cfg, usb_iface_t* out, int max) {
    int n = 0, cur = -1;
    *cfg = 0;
    if (len < 9 || b[1] != USB_DT_CONFIG) return 0;
    unsigned total = (unsigned)b[2] | (unsigned)b[3] << 8;
    if (total < len) len = total;
    *cfg = b[5];
    for (unsigned p = 0; p + 2 <= len; ) {
        unsigned dl = b[p];
        if (dl < 2 || p + dl > len) break;
        uint8_t type = b[p + 1];
        if (type == USB_DT_INTERFACE && dl >= 9) {
            cur = -1;
            if (b[p + 3] == 0 && n < max) {                              /* только альтернатива 0 */
                cur = n++;
                out[cur] = (usb_iface_t){ b[p + 2], b[p + 5], b[p + 6], b[p + 7], 0, 0, { { 0, 0, 0, 0 } } };
            }
        } else if (type == USB_DT_HID && dl >= 9 && cur >= 0) {
            for (unsigned k = 0; k < b[p + 5] && 6 + k * 3 + 2 < dl; k++)
                if (b[p + 6 + k * 3] == USB_DT_REPORT) out[cur].report_len = (uint16_t)(b[p + 7 + k * 3] | b[p + 8 + k * 3] << 8);
        } else if (type == USB_DT_ENDPOINT && dl >= 7 && cur >= 0 && out[cur].nep < 4) {
            usb_iface_t* f = &out[cur];
            f->ep[f->nep].addr = b[p + 2]; f->ep[f->nep].attr = b[p + 3];
            f->ep[f->nep].maxp = (uint16_t)((b[p + 4] | b[p + 5] << 8) & 0x7FF); f->ep[f->nep].interval = b[p + 6];
            f->nep++;
        }
        p += dl;
    }
    return n;
}

/* ---------------- клавиатура ---------------- */

#define E0 0x100
static const uint16_t set1[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,
    [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,
    [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34,
    [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x47] = 0x46,
    [0x49] = E0 | 0x52, [0x4A] = E0 | 0x47, [0x4B] = E0 | 0x49, [0x4C] = E0 | 0x53, [0x4D] = E0 | 0x4F, [0x4E] = E0 | 0x51,
    [0x4F] = E0 | 0x4D, [0x50] = E0 | 0x4B, [0x51] = E0 | 0x50, [0x52] = E0 | 0x48,
    [0x53] = 0x45, [0x54] = E0 | 0x35, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = E0 | 0x1C,
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48,
    [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = E0 | 0x5D,
};
static const uint16_t modcode[8] = { 0x1D, 0x2A, 0x38, E0 | 0x5B, E0 | 0x1D, 0x36, E0 | 0x38, E0 | 0x5C };

uint16_t hid_usage_to_set1(uint8_t u) { return u < 0x66 ? set1[u] : 0; }

static unsigned put(uint16_t code, int up, uint8_t* out, unsigned n, unsigned cap) {
    if (!code) return n;
    if (code & E0) { if (n < cap) out[n++] = 0xE0; }
    if (n < cap) out[n++] = (uint8_t)((code & 0x7F) | (up ? 0x80 : 0));
    return n;
}

void hid_kbd_reset(hid_kbd_t* k) { for (int i = 0; i < 8; i++) k->prev[i] = 0; k->rep_usage = 0; k->rep_at = 0; }

static int has(const uint8_t* r, uint8_t u) { for (int i = 2; i < 8; i++) if (r[i] == u) return 1; return 0; }

unsigned hid_kbd_report(hid_kbd_t* k, const uint8_t* r, unsigned len, uint32_t now, uint8_t* out, unsigned cap) {
    if (len < 8) return 0;
    if (r[2] == 0x01 && r[3] == 0x01) return 0;                    /* переполнение: состояние неизвестно, ждём следующий */
    unsigned n = 0;
    uint8_t ch = r[0] ^ k->prev[0];
    for (int b = 0; b < 8; b++) if (ch & (1u << b)) n = put(modcode[b], !(r[0] & (1u << b)), out, n, cap);
    for (int i = 2; i < 8; i++) {                                  /* отпущенные */
        uint8_t u = k->prev[i];
        if (u > 3 && !has(r, u)) { n = put(hid_usage_to_set1(u), 1, out, n, cap); if (u == k->rep_usage) k->rep_usage = 0; }
    }
    for (int i = 2; i < 8; i++) {                                  /* нажатые */
        uint8_t u = r[i];
        if (u > 3 && !has(k->prev, u)) {
            n = put(hid_usage_to_set1(u), 0, out, n, cap);
            uint16_t c = hid_usage_to_set1(u);
            if (c && u != 0x39 && u != 0x53 && u != 0x47) { k->rep_usage = u; k->rep_at = now + HID_REPEAT_DELAY_MS; }
        }
    }
    for (int i = 0; i < 8; i++) k->prev[i] = r[i];
    return n;
}

unsigned hid_kbd_tick(hid_kbd_t* k, uint32_t now, uint8_t* out, unsigned cap) {
    if (!k->rep_usage || (int32_t)(now - k->rep_at) < 0) return 0;
    if ((int32_t)(now - k->rep_at) > 250) k->rep_at = now;         /* долго не звали - без очереди догоняющих повторов */
    k->rep_at += HID_REPEAT_RATE_MS;
    return put(hid_usage_to_set1(k->rep_usage), 0, out, 0, cap);
}

/* ---------------- указатель: разбор дескриптора отчёта ---------------- */

static int32_t sval(const uint8_t* d, unsigned n) {
    if (n == 1) return (int8_t)d[0];
    if (n == 2) return (int16_t)(d[0] | d[1] << 8);
    if (n == 4) return (int32_t)((uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24);
    return 0;
}
static uint32_t uval(const uint8_t* d, unsigned n) {
    uint32_t v = 0; for (unsigned i = 0; i < n; i++) v |= (uint32_t)d[i] << (8 * i); return v;
}

/* Обход дескриптора. want < 0: ищем номер отчёта, в котором впервые встречается X (*rid_x); иначе собираем поля
   только этого отчёта. У каждого номера отчёта свой счётчик битов. */
static int walk(const uint8_t* rd, unsigned len, int want, int* rid_x, hid_pointer_t* p) {
    uint16_t page = 0, rsize = 0, rcount = 0; uint8_t rid = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16]; int nu = 0; uint32_t umin = 0, umax = 0; int have_range = 0;
    uint16_t off[16]; uint8_t ids[16]; int nids = 1; ids[0] = 0; off[0] = 0;
    int slot = 0;
    for (unsigned i = 0; i < len; ) {
        uint8_t h = rd[i];
        if (h == 0xFE) { if (i + 2 >= len) break; i += 3u + rd[i + 1]; continue; }   /* длинный элемент */
        unsigned sz = h & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > len) break;
        const uint8_t* d = rd + i + 1;
        uint8_t type = (h >> 2) & 3, tag = h >> 4;
        i += 1 + sz;
        if (type == 1) {                                            /* глобальные */
            if (tag == 0) page = (uint16_t)uval(d, sz);
            else if (tag == 1) lmin = sval(d, sz);
            else if (tag == 2) { lmax = sval(d, sz); if (lmin >= 0 && lmax < 0) lmax = (int32_t)uval(d, sz); }
            else if (tag == 7) rsize = (uint16_t)uval(d, sz);
            else if (tag == 9) rcount = (uint16_t)uval(d, sz);
            else if (tag == 8) {
                rid = (uint8_t)uval(d, sz);
                for (slot = 0; slot < nids; slot++) if (ids[slot] == rid) break;
                if (slot == nids) { if (nids == 16) return -1; ids[nids] = rid; off[nids] = 0; nids++; }
            }
        } else if (type == 2) {                                     /* локальные */
            uint32_t u = uval(d, sz); if (sz < 4) u |= (uint32_t)page << 16;
            if (tag == 0 && nu < 16) usages[nu++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { umax = u; have_range = 1; }
        } else if (type == 0) {                                     /* главные */
            if (tag == 8) {                                         /* Input */
                uint32_t flags = uval(d, sz);
                int usable = !(flags & 1) && ((flags >> 1) & 1), relative = (flags >> 2) & 1;
                for (unsigned c = 0; c < rcount && usable; c++) {
                    uint32_t u = 0;
                    if (nu) u = usages[c < (unsigned)nu ? c : (unsigned)nu - 1];
                    else if (have_range) u = umin + c <= umax ? umin + c : umax;
                    uint16_t at = (uint16_t)(off[slot] + c * (uint32_t)rsize);
                    if (want < 0) { if (u == 0x00010030u && *rid_x < 0) *rid_x = rid; continue; }
                    if (rid != want) continue;
                    hid_field_t f = { at, rsize, lmin, lmax, 1, (uint8_t)(lmin < 0) };
                    if (u == 0x00010030u && !p->x.present) { p->x = f; p->abs = !relative; }
                    else if (u == 0x00010031u && !p->y.present) p->y = f;
                    else if (u == 0x00010038u && !p->wheel.present) p->wheel = f;
                    else if ((u >> 16) == 9 && rsize == 1 && p->btn_count < 8) {
                        if (!p->btn_count) p->btn_off = at;
                        if (at == p->btn_off + p->btn_count) p->btn_count++;
                    }
                }
                off[slot] = (uint16_t)(off[slot] + (uint32_t)rsize * rcount);
            }
            nu = 0; have_range = 0; umin = umax = 0;                 /* локальные живут до главного элемента */
        }
    }
    return 0;
}

int hid_parse_pointer(const uint8_t* rd, unsigned len, hid_pointer_t* p) {
    *p = (hid_pointer_t){ 0 };
    int rid = -1;
    if (walk(rd, len, -1, &rid, p) != 0 || rid < 0) return -1;
    walk(rd, len, rid, &rid, p);
    p->report_id = (uint8_t)rid;
    return p->x.present && p->y.present ? 0 : -1;
}

void hid_boot_mouse_layout(hid_pointer_t* p) {
    *p = (hid_pointer_t){ 0 };
    p->btn_off = 0; p->btn_count = 3;
    p->x = (hid_field_t){ 8, 8, -127, 127, 1, 1 };
    p->y = (hid_field_t){ 16, 8, -127, 127, 1, 1 };
}

static int32_t field(const hid_field_t* f, const uint8_t* d, unsigned bytes) {
    if (!f->present || f->bits == 0 || f->bits > 32 || (unsigned)(f->off + f->bits + 7) / 8 > bytes) return 0;
    uint64_t v = 0;
    for (unsigned b = 0; b < f->bits; b++) {
        unsigned at = f->off + b;
        if (d[at >> 3] & (1u << (at & 7))) v |= 1ull << b;
    }
    if (f->sign && f->bits < 32 && (v & (1ull << (f->bits - 1)))) v |= ~0ull << f->bits;
    return (int32_t)v;
}

int hid_pointer_decode(const hid_pointer_t* p, const uint8_t* rep, unsigned len, mouse_event_t* ev) {
    if (p->report_id) { if (len < 1 || rep[0] != p->report_id) return 0; rep++; len--; }
    if (!len) return 0;
    uint8_t buttons = 0;
    for (unsigned b = 0; b < p->btn_count && b < 3; b++) {
        unsigned at = p->btn_off + b;
        if (at / 8 < len && (rep[at >> 3] & (1u << (at & 7)))) buttons |= (uint8_t)(1u << b);
    }
    ev->buttons = buttons;
    int32_t x = field(&p->x, rep, len), y = field(&p->y, rep, len);
    if (p->abs) {
        int64_t rx = (int64_t)p->x.lmax - p->x.lmin, ry = (int64_t)p->y.lmax - p->y.lmin;
        if (rx <= 0 || ry <= 0) return 0;
        int64_t ax = ((int64_t)x - p->x.lmin) * 32767 / rx, ay = ((int64_t)y - p->y.lmin) * 32767 / ry;
        ev->abs = 1; ev->ax = (uint16_t)(ax < 0 ? 0 : ax > 32767 ? 32767 : ax); ev->ay = (uint16_t)(ay < 0 ? 0 : ay > 32767 ? 32767 : ay);
        ev->dx = ev->dy = 0;
    } else {
        ev->abs = 0; ev->ax = ev->ay = 0;
        ev->dx = (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
        ev->dy = (int16_t)(y < -32768 ? -32768 : y > 32767 ? 32767 : y);
    }
    return 1;
}
