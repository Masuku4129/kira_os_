#include "wallpaper.h"
#include "wm.h"
#include "gfx2d.h"
#include <stdint.h>

#define WP_W 320
#define WP_H 180
static uint32_t pixels[WP_W * WP_H];
static surface_t image = { pixels, WP_W, WP_H, WP_W };
static int ready;

static uint32_t mix(uint32_t a, uint32_t b, unsigned t) {
    if (t > 256) t = 256;
    unsigned r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    unsigned g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    unsigned bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return gfx_rgb(r, g, bl);
}
static int abs_i(int v) { return v < 0 ? -v : v; }
static uint32_t noise(unsigned x, unsigned y) {
    uint32_t v = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ 0xC2B2AE3Du;
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; v ^= v >> 16;
    return v;
}
static void px(int x, int y, uint32_t c) {
    if ((unsigned)x < WP_W && (unsigned)y < WP_H) pixels[y * WP_W + x] = c;
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > WP_W) w = WP_W - x; if (y + h > WP_H) h = WP_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t* p = pixels + (y + j) * WP_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}
static void line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs_i(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs_i(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e = err * 2;
        if (e >= dy) { err += dy; x0 += sx; }
        if (e <= dx) { err += dx; y0 += sy; }
    }
}
static int peak_y(int x, int center, int half_width, int base, int height) {
    int d = abs_i(x - center);
    return d >= half_width ? base : base - height * (half_width - d) / half_width;
}
static void draw_sky(void) {
    for (int y = 0; y < WP_H; y++) {
        uint32_t c = y < 94 ? mix(0x040B24u, 0x314F9Bu, (unsigned)y * 256 / 94)
                            : mix(0x314F9Bu, 0xF0A4B5u, (unsigned)(y - 94) * 256 / 48);
        rect(0, y, WP_W, 1, c);
    }
    for (unsigned i = 0; i < 95; i++) {
        unsigned h = noise(i * 37u + 3u, i * 19u + 11u);
        int x = (int)(h % WP_W), y = (int)((h >> 12) % 86u);
        if ((h & 7u) == 0) px(x, y, 0x00DDEAFFu);
    }
}
static void draw_planet(void) {
    const int cx = 105, cy = 62, r = 77;
    for (int y = cy - r; y <= cy + r; y++) for (int x = cx - r; x <= cx + r; x++) {
        int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        unsigned t = (unsigned)(dy + r) * 256 / (2 * r);
        uint32_t c = mix(0x071B50u, 0x5CA7E5u, t);
        int band = (y * 7 + (x / 9) * 3) % 31;
        if (band < 4) c = mix(c, 0xB9D9F2u, 35);
        if (d2 > (r - 2) * (r - 2)) c = mix(c, 0xB8DDFBu, 170);
        px(x, y, c);
    }
    for (int y = 0; y < WP_H; y++) for (int x = 0; x < WP_W; x++) {
        int dx = x - 207, dy = y - 39;
        int d2 = dx * dx + dy * dy;
        if (d2 <= 11 * 11) {
            uint32_t c = mix(0x7779B9u, 0xE5C5D5u, (unsigned)(dy + 11) * 256 / 22);
            if (d2 > 9 * 9) c = 0x00A9B5D7u;
            px(x, y, c);
        }
    }
}
static void draw_mountains(void) {
    for (int x = 0; x < WP_W; x++) {
        int y0 = 137;
        int p[] = { peak_y(x, 26, 34, 137, 42), peak_y(x, 82, 47, 137, 50),
                    peak_y(x, 148, 42, 137, 36), peak_y(x, 235, 48, 137, 48),
                    peak_y(x, 290, 34, 137, 31) };
        for (unsigned k = 0; k < sizeof p / sizeof p[0]; k++) if (p[k] < y0) y0 = p[k];
        for (int y = y0; y < 139; y++) {
            unsigned t = (unsigned)(y - y0) * 256 / (unsigned)(139 - y0);
            uint32_t c = mix(0x1B2B5Au, 0x080F2Bu, t);
            if (y == y0 || y == y0 + 1) c = 0x00B9C6E5u;
            if (y > y0 + 4 && (noise((unsigned)x, (unsigned)y) & 15u) == 0) c = mix(c, 0x005B6DA8u, 80);
            px(x, y, c);
        }
    }
}
static void draw_city(void) {
    for (int x = 188; x < 316; ) {
        unsigned h = noise((unsigned)x, 0xCA17u);
        int w = 4 + (int)(h % 9u), bh = 17 + (int)((h >> 9) % 54u);
        int top = 128 - bh;
        rect(x, top, w, bh, 0x000B1D49u);
        rect(x + 1, top, 1, bh, 0x002C5F9Eu);
        if (w > 5) line(x + w / 2, top, x + w / 2, top - 4 - (int)(h % 8u), 0x005CCBFFu);
        for (int yy = top + 5; yy < 126; yy += 7) for (int xx = x + 1; xx < x + w - 1; xx += 3)
            if ((noise((unsigned)xx, (unsigned)yy) & 3u) == 0) px(xx, yy, 0x00A6DDFBu);
        x += w + 2;
    }
    rect(216, 112, 72, 3, 0x003C8DD0u);
    rect(232, 108, 39, 2, 0x0075C9F0u);
    line(252, 70, 252, 111, 0x00A8E7FFu);
}
static void draw_lake(void) {
    for (int y = 137; y < WP_H; y++) {
        unsigned t = (unsigned)(y - 137) * 256 / 43;
        uint32_t c = mix(0x182B5Au, 0x07152Fu, t);
        rect(0, y, WP_W, 1, c);
    }
    for (int x = 0; x < WP_W; x++) {
        unsigned h = noise((unsigned)x, 0x5151u);
        int center = 138 + (int)(h % 34u);
        int width = 1 + (int)((h >> 8) % 5u);
        uint32_t c = (x > 185 && x < 314) ? 0x004D9ED0u : 0x00516FADu;
        for (int y = center; y < WP_H; y += 5 + (int)((h >> 16) % 5u))
            rect(x, y, width, 1, mix(0x07152Fu, c, 130));
    }
    for (int k = 0; k < 42; k++) {
        unsigned h = noise((unsigned)k * 91u, 0xA55Au);
        int x = (int)(h % WP_W), y = 142 + (int)((h >> 10) % 36u);
        rect(x, y, 2 + (int)((h >> 18) % 9u), 1, 0x00749BCBu);
    }
}
static void draw_foreground(void) {
    for (int x = 0; x < WP_W; x++) {
        int y = 164 + (int)(noise((unsigned)x, 0xF00Du) % 13u);
        for (int yy = y; yy < WP_H; yy++) px(x, yy, mix(0x10162Fu, 0x030817u, (unsigned)(yy - y) * 256 / (unsigned)(WP_H - y)));
    }
    for (int k = 0; k < 95; k++) {
        unsigned h = noise((unsigned)k * 13u, 0xBEEFu);
        int x = (int)(h % WP_W), y = 160 + (int)((h >> 9) % 20u);
        px(x, y, (h & 1u) ? 0x00313B68u : 0x005B477Du);
    }
}
void kira_wallpaper_init(void) {
    if (!ready) {
        draw_sky();
        draw_planet();
        draw_mountains();
        draw_city();
        draw_lake();
        draw_foreground();
        ready = 1;
    }
    wm_set_wallpaper(&image);
}
