#include "gfxui.h"
#include "font.h"
#include "theme.h"

static const uint16_t atan_tab[65] = {
    0, 9, 18, 27, 36, 45, 54, 62, 71, 80, 89, 98, 106, 115, 123, 132, 140, 149, 157, 165, 174, 182, 190, 198, 206,
    213, 221, 229, 236, 244, 251, 258, 266, 273, 280, 287, 294, 300, 307, 314, 320, 326, 333, 339, 345, 351, 357,
    363, 369, 374, 380, 386, 391, 396, 402, 407, 412, 417, 422, 427, 432, 436, 441, 445, 450
};

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

uint32_t ui_mix(uint32_t a, uint32_t b, unsigned t) {
    if (t > 256) t = 256;
    unsigned r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    unsigned g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    unsigned bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return gfx_rgb(r, g, bl);
}

void ui_blend(surface_t* s, int x, int y, uint32_t color, unsigned alpha) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || alpha == 0) return;
    uint32_t* p = &s->px[(long)y * s->pitch + x];
    *p = alpha >= 255 ? (color & 0x00FFFFFF) : ui_mix(*p, color, alpha + (alpha >> 7));
}

/* Покрытие 0..16 пикселя (px,py) кругом радиуса r16 (1/16 px) с центром (cx16,cy16). */
static int disc_cov(int px, int py, int cx16, int cy16, int r16) {
    int dx = px * 16 + 8 - cx16, dy = py * 16 + 8 - cy16;
    int d = (int)isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    int c = r16 - d + 8;
    return c < 0 ? 0 : (c > 16 ? 16 : c);
}

/* Покрытие скруглённым прямоугольником: внутри полосы, не затронутой углами, - 16. */
static int rr_cov(int px, int py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int ccx, ccy;
    if (px < x + r) ccx = x + r; else if (px >= x + w - r) ccx = x + w - r; else return 16;
    if (py < y + r) ccy = y + r; else if (py >= y + h - r) ccy = y + h - r; else return 16;
    return disc_cov(px, py, ccx * 16, ccy * 16, r * 16);
}

void ui_round_rect(surface_t* s, int x, int y, int w, int h, int r, uint32_t color, unsigned alpha) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int y0 = y < 0 ? 0 : y, y1 = y + h > s->h ? s->h : y + h;
    int x0 = x < 0 ? 0 : x, x1 = x + w > s->w ? s->w : x + w;
    for (int py = y0; py < y1; py++) {
        int in_corner = (py < y + r) || (py >= y + h - r);
        int ia = x + r, ib = x + w - r;              /* прямой отрезок строки без влияния углов */
        if (!in_corner) { ia = x; ib = x + w; }
        for (int px = x0; px < x1; px++) {
            if (px >= ia && px < ib) {
                if (alpha >= 255) {                  /* быстрый путь: сплошная заливка отрезка */
                    int e = ib < x1 ? ib : x1;
                    uint32_t* row = &s->px[(long)py * s->pitch];
                    for (; px < e; px++) row[px] = color & 0x00FFFFFF;
                    px--;
                    continue;
                }
                ui_blend(s, px, py, color, alpha);
            } else {
                int c = rr_cov(px, py, x, y, w, h, r);
                if (c) ui_blend(s, px, py, color, (unsigned)c * alpha / 16);
            }
        }
    }
}

void ui_round_frame(surface_t* s, int x, int y, int w, int h, int r, uint32_t color, unsigned alpha) {
    if (w < 4 || h < 4) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= s->h) continue;
        int edge_row = (j == 0 || j == h - 1);
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= s->w) continue;
            int near_corner = (j < r || j >= h - r) && (i < r || i >= w - r);
            int c;
            if (near_corner) {
                c = rr_cov(px, py, x, y, w, h, r) - rr_cov(px, py, x + 1, y + 1, w - 2, h - 2, r - 1);
                if (c < 0) c = 0;
            } else if (edge_row || i == 0 || i == w - 1) c = 16;
            else {                                    /* внутренность строки пропускаем до правого угла */
                i = (j < r || j >= h - r) ? w - r - 1 : w - 2;
                continue;
            }
            if (c) ui_blend(s, px, py, color, (unsigned)c * alpha / 16);
        }
    }
}

void ui_disc(surface_t* s, int cx, int cy, int r, uint32_t color, unsigned alpha) {
    ui_ring(s, cx, cy, 0, r, color, alpha);
}

