/* ui_seg: обход только полосы вокруг отрезка даёт те же точки, что полный обход прямоугольника вокруг него
   (эталон - прежний алгоритм, в нём каждая точка рамки проверяется по расстоянию до отрезка). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfxui.h"

static void ref_seg(surface_t* s, int x0, int y0, int x1, int y1, int w16, uint32_t color, unsigned alpha) {
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
            uint64_t q2 = (uint64_t)(qx * qx + qy * qy), r = 0;
            while ((r + 1) * (r + 1) <= q2) r++;                       /* целый корень (медленно, но независимо от isqrt64) */
            int c = w16 - (int)r + 8;
            if (c <= 0) continue;
            if (c > 16) c = 16;
            ui_blend(s, px, py, color, alpha * (unsigned)c / 16);
        }
}

int main(void) {
    enum { W = 120, H = 90 };
    unsigned seed = 99;
#define RND(n) (seed = seed * 1103515245u + 12345u, (int)((seed >> 8) % (unsigned)(n)))
    long bad = 0;
    uint32_t* a = malloc(W * H * 4); uint32_t* b = malloc(W * H * 4);
    surface_t A = { a, W, H, W }, B = { b, W, H, W };
    for (int it = 0; it < 3000; it++) {
        for (int i = 0; i < W * H; i++) a[i] = b[i] = (uint32_t)RND(0xFFFFFF);
        int x0 = RND(W * 16 + 600) - 300, y0 = RND(H * 16 + 600) - 300;
        int x1 = it % 5 == 0 ? x0 : RND(W * 16 + 600) - 300, y1 = it % 7 == 0 ? y0 : RND(H * 16 + 600) - 300;
        int w16 = RND(40);
        uint32_t col = (uint32_t)RND(0xFFFFFF); unsigned al = (unsigned)RND(256);
        ref_seg(&A, x0, y0, x1, y1, w16, col, al);
        ui_seg(&B, x0, y0, x1, y1, w16, col, al);
        if (memcmp(a, b, W * H * 4) && ++bad < 5) printf("FAIL: отрезок %d,%d - %d,%d толщина %d\n", x0, y0, x1, y1, w16);
    }
    printf("ui_seg: 3000 отрезков, расхождений %ld\n", bad);
    free(a); free(b);
    return bad != 0;
}
