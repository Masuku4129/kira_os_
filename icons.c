#include "icons.h"
#include "gfxui.h"

/* Координаты значка заданы в сетке 0..64 (центр 32): переводятся в 1/16 пикселя. */
typedef struct { surface_t* s; int cx, cy, size; uint32_t col; unsigned alpha; int w16; } pen_t;

static int X(const pen_t* p, int u) { return p->cx * 16 + (u - 32) * p->size * 16 / 64; }
static int Y(const pen_t* p, int v) { return p->cy * 16 + (v - 32) * p->size * 16 / 64; }

static void ln(const pen_t* p, int x0, int y0, int x1, int y1) { ui_seg(p->s, X(p, x0), Y(p, y0), X(p, x1), Y(p, y1), p->w16, p->col, p->alpha); }

static void poly(const pen_t* p, const int* pts, int n, int closed) {
    for (int i = 0; i + 1 < n; i++) ln(p, pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3]);
    if (closed && n > 2) ln(p, pts[2 * n - 2], pts[2 * n - 1], pts[0], pts[1]);
}

/* Окружность из отрезков (32 шага по таблице синусов в 1/1024). */
static const int16_t sn[33] = { 0, 200, 392, 569, 724, 851, 946, 1004, 1024, 1004, 946, 851, 724, 569, 392, 200, 0,
                                -200, -392, -569, -724, -851, -946, -1004, -1024, -1004, -946, -851, -724, -569, -392, -200, 0 };
static int cs(int i) { return sn[(i + 8) % 32]; }
static void circle(const pen_t* p, int u, int v, int r) {
    for (int i = 0; i < 32; i++)
        ln(p, u + r * cs(i) / 1024, v + r * sn[i] / 1024, u + r * cs(i + 1) / 1024, v + r * sn[i + 1] / 1024);
}
static void dot(const pen_t* p, int u, int v, int r) {
    int rr = r * p->size / 64; if (rr < 1) rr = 1;
    ui_disc(p->s, X(p, u) / 16, Y(p, v) / 16, rr, p->col, p->alpha);
}