static void ring_pixel(surface_t* s, int px, int py, int cx, int cy, int ri16, int ro16, int r_in, int use_arc,
                       int start10, int sweep10, uint32_t color, unsigned alpha) {
    int dx = px * 16 + 8 - cx * 16, dy = py * 16 + 8 - cy * 16;
    int d = (int)isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    int co = ro16 - d + 8; if (co < 0) co = 0; if (co > 16) co = 16;
    int ci = r_in <= 0 ? 0 : ri16 - d + 8; if (ci < 0) ci = 0; if (ci > 16) ci = 16;
    int c = co - ci;
    if (c <= 0) return;
    if (use_arc) {
        int a = ui_angle10(dx, dy) - start10;
        a %= 3600; if (a < 0) a += 3600;
        if (a >= sweep10) return;
    }
    ui_blend(s, px, py, color, (unsigned)c * alpha / 16);
}

/* Обходит только кольцо (а не весь квадрат): в каждой строке два коротких отрезка по обе стороны от "дырки". */
static void ring_scan(surface_t* s, int cx, int cy, int r_in, int r_out, int use_arc, int start10, int sweep10,
                      uint32_t color, unsigned alpha) {
    int ro16 = r_out * 16, ri16 = r_in * 16;
    int ro = r_out + 1, ri = r_in - 1;                  /* запас в пиксель под сглаживание */
    int y0 = cy - ro, y1 = cy + ro;
    if (y0 < 0) y0 = 0;
    if (y1 >= s->h) y1 = s->h - 1;
    for (int py = y0; py <= y1; py++) {
        int dy = py - cy; if (dy >= 0) dy++;             /* ближайший к центру край пикселя */
        int dyc = dy < 0 ? -dy : dy; dyc -= 1; if (dyc < 0) dyc = 0;
        int outer = (int)isqrt64((uint64_t)((int64_t)ro * ro - (int64_t)dyc * dyc > 0 ? (int64_t)ro * ro - (int64_t)dyc * dyc : 0)) + 1;
        int hole = 0;
        if (ri > 0 && dyc < ri) hole = (int)isqrt64((uint64_t)((int64_t)ri * ri - (int64_t)dyc * dyc));
        int xa = cx - outer, xb = cx + outer;
        if (xa < 0) xa = 0;
        if (xb >= s->w) xb = s->w - 1;
        for (int px = xa; px <= xb; px++) {
            if (hole && px > cx - hole && px < cx + hole - 1) { px = cx + hole - 2; continue; }
            ring_pixel(s, px, py, cx, cy, ri16, ro16, r_in, use_arc, start10, sweep10, color, alpha);
        }
    }
}

void ui_ring(surface_t* s, int cx, int cy, int r_in, int r_out, uint32_t color, unsigned alpha) {
    ring_scan(s, cx, cy, r_in, r_out, 0, 0, 0, color, alpha);
}

void ui_arc(surface_t* s, int cx, int cy, int r_in, int r_out, int start10, int sweep10, uint32_t color) {
    if (sweep10 <= 0) return;
    if (sweep10 >= 3600) { ring_scan(s, cx, cy, r_in, r_out, 0, 0, 0, color, 255); return; }
    ring_scan(s, cx, cy, r_in, r_out, 1, start10, sweep10, color, 255);
}

int ui_angle10(int dx, int dy) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax == 0 && ay == 0) return 0;
    int phi;                                         /* угол от вертикальной оси, 0..900 */
    /* t = tg в 1/4096; ровно 45 градусов (t = 4096) - последний элемент таблицы, без чтения за её концом */
    long t = ay >= ax ? (long)ax * 4096 / ay : (long)ay * 4096 / ax;
    int i = (int)(t >> 6), f = (int)(t & 63);
    int at = i >= 64 ? atan_tab[64] : atan_tab[i] + ((atan_tab[i + 1] - atan_tab[i]) * f) / 64;
    phi = ay >= ax ? at : 900 - at;
    int a;
    if (dx >= 0 && dy < 0) a = phi;
    else if (dx >= 0) a = 1800 - phi;
    else if (dy >= 0) a = 1800 + phi;
    else a = 3600 - phi;
    return a % 3600;
}

void ui_text_bold(surface_t* s, int x, int y, const char* t, uint32_t color) {
    gfx_text(s, x, y, t, color, -1);
    gfx_text(s, x + 1, y, t, color, -1);
}

int ui_text_scaled(surface_t* s, int x, int y, const char* utf8, int scale, uint32_t color) {
    int cx = x;
    while (*utf8) {
        uint32_t cp = utf8_next(&utf8);
        uint32_t tmp[8 * 16];
        for (int i = 0; i < 8 * 16; i++) tmp[i] = 0x01000000;       /* маркер "прозрачно" */
        surface_t g = { tmp, 8, 16, 8 };
        gfx_glyph(&g, 0, 0, cp, color, -1);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if (tmp[j * 8 + i] != 0x01000000)
                    gfx_rect(s, cx + i * scale, y + j * scale, scale, scale, color);
        cx += 8 * scale;
    }
    return cx - x;
}

