// Рисование не зависит от того, в какую часть кадра смотрит поверхность: примитив, нарисованный в вырезанное окно кадра, даёт те же точки,
// что тот же примитив при рисовании на весь кадр. Композитор собирает кадр прямоугольниками (повреждённые части), поэтому от этого зависит,
// остаются ли на экране следы при перемещении окон (docs/WM_PERF.md).
#include "../gfx2d.h"
#include "../gfxui.h"
#include "../icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { W = 240, H = 140 };
static uint32_t ref[W * H], got[W * H];
static unsigned seed = 4242;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 8) % (unsigned)n); }
typedef void (*draw_fn)(surface_t* s, int ox, int oy, int k);   // ox, oy - смещение координат кадра в поверхность
static void fillbg(uint32_t* p) { for (int i = 0; i < W * H; i++) p[i] = 0x203040u + (uint32_t)(i % 13) * 0x010203u; }

static void d_rr(surface_t* s, int ox, int oy, int k) { ui_round_rect(s, ox + 10 + k, oy + 8, 100 + k, 40, 9, 0x336699, 200 + k % 56); }
static void d_hole(surface_t* s, int ox, int oy, int k) { ui_round_rect_hole(s, ox + 20, oy + 6, 150, 90, 10, 0x445566, 255, ox + 30 + k, oy + 40, 100, 40); }
static void d_frame(surface_t* s, int ox, int oy, int k) { ui_round_frame(s, ox + 12, oy + 10 + k, 160, 70, 10, 0xAABBCC, 255); }
static void d_disc(surface_t* s, int ox, int oy, int k) { ui_disc(s, ox + 60 + k, oy + 40, 17, 0xCC8844, 255); }
static void d_ring(surface_t* s, int ox, int oy, int k) { ui_ring(s, ox + 80, oy + 50 + k % 9, 14, 18, 0x88CC44, 230); }
static void d_seg(surface_t* s, int ox, int oy, int k) { ui_seg(s, (ox + 10) * 16, (oy + 10 + k % 7) * 16, (ox + 150) * 16 + k, (oy + 90) * 16, 24, 0xFFFFFF, 255); }
static void d_text(surface_t* s, int ox, int oy, int k) { gfx_text(s, ox + 10 + k, oy + 20, "Разум: окно Hello", 0xE0E0E0, -1); }
static void d_bold(surface_t* s, int ox, int oy, int k) { ui_text_bold(s, ox + 14, oy + 30 + k % 5, "Терминал", 0xFFFFFF); }
static void d_smooth(surface_t* s, int ox, int oy, int k) { ui_text_smooth(s, ox + 22, oy + 10 + k % 4, "KIRA OS", 18 * 16 / 16, 0xFFFFFF); }
static void d_arc(surface_t* s, int ox, int oy, int k) { ui_arc(s, ox + 70, oy + 60, 20, 28, 300, 2400 + k, 0x44AACC); }
static void d_line(surface_t* s, int ox, int oy, int k) { gfx_line(s, ox + 5, oy + 5 + k % 6, ox + 200, oy + 120, 0xFFAA00); }
static int icon_kind;
static void d_icon(surface_t* s, int ox, int oy, int k) { icon_draw(s, ox + 50 + k % 5, oy + 40 + k % 3, 28 + (k % 3) * 8, icon_kind, 0x00D0FF, k & 1); }

static int check(const char* name, draw_fn f, int variants) {
    int bad = 0;
    for (int k = 0; k < variants; k++) {
        fillbg(ref); surface_t full = { ref, W, H, W }; f(&full, 0, 0, k);
        for (int t = 0; t < 40; t++) {
            int x0 = rnd(W - 20), y0 = rnd(H - 20), w = 1 + rnd(W - x0), h = 1 + rnd(H - y0);
            fillbg(got);
            surface_t clip = { got + (long)y0 * W + x0, w, h, W };
            f(&clip, -x0, -y0, k);
            for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++)
                if (got[y * W + x] != ref[y * W + x]) { if (bad++ < 3) printf("%s: k=%d clip %d,%d %dx%d: differs at %d,%d (%06x, whole-frame %06x)\n", name, k, x0, y0, w, h, x, y, got[y*W+x], ref[y*W+x]); }
            for (int i = 0; i < W * H; i++) {                    // за пределами окна кадра ничего не должно измениться
                int x = i % W, y = i / W;
                if ((x < x0 || x >= x0 + w || y < y0 || y >= y0 + h) && got[i] != (0x203040u + (uint32_t)(i % 13) * 0x010203u)) { if (bad++ < 3) printf("%s: wrote outside the clip at %d,%d\n", name, x, y); break; }
            }
        }
    }
    return bad;
}

int main(void) {
    int bad = 0;
    struct { const char* n; draw_fn f; } t[] = { { "round_rect", d_rr }, { "round_rect_hole", d_hole }, { "round_frame", d_frame }, { "disc", d_disc }, { "ring", d_ring },
        { "seg", d_seg }, { "text", d_text }, { "text_bold", d_bold }, { "text_smooth", d_smooth }, { "arc", d_arc }, { "line", d_line } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) bad += check(t[i].n, t[i].f, 6);
    for (icon_kind = ICON_TERMINAL; icon_kind < ICON_LAST; icon_kind++) bad += check("icon", d_icon, 8);
    if (bad) { printf("clip: %d differences\n", bad); return 1; }
    printf("clip: drawing into a clipped part of the frame = drawing the whole frame (primitives, text, icons) passed\n");
    return 0;
}
