#include "brainfx.h"
#include "gfxui.h"
#include "theme.h"

/* Фон рабочего стола: ночное небо с сеткой и «нейросеть» в форме мозга. Узлы и связи рисуются один раз в буфер
   фона; живые вспышки узлов (импульсы) рисуются поверх буфера только там, где экран перерисовывается. */

#define MAXN BRAIN_MAX_NODES
#define MAXP 24

typedef struct { int16_t x, y; uint8_t nb[4]; uint8_t nn; uint32_t col; } node_t;
typedef struct { int16_t node; uint32_t start, dur; uint8_t live; } pulse_t;

static node_t nd[MAXN];
static int nn;
static pulse_t pl[MAXP];
static uint32_t rng = 0x4B495241u;
static uint32_t last_spawn, last_chain;

static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Форма: два полушария, щель между ними сверху, мозжечок справа внизу (координаты в 1/1000 полуосей). */
static int inside(int x, int y) {
    int64_t lx = x + 230, ly = y + 20, rx = x - 230;
    int in_l = lx * lx * 1000 / (500 * 500) + ly * ly * 1000 / (430 * 430) <= 1000;
    int in_r = rx * rx * 1000 / (500 * 500) + ly * ly * 1000 / (430 * 430) <= 1000;
    int64_t cx = x - 300, cy = y - 420;
    int in_c = cx * cx * 1000 / (240 * 240) + cy * cy * 1000 / (140 * 140) <= 1000;
    if ((x > -25 && x < 25) && y < 200) return 0;                    /* продольная щель */
    return in_l || in_r || in_c;
}

int brainfx_build(int cx, int cy, int half_w, int half_h) {
    nn = 0;
    rng = 0x4B495241u;
    for (int tries = 0; tries < 6000 && nn < MAXN; tries++) {
        int u = (int)(rnd() % 2001) - 1000, v = (int)(rnd() % 2001) - 1000;
        if (!inside(u, v)) continue;
        int x = cx + u * half_w / 1000, y = cy + v * half_h / 1000;
        int ok = 1, mind = half_w / 11;
        for (int i = 0; i < nn && ok; i++) {
            int dx = nd[i].x - x, dy = nd[i].y - y;
            if (dx * dx + dy * dy < mind * mind) ok = 0;
        }
        if (!ok) continue;
        node_t* n = &nd[nn++];
        n->x = (int16_t)x; n->y = (int16_t)y; n->nn = 0;
        unsigned t = (unsigned)((u + 1000) * 256 / 2000);              /* слева голубой, справа фиолетовый */
        n->col = ui_mix(TH_CYAN, TH_PURPLE, t);
        if (rnd() % 9 == 0) n->col = TH_GREEN;
    }
    int maxd = half_w / 4;
    for (int i = 0; i < nn; i++) {                                     /* связи: до трёх ближайших соседей */
        for (int k = 0; k < 3; k++) {
            int best = -1, bd = maxd * maxd + 1;
            for (int j = 0; j < nn; j++) {
                if (j == i) continue;
                int dup = 0;
                for (int q = 0; q < nd[i].nn; q++) if (nd[i].nb[q] == j) dup = 1;
                if (dup) continue;
                int dx = nd[i].x - nd[j].x, dy = nd[i].y - nd[j].y, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0 || nd[i].nn >= 4) break;
            nd[i].nb[nd[i].nn++] = (uint8_t)best;
            if (nd[best].nn < 4) nd[best].nb[nd[best].nn++] = (uint8_t)i;
        }
    }
    for (int i = 0; i < MAXP; i++) pl[i].live = 0;
    return nn;
}

