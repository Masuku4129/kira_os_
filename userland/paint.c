#include "kira.h"
KIRA_ENTRY()

#define W 480
#define H 320
#define BAR 24

static const u32 COLORS[8] = { 0x00FFFFFF, 0x00FF5252, 0x00FFB300, 0x00FFEE58, 0x0066BB6A, 0x0042A5F5, 0x00AB47BC, 0x00101820 };

static void rect(u32* px, int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h; j++) {
        if (j < BAR || j >= H) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < W) px[j * W + i] = c;
    }
}
static void bar(u32* px, int sel) {
    for (int j = 0; j < BAR; j++) for (int i = 0; i < W; i++) px[j * W + i] = 0x00202838;
    for (int k = 0; k < 8; k++) {
        int x0 = 6 + k * 34;
        for (int j = 3; j < BAR - 3; j++) for (int i = x0; i < x0 + 28; i++)
            px[j * W + i] = (j == 3 || j == BAR - 4 || i == x0 || i == x0 + 27) && k == sel ? 0x00FFFFFF : COLORS[k];
    }
}
static void stroke(u32* px, int x0, int y0, int x1, int y1, u32 c) {
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (n == 0) n = 1;
    for (int s = 0; s <= n; s++) rect(px, x0 + dx * s / n - 2, y0 + dy * s / n - 2, 5, 5, c);
}

/* Сохранение: рисунок (область под панелью) кодируется как "KPNT", ширина, высота и серия (цвет 0..7, длина u16);
   файл лежит в песочнице программы (/data/paint/picture.kpnt). Загрузка проверяет каждое поле: файл на диске недоверенный вход. */
#define FILE_NAME "picture.kpnt"
#define MAX_FILE 65536
static u8 file[MAX_FILE];

static int color_index(u32 c) {
    for (int k = 0; k < 8; k++) if (COLORS[k] == c) return k;
    return 7;
}
static void put16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u32 get16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }

static void save_picture(const u32* px) {
    u8* o = file;
    o[0] = 'K'; o[1] = 'P'; o[2] = 'N'; o[3] = 'T'; put16(o + 4, W); put16(o + 6, H - BAR);
    u32 n = 8;
    const u32* src = px + BAR * W;
    u32 total = (u32)W * (H - BAR), i = 0;
    while (i < total) {
        int idx = color_index(src[i]);
        u32 run = 1;
        while (i + run < total && run < 65535 && color_index(src[i + run]) == idx) run++;
        if (n + 3 > MAX_FILE) { k_write("paint: drawing too complex to save\n", 35); return; }
        o[n] = (u8)idx; put16(o + n + 1, run); n += 3;
        i += run;
    }
    i64 r = k_file_write(FILE_NAME, file, n);
    if (r == 0) k_write("paint: saved /data/paint/picture.kpnt\n", 38); else k_write("paint: save failed\n", 19);
}

static int load_picture(u32* px) {
    i64 n = k_file_read(FILE_NAME, file, MAX_FILE, 0);
    if (n < 8) { k_write("paint: no saved picture\n", 24); return 0; }
    if (file[0] != 'K' || file[1] != 'P' || file[2] != 'N' || file[3] != 'T' || get16(file + 4) != W || get16(file + 6) != H - BAR) { k_write("paint: bad picture file\n", 25); return 0; }
    u32 total = (u32)W * (H - BAR), pos = 0;
    for (i64 i = 8; i + 3 <= n; i += 3) {                           /* сначала проверка всего файла, потом рисование */
        if (file[i] > 7 || get16(file + i + 1) == 0) { k_write("paint: bad picture file\n", 25); return 0; }
        pos += get16(file + i + 1);
        if (pos > total) { k_write("paint: bad picture file\n", 25); return 0; }
    }
    if (pos != total || (n - 8) % 3 != 0) { k_write("paint: bad picture file\n", 25); return 0; }
    u32* dst = px + BAR * W;
    pos = 0;
    for (i64 i = 8; i + 3 <= n; i += 3) {
        u32 c = COLORS[file[i]], run = get16(file + i + 1);
        for (u32 k = 0; k < run; k++) dst[pos++] = c;
    }
    k_write("paint: loaded /data/paint/picture.kpnt\n", 39);
    return 1;
}

int umain(void) {
    u32* px = k_gfx_create(W, H);
    if (!px) { k_write("paint: cannot create window\n", 28); return 1; }
    for (int i = BAR * W; i < W * H; i++) px[i] = 0x00101820;
    int sel = 0, down = 0, lx = 0, ly = 0, dirty = 1;
    bar(px, sel);
    for (;;) {
        kevent_t ev;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) return 0;
            if (ev.type == KEV_KEY_DOWN) {
                if (ev.b >= '1' && ev.b <= '8') { sel = (int)ev.b - '1'; bar(px, sel); dirty = 1; }
                else if (ev.b == 'c') { for (int i = BAR * W; i < W * H; i++) px[i] = 0x00101820; dirty = 1; }
                else if (ev.b == 's') save_picture(px);
                else if (ev.b == 'l') { if (load_picture(px)) dirty = 1; }
                else if (ev.b == 'q') return 0;
            } else if (ev.type == KEV_MOUSE_BUTTON) {
                int d = (int)(ev.c & 1);
                int x = (int)ev.a, y = (int)ev.b;
                if (d && !down) {
                    if (y >= 0 && y < BAR && x >= 6 && x < 6 + 8 * 34) { sel = (x - 6) / 34; bar(px, sel); dirty = 1; }
                    else { lx = x; ly = y; stroke(px, x, y, x, y, COLORS[sel]); dirty = 1; }
                }
                down = d;
            } else if (ev.type == KEV_MOUSE_MOVE) {
                int x = (int)ev.a, y = (int)ev.b;
                if (down && (ev.c & 1) && y >= BAR) { stroke(px, lx, ly, x, y, COLORS[sel]); dirty = 1; }
                lx = x; ly = y;
            }
        }
        if (dirty) { k_gfx_present(); dirty = 0; }
        k_sleep(2);
    }
}
