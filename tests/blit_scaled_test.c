/* Уменьшение/увеличение окна (blit_scaled): быстрая версия обязана давать те же точки, что прямое усреднение по площади
   (эталон ниже - прежняя реализация с делением на каждую точку). Случайные размеры, смещения и обрезка по экрану. */
#include "wm.c"
#include <stdio.h>

static void ref_blit_scaled(surface_t* c, int dx, int dy, int dw, int dh, const surface_t* src) {
    if (dw == src->w && dh == src->h) { gfx_blit(c, dx, dy, src, 0, 0, src->w, src->h); return; }
    if (dw <= 0 || dh <= 0) return;
    int xa = dx < 0 ? 0 : dx, xb = dx + dw > c->w ? c->w : dx + dw;
    int ya = dy < 0 ? 0 : dy, yb = dy + dh > c->h ? c->h : dy + dh;
    if (xa >= xb || ya >= yb) return;
    int sw = src->w, sh = src->h;
    if (dw >= sw && dh >= sh) {                                  /* увеличение: ближайшая точка */
        long n0 = (long)(xa - dx) * sw;
        int q0 = (int)(n0 / dw);
        long r0 = n0 - (long)q0 * dw;
        for (int j = ya; j < yb; j++) {
            const uint32_t* sp = src->px + (long)((j - dy) * sh / dh) * src->pitch;
            uint32_t* dp = &c->px[(long)j * c->pitch];
            int q = q0; long r = r0;
            for (int i = xa; i < xb; i++) {
                dp[i] = sp[q];
                r += sw;
                while (r >= dw) { r -= dw; q++; }
            }
        }
        return;
    }
    for (int j = ya; j < yb; j++) {
        long y0 = (long)(j - dy) * sh, y1 = y0 + sh;              /* строка назначения покрывает [y0, y1) в долях 1/dh строки источника */
        int ry0 = (int)(y0 / dh), ry1 = (int)((y1 - 1) / dh);
        if (ry1 >= sh) ry1 = sh - 1;
        uint32_t* dp = &c->px[(long)j * c->pitch];
        for (int i = xa; i < xb; i++) {
            long x0 = (long)(i - dx) * sw, x1 = x0 + sw;
            int rx0 = (int)(x0 / dw), rx1 = (int)((x1 - 1) / dw);
            if (rx1 >= sw) rx1 = sw - 1;
            uint64_t sr = 0, sg = 0, sb = 0;
            for (int ry = ry0; ry <= ry1; ry++) {
                long ya0 = (long)ry * dh > y0 ? (long)ry * dh : y0;
                long ya1 = (long)(ry + 1) * dh < y1 ? (long)(ry + 1) * dh : y1;
                uint64_t wy = (uint64_t)(ya1 - ya0);
                const uint32_t* sp = src->px + (long)ry * src->pitch;
                for (int rx = rx0; rx <= rx1; rx++) {
                    long xa0 = (long)rx * dw > x0 ? (long)rx * dw : x0;
                    long xa1 = (long)(rx + 1) * dw < x1 ? (long)(rx + 1) * dw : x1;
                    uint64_t wgt = wy * (uint64_t)(xa1 - xa0);
                    uint32_t p = sp[rx];
                    sr += wgt * ((p >> 16) & 255); sg += wgt * ((p >> 8) & 255); sb += wgt * (p & 255);
                }
            }
            uint64_t tot = (uint64_t)sw * (uint64_t)sh;
            dp[i] = (uint32_t)((sr / tot) << 16 | (sg / tot) << 8 | (sb / tot));
        }
    }
}


int main(void) {
    unsigned seed = 2024;
#define RND(n) (seed = seed * 1103515245u + 12345u, (int)((seed >> 8) % (unsigned)(n)))
    long bad = 0, runs = 0;
    for (int it = 0; it < 4000; it++) {
        int sw = 1 + RND(300), sh = 1 + RND(200);
        int dw = 1 + RND(it % 3 == 0 ? 500 : 300), dh = 1 + RND(it % 3 == 0 ? 400 : 200);
        if (it % 7 == 0) { dw = 1 + RND(8); dh = 1 + RND(8); }                  /* сильное уменьшение: больше строк, чем в быстрой таблице */
        if (it % 11 == 0) { sh = 300 + RND(400); dh = 1 + RND(6); }
        int cw = 200 + RND(200), ch = 150 + RND(150);
        int dx = RND(cw + 80) - 40, dy = RND(ch + 80) - 40;
        uint32_t* sp = malloc((size_t)sw * sh * 4);
        for (int i = 0; i < sw * sh; i++) { seed = seed * 1103515245u + 12345u; sp[i] = (seed >> 3) & 0xFFFFFF; }
        surface_t src = { sp, sw, sh, sw };
        uint32_t* a = calloc((size_t)cw * ch, 4); uint32_t* b = calloc((size_t)cw * ch, 4);
        surface_t A = { a, cw, ch, cw }, B = { b, cw, ch, cw };
        ref_blit_scaled(&A, dx, dy, dw, dh, &src);
        blit_scaled(&B, dx, dy, dw, dh, &src);
        runs++;
        if (memcmp(a, b, (size_t)cw * ch * 4)) { if (++bad < 6) printf("FAIL: src %dx%d -> %dx%d at %d,%d\n", sw, sh, dw, dh, dx, dy); }
        free(sp); free(a); free(b);
    }
    printf("blit_scaled: %ld прогонов, расхождений %ld\n", runs, bad);
    return bad != 0;
}