/* ---- линии, текст, градиенты, свечение (оболочка v23) ---- */

void ui_seg(surface_t* s, int x0, int y0, int x1, int y1, int w16, uint32_t color, unsigned alpha) {
    /* концы в 1/16 пикселя, w16 - полутолщина в 1/16 пикселя; края сглажены (покрытие по расстоянию до отрезка) */
    int minx = (x0 < x1 ? x0 : x1) - w16 - 16, maxx = (x0 > x1 ? x0 : x1) + w16 + 16;
    int miny = (y0 < y1 ? y0 : y1) - w16 - 16, maxy = (y0 > y1 ? y0 : y1) + w16 + 16;
    int px0 = minx >> 4, px1 = (maxx >> 4) + 1, py0 = miny >> 4, py1 = (maxy >> 4) + 1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > s->w) px1 = s->w;
    if (py1 > s->h) py1 = s->h;
    int64_t dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    for (int py = py0; py < py1; py++)
        for (int px = px0; px < px1; px++) {
            int64_t cx = px * 16 + 8 - x0, cy = py * 16 + 8 - y0;
            int64_t t = ll ? (cx * dx + cy * dy) : 0;
            int64_t qx, qy;
            if (t <= 0 || !ll) { qx = cx; qy = cy; }
            else if (t >= ll) { qx = cx - dx; qy = cy - dy; }
            else { qx = cx - dx * t / ll; qy = cy - dy * t / ll; }
            int d = (int)isqrt64((uint64_t)(qx * qx + qy * qy));
            int c = w16 - d + 8;
            if (c <= 0) continue;
            if (c > 16) c = 16;
            ui_blend(s, px, py, color, alpha * (unsigned)c / 16);
        }
}

/* Текст крупнее 8x16 со сглаживанием: глиф выбирается билинейно из битовой маски, scale16 = 16 - исходный размер. */
int ui_text_smooth(surface_t* s, int x, int y, const char* t, int scale16, uint32_t color) {
    int gw = FONT_W * scale16 / 16, gh = FONT_H * scale16 / 16, x0 = x;
    while (*t) {
        uint32_t cp = utf8_next(&t);
        if (cp == '\n') { x = x0; y += gh; continue; }
        const uint8_t* g = font_glyph(cp);
        for (int oy = 0; oy < gh; oy++) {
            int sy16 = ((oy * 2 + 1) * 16 * 16 / scale16 - 16) / 2;      /* координата в глифе, 1/16 */
            int yi = sy16 >> 4, fy = sy16 & 15;
            if (sy16 < 0) { yi = -1; fy = 15; }
            for (int ox = 0; ox < gw; ox++) {
                int sx16 = ((ox * 2 + 1) * 16 * 16 / scale16 - 16) / 2;
                int xi = sx16 >> 4, fx = sx16 & 15;
                if (sx16 < 0) { xi = -1; fx = 15; }
                int v = 0;
                for (int k = 0; k < 4; k++) {
                    int gx = xi + (k & 1), gy = yi + (k >> 1);
                    if (gx < 0 || gy < 0 || gx >= FONT_W || gy >= FONT_H) continue;
                    if (!((g[gy] >> (7 - gx)) & 1)) continue;
                    int wx = (k & 1) ? fx : 16 - fx, wy = (k >> 1) ? fy : 16 - fy;
                    v += wx * wy;
                }
                if (!v) continue;
                /* резче, чем чистая билинейная выборка: середина перехода сохраняется, края уже */
                int a = (v - 64) * 2;
                if (a <= 0) continue;
                if (a > 256) a = 256;
                ui_blend(s, x + ox, y + oy, color, (unsigned)(a > 255 ? 255 : a));
            }
        }
        x += gw;
    }
    return x - x0;
}

int ui_text_smooth_width(const char* t, int scale16) {
    int n = 0;
    while (*t) { uint32_t cp = utf8_next(&t); if (cp != '\n') n++; }
    return n * (FONT_W * scale16 / 16);
}

void ui_vgrad(surface_t* s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) gfx_rect(s, x, y + j, w, 1, ui_mix(top, bottom, (unsigned)(h > 1 ? j * 256 / (h - 1) : 0)));
}

