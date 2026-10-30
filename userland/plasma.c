#include "kira.h"
#include "sin_table.h"
KIRA_ENTRY()

#define W 256
#define H 160

int umain(void) {
    u32* px = k_gfx_create(W, H);
    if (!px) { k_write("plasma: cannot create window\n", 29); return 1; }
    k_write("plasma: running; close the window to exit\n", 42);
    u32 t = 0;
    for (;;) {
        kevent_t ev;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) return 0;
            if (ev.type == KEV_KEY_DOWN && ev.b == 'q') return 0;
        }
        for (u32 y = 0; y < H; y++) {
            u32 row = SIN8[(y * 5 + t * 2) & 255];
            for (u32 x = 0; x < W; x++) {
                u32 v = SIN8[(x * 3 + t) & 255] + row + SIN8[((x + y) * 2 + 255 - (t & 255)) & 255];
                v = (v >> 1) + t;
                u32 r = SIN8[v & 255], g = SIN8[(v + 85) & 255], b = SIN8[(v + 170) & 255];
                px[y * W + x] = (r << 16) | (g << 8) | b;
            }
        }
        k_gfx_present();
        t += 2;
        k_sleep(2);
    }
}
