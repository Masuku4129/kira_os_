#include "gfx2d.h"
#include "font.h"

#if defined(__x86_64__)
void gfx_copy64(void* dst, const void* src, uint64_t n) {
    __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}
void gfx_fill64(void* dst, uint64_t pattern, uint64_t n) {
    __asm__ volatile ("rep stosq" : "+D"(dst), "+c"(n) : "a"(pattern) : "memory");
}
#else
/* Без строковых команд x86: по 8 байт (поверхности выровнены по 8). */
void gfx_copy64(void* dst, const void* src, uint64_t n) {
    uint64_t* d = (uint64_t*)dst; const uint64_t* s = (const uint64_t*)src;
    while (n--) *d++ = *s++;
}
void gfx_fill64(void* dst, uint64_t pattern, uint64_t n) {
    uint64_t* d = (uint64_t*)dst;
    while (n--) *d++ = pattern;
}
#endif

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void gfx_fill(surface_t* s, uint32_t color) {
    uint64_t pat = ((uint64_t)color << 32) | color;
    if (s->pitch == s->w && (s->w & 1) == 0) {
        gfx_fill64(s->px, pat, ((uint64_t)s->w * (uint64_t)s->h) / 2);
        return;
    }
    gfx_rect(s, 0, 0, s->w, s->h, color);
}

void gfx_pixel(surface_t* s, int x, int y, uint32_t color) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[(long)y * s->pitch + x] = color;
}

void gfx_hline(surface_t* s, int x, int y, int w, uint32_t color) {
    if ((unsigned)y >= (unsigned)s->h) return;
    int x0 = imax(x, 0), x1 = imin(x + w, s->w);
    uint32_t* row = s->px + (long)y * s->pitch;
    for (int i = x0; i < x1; i++) row[i] = color;
}

void gfx_rect(surface_t* s, int x, int y, int w, int h, uint32_t color) {
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, s->w), y1 = imin(y + h, s->h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        uint32_t* row = s->px + (long)j * s->pitch;
        for (int i = x0; i < x1; i++) row[i] = color;
    }
}

void gfx_frame(surface_t* s, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_hline(s, x, y, w, color);
    gfx_hline(s, x, y + h - 1, w, color);
    gfx_rect(s, x, y, 1, h, color);
    gfx_rect(s, x + w - 1, y, 1, h, color);
}

void gfx_line(surface_t* s, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1; // dy <= 0
    int err = dx + dy;
    for (;;) {
        gfx_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_blit(surface_t* dst, int dx, int dy, const surface_t* src, int sx, int sy, int w, int h) {
    // обрезка по источнику и приёмнику
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    w = imin(w, imin(src->w - sx, dst->w - dx));
    h = imin(h, imin(src->h - sy, dst->h - dy));
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t* sp = src->px + (long)(sy + j) * src->pitch + sx;
        uint32_t* dp = dst->px + (long)(dy + j) * dst->pitch + dx;
        if (w >= 4) { gfx_copy64(dp, sp, (uint64_t)(w / 2)); if (w & 1) dp[w - 1] = sp[w - 1]; }   /* rep movsq: на эмуляции в разы быстрее цикла */
        else for (int i = 0; i < w; i++) dp[i] = sp[i];
    }
}

void gfx_blit_alpha(surface_t* dst, int dx, int dy, const surface_t* src, int sx, int sy, int w, int h) {
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    w = imin(w, imin(src->w - sx, dst->w - dx));
    h = imin(h, imin(src->h - sy, dst->h - dy));
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t* sp = src->px + (long)(sy + j) * src->pitch + sx;
        uint32_t* dp = dst->px + (long)(dy + j) * dst->pitch + dx;
        for (int i = 0; i < w; i++) {
            uint32_t s = sp[i], d = dp[i];
            uint32_t a = s >> 24;
            if (a == 255) { dp[i] = s & 0x00FFFFFF; continue; }
            if (a == 0) continue;
            uint32_t ia = 255 - a;
            // (c*a + d*(255-a) + 127) / 255, точно: x/255 = (x + 128 + ((x + 128) >> 8)) >> 8
            uint32_t r = ((s >> 16) & 255) * a + ((d >> 16) & 255) * ia;
            uint32_t g = ((s >> 8) & 255) * a + ((d >> 8) & 255) * ia;
            uint32_t b = (s & 255) * a + (d & 255) * ia;
            r = (r + 128 + ((r + 128) >> 8)) >> 8;
            g = (g + 128 + ((g + 128) >> 8)) >> 8;
            b = (b + 128 + ((b + 128) >> 8)) >> 8;
            dp[i] = (r << 16) | (g << 8) | b;
        }
    }
}

uint32_t utf8_next(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    uint32_t c = s[0];
    if (c == 0) return 0;                    // конец строки: указатель не двигаем
    if (c < 0x80) { *p += 1; return c; }
    int len; uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else { *p += 1; return 0xFFFD; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; } // обрыв последовательности
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

void gfx_glyph(surface_t* s, int x, int y, uint32_t cp, uint32_t fg, int64_t bg) {
    if (x >= s->w || y >= s->h || x + FONT_W <= 0 || y + FONT_H <= 0) return;
    const uint8_t* g = font_glyph(cp);
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if ((unsigned)py >= (unsigned)s->h) continue;
        uint32_t* line = s->px + (long)py * s->pitch;
        uint8_t bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if ((unsigned)px >= (unsigned)s->w) continue;
            if (bits & (0x80 >> col)) line[px] = fg;
            else if (bg >= 0) line[px] = (uint32_t)bg;
        }
    }
}

int gfx_text(surface_t* s, int x, int y, const char* str, uint32_t fg, int64_t bg) {
    int cx = x, maxw = 0;
    uint32_t cp;
    while ((cp = utf8_next(&str)) != 0) {
        if (cp == '\n') { if (cx - x > maxw) maxw = cx - x; cx = x; y += FONT_H; continue; }
        gfx_glyph(s, cx, y, cp, fg, bg);
        cx += FONT_W;
    }
    return imax(maxw, cx - x);
}

int gfx_text_width(const char* str) {
    int cx = 0, maxw = 0;
    uint32_t cp;
    while ((cp = utf8_next(&str)) != 0) {
        if (cp == '\n') { if (cx > maxw) maxw = cx; cx = 0; continue; }
        cx += FONT_W;
    }
    return imax(maxw, cx);
}