/* Мягкое свечение вокруг скруглённого прямоугольника: несколько колец с затухающей прозрачностью. */
void ui_glow(surface_t* s, int x, int y, int w, int h, int r, int size, uint32_t color, unsigned alpha) {
    for (int k = size; k >= 1; k--) {
        unsigned a = alpha * (unsigned)(size - k + 1) / (unsigned)(size + 1) / 2;
        ui_round_frame(s, x - k, y - k, w + 2 * k, h + 2 * k, r + k, color, a);
    }
}

/* ---------------- единая тема приложений ---------------- */

void ui_text_fit(surface_t* s, int x, int y, const char* t, int maxw, uint32_t col) {
    int maxc = maxw / FONT_W;
    if (maxc <= 0) return;
    int n = 0;
    for (const char* p = t; *p; ) { utf8_next(&p); n++; }
    if (n <= maxc) { gfx_text(s, x, y, t, col, -1); return; }
    char buf[200]; int b = 0, k = 0;
    const char* p = t;
    while (*p && k < maxc - 1 && b < 190) {
        const char* q = p; utf8_next(&q);
        while (p < q) buf[b++] = *p++;
        k++;
    }
    buf[b++] = (char)0xE2; buf[b++] = (char)0x80; buf[b++] = (char)0xA6; buf[b] = 0;       /* «…» */
    gfx_text(s, x, y, buf, col, -1);
}

void ui_card(surface_t* s, int x, int y, int w, int h, const char* title, uint32_t accent) {
    ui_round_rect(s, x, y, w, h, TH_RADIUS, TH_CARD, 255);
    ui_round_frame(s, x, y, w, h, TH_RADIUS, TH_CARD_EDGE, 255);
    if (title) {
        gfx_rect(s, x + 12, y + 11, 3, 12, accent);
        gfx_text(s, x + 21, y + 9, title, TH_TEXT_FAINT, -1);
        if (h > 44 && w > 40) ui_vgrad(s, x + 12, y + 29, w - 24, 1, accent, TH_CARD_EDGE);
    }
}

void ui_button(surface_t* s, int x, int y, int w, int h, const char* label, uint32_t color, int hover, int on) {
    ui_round_rect(s, x, y, w, h, 8, on ? ui_mix(TH_CARD, color, 70) : hover ? ui_mix(TH_CARD, color, 45) : TH_TRACK, 255);
    ui_round_frame(s, x, y, w, h, 8, on ? color : hover ? ui_mix(TH_CARD_EDGE, color, 140) : TH_CARD_EDGE, 255);
    int tw = gfx_text_width(label);
    gfx_text(s, x + (w - tw) / 2, y + (h - 16) / 2, label, on || hover ? TH_TEXT : TH_TEXT_DIM, -1);
}

void ui_statusbar(surface_t* s, int y, const char* left, uint32_t lc, const char* right) {
    ui_vgrad(s, 0, y, s->w, TH_BAR_H, TH_BAR0, TH_BAR1);
    gfx_rect(s, 0, y, s->w, 1, TH_CARD_EDGE);
    int rw = right && right[0] ? gfx_text_width(right) : 0;
    if (left && left[0]) ui_text_fit(s, 12, y + 4, left, s->w - 36 - rw, lc);
    if (rw) gfx_text(s, s->w - 12 - rw, y + 4, right, TH_TEXT_FAINT, -1);
}

static void keybar_cell(int width, int n, int i, int* x, int* w) {
    *x = i * width / n; *w = (i + 1) * width / n - *x;
}

int ui_keybar_hit(int width, int n, int x) {
    if (n <= 0 || x < 0 || x >= width) return -1;
    for (int i = 0; i < n; i++) { int cx, cw; keybar_cell(width, n, i, &cx, &cw); if (x >= cx && x < cx + cw) return i; }
    return -1;
}

void ui_keybar(surface_t* s, int y, const ui_key_t* k, int n, int hover) {
    gfx_rect(s, 0, y, s->w, TH_BAR_H, TH_BAR1);
    for (int i = 0; i < n; i++) {
        int cx, cw; keybar_cell(s->w, n, i, &cx, &cw);
        int has = k[i].label && k[i].label[0];
        int kw = gfx_text_width(k[i].key) + 8;
        uint32_t c = has ? k[i].color : TH_TEXT_FAINT;
        ui_round_rect(s, cx + 3, y + 3, kw, TH_BAR_H - 6, 4, hover == i && has ? c : ui_mix(TH_TRACK, c, has ? 90 : 20), 255);
        gfx_text(s, cx + 7, y + 4, k[i].key, hover == i && has ? TH_BAR1 : has ? TH_TEXT : TH_TEXT_FAINT, -1);
        if (has) ui_text_fit(s, cx + kw + 7, y + 4, k[i].label, cw - kw - 9, hover == i ? TH_TEXT : TH_TEXT_DIM);
    }
}