void brainfx_render(surface_t* bg, int cx, int cy, int half_w, int half_h) {
    int W = bg->w, H = bg->h;
    ui_vgrad(bg, 0, 0, W, H, TH_BG0, TH_BG1);
    for (int y = 0; y < H; y += 48) for (int x = 0; x < W; x++) ui_blend(bg, x, y, TH_GRID, 9);      /* тонкая сетка */
    for (int x = 0; x < W; x += 48) for (int y = 0; y < H; y++) ui_blend(bg, x, y, TH_GRID, 9);
    for (int y = 0; y < H; y += 48) for (int x = 0; x < W; x += 48) ui_blend(bg, x, y, TH_GRID, 40);
    /* сияние за мозгом: фиолетовое ядро, голубой ореол */
    int rx = half_w * 13 / 10, ry = half_h * 13 / 10;
    for (int y = cy - ry; y < cy + ry; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = cx - rx; x < cx + rx; x++) {
            if (x < 0 || x >= W) continue;
            int64_t dx = (int64_t)(x - cx) * 1000 / rx, dy = (int64_t)(y - cy) * 1000 / ry;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 >= 1000000) continue;
            unsigned a = (unsigned)((1000000 - d2) * 70 / 1000000);
            ui_blend(bg, x, y, d2 < 250000 ? 0x002A1E5Cu : 0x00102A50u, a);
        }
    }
    for (int i = 0; i < nn; i++)                                        /* связи: мягкое свечение и тонкая линия */
        for (int k = 0; k < nd[i].nn; k++) {
            int j = nd[i].nb[k];
            if (j < i) continue;
            uint32_t c = ui_mix(nd[i].col, nd[j].col, 128);
            ui_seg(bg, nd[i].x * 16, nd[i].y * 16, nd[j].x * 16, nd[j].y * 16, 20, c, 16);
            ui_seg(bg, nd[i].x * 16, nd[i].y * 16, nd[j].x * 16, nd[j].y * 16, 7, c, 90);
        }
    for (int i = 0; i < nn; i++) {
        ui_disc(bg, nd[i].x, nd[i].y, 6, nd[i].col, 26);
        ui_disc(bg, nd[i].x, nd[i].y, 3, nd[i].col, 120);
        ui_disc(bg, nd[i].x, nd[i].y, 1, 0x00FFFFFF, 200);
    }
}

static void pulse_rect(int i, int* x0, int* y0, int* x1, int* y1) {
    const node_t* n = &nd[pl[i].node];
    *x0 = n->x - 14; *y0 = n->y - 14; *x1 = n->x + 15; *y1 = n->y + 15;
}

static int spawn(int node, uint32_t at, uint32_t dur) {
    for (int i = 0; i < MAXP; i++) if (!pl[i].live) { pl[i].node = (int16_t)node; pl[i].start = at; pl[i].dur = dur; pl[i].live = 1; return i; }
    return -1;
}

void brainfx_anim(uint32_t now_ms, int fx, unsigned activity, void (*damage)(int x0, int y0, int x1, int y1)) {
    if (!nn) return;
    for (int i = 0; i < MAXP; i++) {                                    /* живые вспышки меняют яркость каждый кадр */
        if (!pl[i].live) continue;
        int x0, y0, x1, y1; pulse_rect(i, &x0, &y0, &x1, &y1);
        if (now_ms >= pl[i].start + pl[i].dur) pl[i].live = 0;          /* последний кадр: стереть вспышку */
        if (now_ms >= pl[i].start) damage(x0, y0, x1, y1);
    }
    if (fx <= 0) return;
    uint32_t period = fx >= 3 ? 140 : fx == 2 ? 400 : 1600;            /* спокойное «дыхание» сети */
    if (now_ms - last_spawn >= period) {
        last_spawn = now_ms;
        spawn((int)(rnd() % (unsigned)nn), now_ms, 700 + rnd() % 500);
    }
    /* Разум принял решение или заметил необычное: по сети пробегает цепочка импульсов */
    if (activity && fx >= 2 && now_ms - last_chain >= 300) {
        last_chain = now_ms;
        int a = (int)(rnd() % (unsigned)nn);
        for (int step = 0; step < 6 && a >= 0; step++) {
            spawn(a, now_ms + (uint32_t)step * 110, 650);
            a = nd[a].nn ? nd[a].nb[rnd() % nd[a].nn] : -1;
        }
    }
}

void brainfx_draw(surface_t* clip, int ox, int oy, uint32_t now_ms) {
    for (int i = 0; i < MAXP; i++) {
        if (!pl[i].live || now_ms < pl[i].start) continue;
        uint32_t ph = now_ms - pl[i].start;
        if (ph >= pl[i].dur) continue;
        unsigned k = ph < pl[i].dur / 4 ? (unsigned)(ph * 256 / (pl[i].dur / 4 + 1)) : (unsigned)((pl[i].dur - ph) * 256 / (pl[i].dur * 3 / 4 + 1));
        const node_t* n = &nd[pl[i].node];
        int x = n->x + ox, y = n->y + oy;
        ui_disc(clip, x, y, 12, n->col, k * 40 / 256);
        ui_disc(clip, x, y, 7, n->col, k * 110 / 256);
        ui_disc(clip, x, y, 3, 0x00FFFFFF, k * 230 / 256);
    }
}

int brainfx_nodes(void) { return nn; }