static void shape(const pen_t* p, int kind) {
    switch (kind) {
    case ICON_TERMINAL: {
        static const int box[] = { 6, 12, 58, 12, 58, 52, 6, 52 };
        poly(p, box, 4, 1);
        ln(p, 16, 24, 26, 32); ln(p, 26, 32, 16, 40);
        ln(p, 30, 42, 46, 42);
        break;
    }
    case ICON_FOLDER: {
        static const int f[] = { 6, 16, 24, 16, 30, 22, 58, 22, 58, 52, 6, 52 };
        poly(p, f, 6, 1);
        ln(p, 6, 28, 58, 28);
        break;
    }
    case ICON_FILE: {
        static const int f[] = { 14, 6, 40, 6, 52, 18, 52, 58, 14, 58 };
        poly(p, f, 5, 1);
        ln(p, 40, 6, 40, 18); ln(p, 40, 18, 52, 18);
        ln(p, 22, 32, 44, 32); ln(p, 22, 42, 44, 42);
        break;
    }
    case ICON_AI: {
        circle(p, 32, 32, 26);
        /* буквы A и I линиями */
        ln(p, 17, 44, 24, 20); ln(p, 24, 20, 31, 44); ln(p, 20, 36, 28, 36);
        ln(p, 40, 20, 40, 44); ln(p, 36, 20, 44, 20); ln(p, 36, 44, 44, 44);
        break;
    }
    case ICON_BRAIN: {
        /* силуэт двух полушарий и «нейроны» внутри */
        static const int l[] = { 31, 10, 22, 8, 14, 12, 9, 20, 6, 30, 9, 40, 14, 48, 22, 54, 31, 54 };
        static const int r[] = { 33, 10, 42, 8, 50, 12, 55, 20, 58, 30, 55, 40, 50, 48, 42, 54, 33, 54 };
        poly(p, l, 9, 0); poly(p, r, 9, 0);
        ln(p, 32, 12, 32, 52);
        static const int nodes[] = { 18, 22, 24, 34, 16, 40, 44, 22, 40, 34, 48, 40, 26, 46, 38, 46 };
        ln(p, 18, 22, 24, 34); ln(p, 24, 34, 16, 40); ln(p, 24, 34, 26, 46);
        ln(p, 44, 22, 40, 34); ln(p, 40, 34, 48, 40); ln(p, 40, 34, 38, 46);
        for (int i = 0; i < 8; i++) dot(p, nodes[2 * i], nodes[2 * i + 1], 4);
        break;
    }
    case ICON_MONITOR: {
        ln(p, 8, 54, 56, 54);
        static const int b1[] = { 12, 54, 12, 38, 22, 38, 22, 54 };
        static const int b2[] = { 27, 54, 27, 24, 37, 24, 37, 54 };
        static const int b3[] = { 42, 54, 42, 12, 52, 12, 52, 54 };
        poly(p, b1, 4, 0); poly(p, b2, 4, 0); poly(p, b3, 4, 0);
        break;
    }
    case ICON_WAVE: {
        int prev_u = 6, prev_v = 32;
        for (int i = 1; i <= 16; i++) {
            int u = 6 + i * 52 / 16, v = 32 - 16 * sn[(i * 4) % 32] / 1024;
            ln(p, prev_u, prev_v, u, v);
            prev_u = u; prev_v = v;
        }
        dot(p, 6, 32, 3); dot(p, 58, 32, 3);
        break;
    }
    case ICON_BRUSH: {
        ln(p, 50, 8, 56, 14); ln(p, 50, 8, 24, 34); ln(p, 56, 14, 30, 40);
        static const int tip[] = { 24, 34, 30, 40, 22, 50, 10, 56, 14, 44 };
        poly(p, tip, 5, 1);
        break;
    }
    case ICON_CUBE: {
        static const int top[] = { 32, 6, 56, 18, 32, 30, 8, 18 };
        poly(p, top, 4, 1);
        ln(p, 8, 18, 8, 46); ln(p, 56, 18, 56, 46); ln(p, 32, 30, 32, 58);
        ln(p, 8, 46, 32, 58); ln(p, 56, 46, 32, 58);
        break;
    }
    case ICON_GEAR: {
        circle(p, 32, 32, 18);
        circle(p, 32, 32, 7);
        for (int i = 0; i < 32; i += 4) ln(p, 32 + 18 * cs(i) / 1024, 32 + 18 * sn[i] / 1024, 32 + 27 * cs(i) / 1024, 32 + 27 * sn[i] / 1024);
        break;
    }
    case ICON_HOME: {
        static const int h[] = { 8, 30, 32, 8, 56, 30 };
        poly(p, h, 3, 0);
        static const int b[] = { 14, 26, 14, 56, 50, 56, 50, 26 };
        poly(p, b, 4, 0);
        static const int d[] = { 27, 56, 27, 40, 37, 40, 37, 56 };
        poly(p, d, 4, 0);
        break;
    }
    case ICON_CHIP: {                                    /* микросхема: корпус, сердцевина и выводы по четырём сторонам */
        static const int body[] = { 16, 16, 48, 16, 48, 48, 16, 48 };
        poly(p, body, 4, 1);
        static const int core[] = { 26, 26, 38, 26, 38, 38, 26, 38 };
        poly(p, core, 4, 1);
        for (int i = 0; i < 3; i++) {
            int t = 22 + i * 10;
            ln(p, t, 16, t, 7); ln(p, t, 48, t, 57); ln(p, 16, t, 7, t); ln(p, 48, t, 57, t);
        }
        break;
    }
    default: break;
    }
}

void icon_draw(surface_t* s, int cx, int cy, int size, int kind, uint32_t color, int glow) {
    pen_t p = { s, cx, cy, size, color, 255, size >= 32 ? 22 : 16 };
    if (glow) {                                         /* свечение: та же фигура шире и прозрачнее */
        pen_t g = p; g.w16 = p.w16 * 3; g.alpha = 36;
        shape(&g, kind);
        g.w16 = p.w16 * 2; g.alpha = 60;
        shape(&g, kind);
    }
    shape(&p, kind);
}
