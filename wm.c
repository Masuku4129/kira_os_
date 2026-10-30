#include "wm.h"
#include "gfxui.h"
#include "icons.h"
#include "brainfx.h"
#include "theme.h"

#define CUR_W 12
#define CUR_H 19
#define PANEL_R 10
#define TILE 44                    /* плитка значка боковой панели */
#define TILE_GAP 10
#define SIDE_X 10
#define PILL_ZONE 56               /* в полноэкранном режиме панель кнопок показывается, когда курсор выше этой черты */
#define LIGHT_R 6                  /* «светофор» окна */

typedef struct {
    int used, visible, closable, pinned, user, full, input;
    int resizable, zoomable;
    int zoom16;
    int cap_w, cap_h, min_w, min_h, step_w, step_h, norm_w, norm_h;
    int x, y;
    char title[40];
    char sub[24];
    char tag[16];
    surface_t content;
    const volatile uint32_t* seq;
} win_t;

static surface_t* scr;
static wm_ops_t ops;
static win_t win[WM_MAX_WIN];
static int zorder[WM_MAX_WIN];
static int zcount;
static int cx, cy;
static unsigned prev_buttons;
static int dragging = -1, drag_ox, drag_oy;
static int resizing = -1;
static int capture = -1;
static int dmg_valid, dx0, dy0, dx1, dy1;
static wm_side_item_t side[WM_SIDE_MAX];
static int side_n, side_hover = -1;
static wm_tab_t tabs[WM_TAB_MAX];
static int tab_n, tab_hover = -1, last_tab = -2;
static int light_hover = -1;       /* окно, над «светофором» которого курсор */
static int last_fs = -1, last_pill;
static int cascade;
static uint32_t* bg;
static surface_t bgs;
static char clock_date[48], clock_hm[8];
static uint8_t m_busy[64];
static int m_cores, m_mem = -1, m_health = -1;
static int fx_level = 3;
static uint32_t anim_ms;
static int brain_cx, brain_cy;

static const char* const cursor_art[CUR_H] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.....##### ", "#..#..#     ",
    "#.# #..#    ", "##  #..#    ", "#    #..#   ", "     #..#   ", "      #..#  ", "      #..#  ", "       ##   "
};

/* Изменённые места экрана - карта плиток 64x64 (а не одна охватывающая рамка): два окна в разных углах экрана
   перерисовываются как две области, а не как весь экран между ними. Плитки собираются в полосы-задания, которые
   композитор раздаёт ядрам (wm_compose_par). */
#define DTILE 64
#define TMAX_X 160                       /* экран до 10240 точек в ширину */
#define TMAX_Y 160
static uint8_t tdirty[TMAX_Y][TMAX_X];

static void damage(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > scr->w) x1 = scr->w;
    if (y1 > scr->h) y1 = scr->h;
    if (x0 >= x1 || y0 >= y1) return;
    {
        int ty1 = (y1 - 1) / DTILE, tx1 = (x1 - 1) / DTILE;
        if (ty1 >= TMAX_Y) ty1 = TMAX_Y - 1;
        if (tx1 >= TMAX_X) tx1 = TMAX_X - 1;
        for (int ty = y0 / DTILE; ty <= ty1; ty++)
            for (int tx = x0 / DTILE; tx <= tx1; tx++) tdirty[ty][tx] = 1;
    }
    if (!dmg_valid) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; dmg_valid = 1; return; }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

/* --- геометрия --- */

#define ZOOM_MIN 8
#define ZOOM_MAX 64

static int disp_w(const win_t* w) { return w->zoomable ? (w->content.w * w->zoom16 + 8) / 16 : w->content.w; }
static int disp_h(const win_t* w) { return w->zoomable ? (w->content.h * w->zoom16 + 8) / 16 : w->content.h; }
static int outer_w(const win_t* w) { return disp_w(w) + 2 * WM_PAD; }
static int outer_h(const win_t* w) { return disp_h(w) + WM_HEAD + WM_PAD; }

static int fs_scale(const win_t* w) {
    int sx = scr->w / w->content.w, sy = scr->h / w->content.h;
    int s = sx < sy ? sx : sy;
    return s < 1 ? 1 : s;
}

static void geom(const win_t* w, int* ox, int* oy, int* dw, int* dh) {
    if (w->full) {
        int s = w->resizable ? 1 : fs_scale(w);
        *dw = w->content.w * s; *dh = w->content.h * s;
        *ox = (scr->w - *dw) / 2; *oy = (scr->h - *dh) / 2;
    } else {
        *dw = disp_w(w); *dh = disp_h(w); *ox = w->x + WM_PAD; *oy = w->y + WM_HEAD;
    }
}

static void damage_win(const win_t* w) {
    if (w->full) damage(0, 0, scr->w, scr->h);
    else damage(w->x - WM_GLOW, w->y - WM_GLOW, w->x + outer_w(w) + WM_GLOW, w->y + outer_h(w) + WM_GLOW);
}
static void damage_cursor(void) { damage(cx, cy, cx + CUR_W, cy + CUR_H); }
static void damage_top(void) { damage(0, 0, scr->w, WM_TOP_H); }

static int fs_top(void) {
    for (int i = zcount - 1; i >= 0; i--) {
        const win_t* w = &win[zorder[i]];
        if (w->visible && !w->pinned) return w->full ? zorder[i] : -1;
    }
    return -1;
}

/* --- боковая панель --- */

static int side_panel_h(void) { return scr->h - WM_TOP_H - 20; }

#define SEP_GAP 14                 /* черта между закреплёнными и идущими */
static int side_item_y(int i) {
    int top = WM_TOP_H + 10 + 12;
    if (!side[i].bottom) {
        int k = 0, sep = 0;
        for (int j = 0; j < i; j++) if (!side[j].bottom) k++;
        if (side[i].temp) for (int j = 0; j < i; j++) if (!side[j].bottom && !side[j].temp) { sep = 1; break; }
        return top + k * (TILE + TILE_GAP) + sep * SEP_GAP;
    }
    int k = 0; for (int j = i + 1; j < side_n; j++) if (side[j].bottom) k++;
    return WM_TOP_H + 10 + side_panel_h() - 12 - TILE - k * (TILE + TILE_GAP);
}

static void damage_side(void) { damage(0, WM_TOP_H, WM_SIDE_W + 300, scr->h); }

static int side_hit(int px, int py) {
    for (int i = 0; i < side_n; i++) {
        int x = SIDE_X + 6, y = side_item_y(i);
        if (px >= x && px < x + TILE && py >= y && py < y + TILE) return i;
    }
    return -1;
}

int wm_side_center(int i, int* x, int* y) {
    if (i < 0 || i >= side_n) return -1;
    *x = SIDE_X + 6 + TILE / 2; *y = side_item_y(i) + TILE / 2;
    return 0;
}

/* --- верхняя панель --- */

#define TAB_X0 168
#define TAB_H 26

static int tab_x(int i) {
    int x = TAB_X0;
    for (int k = 0; k < i; k++) x += gfx_text_width(tabs[k].label) + 26;
    return x;
}
static int tab_w(int i) { return gfx_text_width(tabs[i].label) + 22; }

static int tab_hit(int px, int py) {
    if (py < 7 || py >= 7 + TAB_H) return -1;
    for (int i = 0; i < tab_n; i++) if (px >= tab_x(i) && px < tab_x(i) + tab_w(i)) return i;
    return -1;
}

int wm_tab_center(int i, int* x, int* y) {
    if (i < 0 || i >= tab_n) return -1;
    *x = tab_x(i) + tab_w(i) / 2; *y = 7 + TAB_H / 2;
    return 0;
}

/* Раздел, соответствующий окну сверху (0 - «Главная», если сверху ничего нет). */
static int active_tab(void);

/* --- полноэкранная панель кнопок --- */

static void pill_rect(const win_t* w, int* x, int* y, int* pw, int* ph, int* n) {
    *n = 3; (void)w;
    *pw = 24 + *n * 36; *ph = 36;
    *x = (scr->w - *pw) / 2; *y = 8;
}
static void damage_pill(void) { damage(scr->w / 2 - 90, 0, scr->w / 2 + 90, 52); }

static uint32_t accent_of(const win_t* w) {
    for (int i = 0; i < side_n; i++) {
        const char* a = side[i].tag; const char* b = w->tag; int same = a && *a;
        while (same && (*a || *b)) { if (*a++ != *b++) same = 0; }
        if (same) return side[i].color;
    }
    return TH_CYAN;
}

/* --- фон --- */

static void bg_render(void) {
    int W = scr->w, H = scr->h;
    bg = (uint32_t*)ops.alloc((uint64_t)W * H * 4);
    if (!bg) return;
    bgs.px = bg; bgs.w = W; bgs.h = H; bgs.pitch = W;
    int area_x = WM_SIDE_W, area_w = W - WM_SIDE_W;
    brain_cx = area_x + area_w / 2;
    brain_cy = WM_TOP_H + (H - WM_TOP_H) / 2;
    int hh = (H - WM_TOP_H) * 34 / 100, hw = hh * 13 / 10;
    if (hw > area_w * 40 / 100) { hw = area_w * 40 / 100; hh = hw * 10 / 13; }
    brainfx_build(brain_cx, brain_cy, hw, hh);
    brainfx_render(&bgs, brain_cx, brain_cy, hw, hh);
}

/* --- создание и состояние окон --- */

static void str_copy(char* d, const char* s, int cap) {
    int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0;
}

void wm_init(surface_t* screen, const wm_ops_t* o) {
    scr = screen;
    ops = *o;
    for (int i = 0; i < WM_MAX_WIN; i++) win[i].used = 0;
    zcount = 0; dragging = -1; resizing = -1; capture = -1; prev_buttons = 0;
    side_n = 0; side_hover = -1; tab_n = 0; tab_hover = -1; light_hover = -1; last_tab = -2;
    last_fs = -1; last_pill = 0; cascade = 0; clock_date[0] = 0; clock_hm[0] = 0; m_cores = 0; m_mem = -1; m_health = -1;
    cx = scr->w / 2; cy = scr->h / 2;
    dmg_valid = 0;
    bg_render();
    damage(0, 0, scr->w, scr->h);
}

static void auto_place(win_t* w) {
    int ow = outer_w(w), oh = outer_h(w);
    int ax = scr->w >= 800 ? WM_SIDE_W + 8 : 8, aw = scr->w - ax - 12;
    int ay = WM_TOP_H + 12, ah = scr->h - ay - 12;
    int k = cascade++ % 4;
    int x = ax + (aw - ow) / 2 + k * 26, y = ay + (ah - oh) / 2 + k * 22 - 8;
    if (x + ow > scr->w - 8) x = scr->w - 8 - ow;
    if (x < 8) x = 8;
    if (y + oh > scr->h - 4) y = scr->h - 4 - oh;
    if (y < ay) y = ay;
    w->x = x; w->y = y;
}

static int create_in(const char* title, int x, int y, int cw, int ch, int flags, int user, uint32_t* mem, int capw, int caph) {
    if (cw <= 0 || ch <= 0) return -1;
    if (capw < cw) capw = cw;
    if (caph < ch) caph = ch;
    int id = -1;
    for (int i = 0; i < WM_MAX_WIN; i++) if (!win[i].used) { id = i; break; }
    if (id < 0) return -1;
    if (!mem) {
        mem = (uint32_t*)ops.alloc((uint64_t)capw * caph * 4);
        if (!mem) return -1;
        user = 0;
    }
    win_t* w = &win[id];
    w->used = 1; w->visible = 1; w->full = 0; w->seq = 0;
    w->closable = (flags & WM_CLOSABLE) != 0; w->pinned = (flags & WM_PINNED) != 0; w->user = user;
    w->input = (flags & WM_INPUT) != 0;
    str_copy(w->title, title, sizeof(w->title));
    str_copy(w->tag, title, sizeof(w->tag));
    w->sub[0] = 0;
    w->resizable = (flags & WM_RESIZABLE) != 0 && !user;
    w->zoomable = (flags & WM_ZOOMABLE) != 0 || user;
    w->zoom16 = 16;
    w->cap_w = capw; w->cap_h = caph;
    w->min_w = cw < 160 ? cw : 160; w->min_h = ch < 96 ? ch : 96; w->step_w = 2; w->step_h = 2;
    w->norm_w = cw; w->norm_h = ch;
    w->content.px = mem; w->content.w = cw; w->content.h = ch; w->content.pitch = user ? cw : capw;
    if (user) str_copy(w->sub, "Ring 3", sizeof w->sub);
    if (x == WM_AUTO || y == WM_AUTO) auto_place(w); else { w->x = x; w->y = y; }
    if (!user) gfx_fill(&w->content, TH_CONTENT);
    if (w->pinned) {
        for (int i = zcount; i > 0; i--) zorder[i] = zorder[i - 1];
        zorder[0] = id; zcount++;
    } else zorder[zcount++] = id;
    damage_win(w);
    damage_side();
    damage_top();
    return id;
}

int wm_create(const char* title, int x, int y, int cw, int ch, int flags) {
    return create_in(title, x, y, cw, ch, flags, 0, 0, cw, ch);
}

int wm_create_ex(const char* title, int x, int y, int cw, int ch, int flags, int cap_w, int cap_h) {
    return create_in(title, x, y, cw, ch, flags, 0, 0, cap_w, cap_h);
}

void wm_set_resize_limits(int id, int min_w, int min_h, int step_w, int step_h) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    win[id].min_w = min_w; win[id].min_h = min_h;
    win[id].step_w = step_w < 1 ? 1 : step_w; win[id].step_h = step_h < 1 ? 1 : step_h;
}

int wm_create_user(const char* title, int x, int y, int cw, int ch, uint32_t* mem) {
    if (!mem) return -1;
    return create_in(title, x, y, cw, ch, WM_CLOSABLE | WM_ZOOMABLE, 1, mem, cw, ch);
}

void wm_set_frame_seq(int id, const volatile uint32_t* seq) {
    if (id >= 0 && id < WM_MAX_WIN && win[id].used) win[id].seq = seq;
}

void wm_destroy(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    damage_win(&win[id]);
    win[id].used = 0; win[id].visible = 0;
    int j = 0;
    for (int i = 0; i < zcount; i++) if (zorder[i] != id) zorder[j++] = zorder[i];
    zcount = j;
    if (dragging == id) dragging = -1;
    if (resizing == id) resizing = -1;
    if (capture == id) capture = -1;
    if (light_hover == id) light_hover = -1;
    damage_side();
    damage_top();
}

surface_t* wm_content(int id) { return (id >= 0 && id < WM_MAX_WIN && win[id].used) ? &win[id].content : 0; }

void wm_set_title(int id, const char* t) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    str_copy(win[id].title, t, sizeof(win[id].title));
    if (win[id].visible && !win[id].full) damage(win[id].x, win[id].y, win[id].x + outer_w(&win[id]), win[id].y + WM_HEAD);
}

void wm_set_subtitle(int id, const char* t) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    str_copy(win[id].sub, t, sizeof(win[id].sub));
    if (win[id].visible && !win[id].full) damage(win[id].x, win[id].y, win[id].x + outer_w(&win[id]), win[id].y + WM_HEAD);
}

void wm_set_tag(int id, const char* t) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    str_copy(win[id].tag, t, sizeof(win[id].tag));
}

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int wm_find(const char* tag) {
    if (!tag || !*tag) return -1;
    for (int i = 0; i < WM_MAX_WIN; i++) if (win[i].used && streq(win[i].tag, tag)) return i;
    return -1;
}

void wm_side_set(const wm_side_item_t* items, int n) {
    if (n > WM_SIDE_MAX) n = WM_SIDE_MAX;
    for (int i = 0; i < n; i++) side[i] = items[i];
    side_n = n;
    damage_side();
}

void wm_tabs_set(const wm_tab_t* t, int n) {
    if (n > WM_TAB_MAX) n = WM_TAB_MAX;
    for (int i = 0; i < n; i++) tabs[i] = t[i];
    tab_n = n;
    damage_top();
}

int wm_user_tags(char (*out)[16], int max) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WIN && n < max; i++)
        if (win[i].used && win[i].user) { str_copy(out[n], win[i].tag, 16); n++; }
    return n;
}

int wm_top(void) {
    for (int i = zcount - 1; i >= 0; i--) if (win[zorder[i]].visible && !win[zorder[i]].pinned) return zorder[i];
    return -1;
}

static int active_tab(void) {
    int top = wm_top();
    if (top < 0) return 0;
    for (int i = 0; i < tab_n; i++) if (tabs[i].tag && tabs[i].tag[0] && streq(tabs[i].tag, win[top].tag)) return i;
    return -1;
}

void wm_raise(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used || win[id].pinned) return;
    int pos = -1;
    for (int i = 0; i < zcount; i++) if (zorder[i] == id) pos = i;
    if (pos < 0 || pos == zcount - 1) return;
    int prev_top = wm_top();
    for (int i = pos; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = id;
    if (prev_top >= 0 && prev_top != id) damage_win(&win[prev_top]);
    damage_win(&win[id]);
    damage_side();
    damage_top();
}

void wm_set_visible(int id, int v) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    v = v != 0;
    if (win[id].visible == v) return;
    win[id].visible = v;
    if (v) wm_raise(id);
    damage_win(&win[id]);
    damage_side();
    damage_top();
}

int wm_visible(int id) { return id >= 0 && id < WM_MAX_WIN && win[id].used && win[id].visible; }

void wm_minimize(int id) {
    if (!wm_visible(id) || win[id].pinned) return;
    if (win[id].full) wm_set_fullscreen(id, 0);
    wm_set_visible(id, 0);
    if (ops.notify) ops.notify(id, 0);
}

void wm_minimize_all(void) {
    for (int i = 0; i < WM_MAX_WIN; i++) if (win[i].used && win[i].visible && !win[i].pinned) wm_minimize(i);
}

static void owner_resize(int id, int nw, int nh) {
    win_t* w = &win[id];
    w->content.w = nw; w->content.h = nh;
    if (ops.resize) ops.resize(id, nw, nh);
}

void wm_set_fullscreen(int id, int on) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used || win[id].pinned) return;
    on = on != 0;
    if (win[id].full == on) return;
    damage_win(&win[id]);
    win[id].full = on;
    if (win[id].resizable) {
        if (on) {
            win[id].norm_w = win[id].content.w; win[id].norm_h = win[id].content.h;
            int nw = scr->w < win[id].cap_w ? scr->w : win[id].cap_w, nh = scr->h < win[id].cap_h ? scr->h : win[id].cap_h;
            owner_resize(id, nw / win[id].step_w * win[id].step_w, nh / win[id].step_h * win[id].step_h);
        } else owner_resize(id, win[id].norm_w, win[id].norm_h);
    }
    if (win[id].visible) wm_raise(id);
    damage_win(&win[id]);
    damage(0, 0, scr->w, scr->h);
}

int wm_fullscreen(int id) { return id >= 0 && id < WM_MAX_WIN && win[id].used && win[id].full; }

void wm_invalidate(int id) {
    if (!wm_visible(id)) return;
    win_t* w = &win[id];
    int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
    damage(ox, oy, ox + dw, oy + dh);
}

void wm_invalidate_rows(int id, int y0, int y1) {
    if (!wm_visible(id)) return;
    win_t* w = &win[id];
    if (y0 < 0) y0 = 0;
    if (y1 > w->content.h) y1 = w->content.h;
    if (y0 >= y1) return;
    int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
    int ch = w->content.h;
    damage(ox, oy + y0 * dh / ch, ox + dw, oy + (y1 * dh + ch - 1) / ch + 1);
}

void wm_invalidate_all(void) { damage(0, 0, scr->w, scr->h); }

static int same_str(const char* a, const char* b, int cap) {
    for (int i = 0; i < cap; i++) {
        char x = a[i], y = i < cap - 1 ? b[i] : 0;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

void wm_set_clock(const char* date, const char* hm) {
    if (same_str(clock_date, date, (int)sizeof clock_date) && same_str(clock_hm, hm, (int)sizeof clock_hm)) return;
    str_copy(clock_date, date, sizeof clock_date);
    str_copy(clock_hm, hm, sizeof clock_hm);
    damage_top();
}

void wm_set_meters(const uint8_t* busy, int ncores, int mem_pct, int health) {
    if (ncores > 64) ncores = 64;
    int changed = ncores != m_cores || mem_pct != m_mem || health != m_health;
    for (int i = 0; i < ncores && !changed; i++) if (m_busy[i] / 8 != busy[i] / 8) changed = 1;   /* шаг столбика ~ 8% */
    if (!changed) return;
    for (int i = 0; i < ncores; i++) m_busy[i] = busy[i];
    m_cores = ncores; m_mem = mem_pct; m_health = health;
    damage_top();
}

void wm_set_fx(int level) { fx_level = level < 0 ? 0 : level > 3 ? 3 : level; }

static void anim_damage(int x0, int y0, int x1, int y1) { damage(x0, y0, x1, y1); }

void wm_anim(uint32_t now_ms, unsigned activity) {
    anim_ms = now_ms;
    if (fs_top() >= 0) return;                        /* во весь экран фон не виден */
    brainfx_anim(now_ms, fx_level, activity, anim_damage);
}

void wm_cursor(int* x, int* y) { *x = cx; *y = cy; }
int wm_window_pos(int id, int* x, int* y) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return -1;
    *x = win[id].x; *y = win[id].y; return 0;
}
int wm_content_origin(int id, int* x, int* y, int* s) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return -1;
    int dw, dh; geom(&win[id], x, y, &dw, &dh);
    *s = dw / win[id].content.w; if (*s < 1) *s = 1;
    return 0;
}

int wm_zoom_percent(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return 0;
    return win[id].zoom16 * 100 / 16;
}
void wm_content_size(int id, int* w, int* h) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) { *w = *h = 0; return; }
    *w = win[id].content.w; *h = win[id].content.h;
}

void wm_set_content_size(int id, int w, int h) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    win_t* wn = &win[id];
    if (w > wn->cap_w) w = wn->cap_w;
    if (h > wn->cap_h) h = wn->cap_h;
    if (w < 1 || h < 1 || (w == wn->content.w && h == wn->content.h)) return;
    damage_win(wn);
    wn->content.w = w; wn->content.h = h;
    damage_win(wn);
}

/* --- мышь --- */

static int hit_rect(const win_t* w, int px, int py) {
    if (w->full) return 1;
    return px >= w->x && px < w->x + outer_w(w) && py >= w->y && py < w->y + outer_h(w);
}

static int hit_window(int px, int py) {
    for (int pass = 0; pass < 2; pass++)
        for (int i = zcount - 1; i >= 0; i--) {
            const win_t* w = &win[zorder[i]];
            if (!w->visible || w->pinned != pass) continue;
            if (hit_rect(w, px, py)) return zorder[i];
        }
    return -1;
}

/* «Светофор»: 1 - закрыть (красный), 2 - свернуть (жёлтый), 3 - на весь экран (зелёный). */
static int light_x(const win_t* w, int which) { return w->x + 18 + (which - 1) * 20; }
static int light_y(const win_t* w) { return w->y + WM_HEAD / 2; }

int wm_button_center(int id, int which, int* x, int* y) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used || win[id].pinned || which < 1 || which > 3) return -1;
    *x = light_x(&win[id], which); *y = light_y(&win[id]);
    return 0;
}

static int ctl_hit(const win_t* w, int px, int py) {
    if (w->pinned) return 0;
    for (int k = 1; k <= 3; k++) {
        int dx = px - light_x(w, k), dy = py - light_y(w);
        if (dx * dx + dy * dy <= 9 * 9) return k;
    }
    return 0;
}

static void close_req(int id) {
    win_t* w = &win[id];
    if (w->user) { if (ops.notify) ops.notify(id, -1); return; }
    wm_minimize(id);                                   /* окна ядра закрываются «на фон»: их можно вернуть значком */
}

static void side_click(int i) {
    if (i < 0 || i >= side_n) return;
    int id = wm_find(side[i].tag);
    if (id < 0) { if (ops.launch) ops.launch(i); return; }
    if (win[id].visible && wm_top() == id) { wm_minimize(id); return; }
    int was = win[id].visible;
    wm_set_visible(id, 1);
    wm_raise(id);
    if (!was && ops.notify) ops.notify(id, 1);
}

static int pill_visible(int fs) { return fs >= 0 && cy < PILL_ZONE; }

static int click_pill(int fs, int px, int py) {
    win_t* w = &win[fs];
    int x, y, pw, ph, n; pill_rect(w, &x, &y, &pw, &ph, &n);
    if (px < x || px >= x + pw || py < y || py >= y + ph) return 0;
    int k = (px - x - 12) / 36;
    if (k == 0) close_req(fs);
    else if (k == 1) wm_minimize(fs);
    else if (k == 2) wm_set_fullscreen(fs, 0);
    return 1;
}

static int grip_hit(const win_t* w, int px, int py) {
    if (!(w->resizable || w->zoomable) || w->full) return 0;
    return px >= w->x + outer_w(w) - 22 && py >= w->y + outer_h(w) - 22;
}

static void apply_resize(int id) {
    win_t* w = &win[id];
    if (w->zoomable) {
        int z = (cx - (w->x + WM_PAD)) * 16 / w->content.w;
        int zmax = (scr->w - w->x - 2 * WM_PAD) * 16 / w->content.w;
        int zmax_h = (scr->h - w->y - WM_HEAD - WM_PAD) * 16 / w->content.h;
        if (zmax_h < zmax) zmax = zmax_h;
        if (zmax > ZOOM_MAX) zmax = ZOOM_MAX;
        if (z > zmax) z = zmax;
        if (z < ZOOM_MIN) z = ZOOM_MIN;
        int near = (z + 8) / 16 * 16;
        if (near >= 16 && near <= zmax && z - near > -4 && z - near < 4) z = near;
        else if (z >= 7 && z <= 9) z = 8;
        if (z != w->zoom16) { damage_win(w); w->zoom16 = z; damage_win(w); }
    } else if (w->resizable) {
        int nw = cx - (w->x + WM_PAD), nh = cy - (w->y + WM_HEAD);
        int maxw = w->cap_w, maxh = w->cap_h;
        int sw = scr->w - w->x - 2 * WM_PAD, sh = scr->h - w->y - WM_HEAD - WM_PAD;
        if (sw < maxw) maxw = sw;
        if (sh < maxh) maxh = sh;
        if (nw > maxw) nw = maxw;
        if (nh > maxh) nh = maxh;
        nw = nw / w->step_w * w->step_w; nh = nh / w->step_h * w->step_h;
        if (nw < w->min_w) nw = w->min_w;
        if (nh < w->min_h) nh = w->min_h;
        if (nw != w->content.w || nh != w->content.h) { damage_win(w); owner_resize(id, nw, nh); damage_win(w); }
    }
}

static int gets_input(const win_t* w) { return w->user || w->input; }

static void click(int px, int py) {
    int fs = fs_top();
    if (fs >= 0) {
        if (pill_visible(fs) && click_pill(fs, px, py)) return;
        if (gets_input(&win[fs])) capture = fs;
        return;
    }
    if (py < WM_TOP_H) {
        int t = tab_hit(px, py);
        if (t >= 0 && ops.tab) ops.tab(t);
        return;
    }
    int si = side_hit(px, py);
    if (si >= 0) { side_click(si); return; }
    int id = hit_window(px, py);
    if (id < 0) return;
    win_t* w = &win[id];
    if (w->full) { wm_raise(id); return; }
    wm_raise(id);
    if (grip_hit(w, px, py)) { resizing = id; return; }
    int rx = px - w->x, ry = py - w->y;
    if (ry < WM_HEAD) {
        int k = ctl_hit(w, px, py);
        if (k == 1) { close_req(id); return; }
        if (k == 2) { wm_minimize(id); return; }
        if (k == 3) { wm_set_fullscreen(id, 1); return; }
        if (!w->pinned) { dragging = id; drag_ox = px - w->x; drag_oy = py - w->y; }
    } else if (gets_input(w) && rx >= WM_PAD && rx < WM_PAD + disp_w(w) && ry < WM_HEAD + disp_h(w)) {
        capture = id;
    }
}

static void update_hover(void) {
    int fs = fs_top();
    int sh = fs >= 0 ? -1 : side_hit(cx, cy);
    if (sh != side_hover) { side_hover = sh; damage_side(); }
    int th = fs >= 0 ? -1 : tab_hit(cx, cy);
    if (th != tab_hover) { tab_hover = th; damage_top(); }
    int lh = -1;
    if (fs < 0 && cy >= WM_TOP_H) {
        int id = hit_window(cx, cy);
        if (id >= 0 && !win[id].pinned && !win[id].full && cy < win[id].y + WM_HEAD && cx < win[id].x + 70) lh = id;
    }
    if (lh != light_hover) {
        if (light_hover >= 0 && win[light_hover].used) damage(win[light_hover].x, win[light_hover].y, win[light_hover].x + 72, win[light_hover].y + WM_HEAD);
        light_hover = lh;
        if (lh >= 0) damage(win[lh].x, win[lh].y, win[lh].x + 72, win[lh].y + WM_HEAD);
    }
}

void wm_mouse(int ddx, int ddy, unsigned buttons) {
    damage_cursor();
    cx += ddx; cy += ddy;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > scr->w - 1) cx = scr->w - 1;
    if (cy > scr->h - 1) cy = scr->h - 1;
    damage_cursor();
    update_hover();

    int left = buttons & 1, was = prev_buttons & 1;
    if (left && !was) click(cx, cy);
    if ((buttons & 2) && !(prev_buttons & 2) && fs_top() < 0 && ops.side_alt) {      /* правая кнопка на значке */
        int si = side_hit(cx, cy);
        if (si >= 0) { ops.side_alt(si); prev_buttons = buttons; return; }
    }
    if (!left) { dragging = -1; resizing = -1; }
    if (left && resizing >= 0 && win[resizing].used) apply_resize(resizing);
    if (left && dragging >= 0 && win[dragging].used) {
        win_t* w = &win[dragging];
        int nx = cx - drag_ox, ny = cy - drag_oy;
        if (nx < 80 - outer_w(w)) nx = 80 - outer_w(w);
        if (nx > scr->w - 80) nx = scr->w - 80;
        if (ny < WM_TOP_H) ny = WM_TOP_H;
        if (ny > scr->h - WM_HEAD) ny = scr->h - WM_HEAD;
        if (nx != w->x || ny != w->y) {
            damage_win(w);
            w->x = nx; w->y = ny;
            damage_win(w);
        }
    }
    int fs = fs_top();
    if (ops.input && dragging < 0 && resizing < 0) {
        int target = capture;
        if (target < 0) {
            if (fs >= 0 && gets_input(&win[fs]) && !(pill_visible(fs) && cy < PILL_ZONE)) target = fs;
            else if (fs < 0 && cy >= WM_TOP_H && side_hit(cx, cy) < 0) {
                int hid = hit_window(cx, cy);
                if (hid >= 0 && gets_input(&win[hid]) && !win[hid].full) {
                    const win_t* w = &win[hid];
                    int rx = cx - w->x, ry = cy - w->y;
                    if (rx >= WM_PAD && rx < WM_PAD + disp_w(w) && ry >= WM_HEAD && ry < WM_HEAD + disp_h(w)) target = hid;
                }
            }
        }
        if (target >= 0 && win[target].used) {
            const win_t* w = &win[target];
            int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
            int rx = (cx - ox) * w->content.w / dw, ry = (cy - oy) * w->content.h / dh;
            if (cx < ox) rx = 0;
            if (cy < oy) ry = 0;
            if (rx >= w->content.w) rx = w->content.w - 1;
            if (ry >= w->content.h) ry = w->content.h - 1;
            if (ddx || ddy) ops.input(target, 3, rx, ry, buttons & 7);
            if ((buttons & 7) != (prev_buttons & 7)) ops.input(target, 4, rx, ry, buttons & 7);
        }
    }
    if (!(buttons & 1)) capture = -1;
    prev_buttons = buttons;
}

/* --- рисование (в поверхность clip, координаты экрана сдвинуты на ox,oy) --- */

/* Масштабированная копия. Без общих таблиц: композитор рисует полосы экрана на нескольких ядрах одновременно.
   Номер столбца источника (i - dx) * w / dw ведётся приращениями - точно то же значение, без деления на точку. */
static void blit_scaled(surface_t* c, int dx, int dy, int dw, int dh, const surface_t* src) {
    if (dw == src->w && dh == src->h) { gfx_blit(c, dx, dy, src, 0, 0, src->w, src->h); return; }
    if (dw <= 0 || dh <= 0) return;
    int xa = dx < 0 ? 0 : dx, xb = dx + dw > c->w ? c->w : dx + dw;
    int ya = dy < 0 ? 0 : dy, yb = dy + dh > c->h ? c->h : dy + dh;
    if (xa >= xb || ya >= yb) return;
    long n0 = (long)(xa - dx) * src->w;
    int q0 = (int)(n0 / dw);
    long r0 = n0 - (long)q0 * dw;
    for (int j = ya; j < yb; j++) {
        const uint32_t* sp = src->px + (long)((j - dy) * src->h / dh) * src->pitch;
        uint32_t* dp = &c->px[(long)j * c->pitch];
        int q = q0; long r = r0;
        for (int i = xa; i < xb; i++) {
            dp[i] = sp[q];
            r += src->w;
            while (r >= dw) { r -= dw; q++; }
        }
    }
}

static void blit_content(surface_t* c, int dx, int dy, int dw, int dh, const win_t* w) {
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s0 = w->seq ? *w->seq : 0;
        if (s0 & 1) continue;
        __asm__ volatile ("" ::: "memory");
        blit_scaled(c, dx, dy, dw, dh, &w->content);
        __asm__ volatile ("" ::: "memory");
        if (!w->seq || *w->seq == s0) return;
    }
    blit_scaled(c, dx, dy, dw, dh, &w->content);
}

static void draw_lights(surface_t* c, int x, int y, int hover) {
    static const uint32_t col[3] = { TH_CLOSE, TH_MIN, TH_FULL };
    for (int k = 0; k < 3; k++) {
        int lx = x + 18 + k * 20, ly = y + WM_HEAD / 2;
        ui_disc(c, lx, ly, LIGHT_R, col[k], 255);
        if (!hover) continue;
        uint32_t g = 0x003A1010;
        if (k == 0) { gfx_line(c, lx - 2, ly - 2, lx + 2, ly + 2, g); gfx_line(c, lx + 2, ly - 2, lx - 2, ly + 2, g); }
        else if (k == 1) gfx_hline(c, lx - 3, ly, 7, g);
        else { gfx_line(c, lx - 3, ly + 3, lx + 1, ly - 1, g); gfx_line(c, lx - 1, ly - 3, lx + 3, ly + 1, g); }
    }
}

static void draw_panel(surface_t* c, int ox, int oy, int id) {
    const win_t* w = &win[id];
    int x = w->x + ox, y = w->y + oy, ow = outer_w(w), oh = outer_h(w);
    int focused = w->pinned || id == wm_top();
    uint32_t accent = accent_of(w);
    if (focused && !w->pinned) ui_glow(c, x, y, ow, oh, PANEL_R, WM_GLOW, accent, 150);
    ui_round_rect(c, x, y, ow, oh, PANEL_R, TH_PANEL1, 255);
    ui_round_rect(c, x, y, ow, WM_HEAD, PANEL_R, TH_PANEL0, 255);              /* заголовок светлее */
    gfx_rect(c, x + 1, y + PANEL_R, ow - 2, WM_HEAD - PANEL_R, TH_PANEL0);
    for (int i = 0; i < ow - 2 * PANEL_R; i++)                                   /* неоновая черта под заголовком */
        ui_blend(c, x + PANEL_R + i, y + WM_HEAD - 1, accent, (unsigned)(focused ? 70 : 25));
    ui_round_frame(c, x, y, ow, oh, PANEL_R, focused ? ui_mix(TH_BORDER, accent, 150) : TH_BORDER, 255);
    if (!w->pinned) draw_lights(c, x, y, light_hover == id);
    int tw = gfx_text_width(w->title);
    int tx = x + (ow - tw) / 2;
    if (!w->pinned && tx < x + 80) tx = x + 80;
    if (w->pinned) { gfx_rect(c, x + 12, y + 11, 3, 14, accent); tx = x + 22; }
    ui_text_bold(c, tx, y + 10, w->title, focused ? TH_TEXT : TH_TEXT_DIM);
    char sub[24]; int sn = 0;
    if (w->zoomable && w->zoom16 != 16) {
        int z = w->zoom16 * 100 / 16; char dg[4]; int k = 0;
        do { dg[k++] = (char)('0' + z % 10); z /= 10; } while (z && k < 4);
        while (k) sub[sn++] = dg[--k];
        sub[sn++] = '%'; sub[sn++] = ' ';
    }
    for (int i = 0; w->sub[i] && sn < 22; i++) sub[sn++] = w->sub[i];
    sub[sn] = 0;
    if (sn) gfx_text(c, x + ow - 14 - gfx_text_width(sub), y + 10, sub, TH_TEXT_FAINT, -1);
    blit_content(c, x + WM_PAD, y + WM_HEAD, disp_w(w), disp_h(w), w);
    if (w->resizable || w->zoomable) {
        uint32_t gc = resizing == id ? TH_CYAN : 0x0035557A;
        int bx = x + ow - 6, by = y + oh - 6;
        for (int a = 3; a <= 9; a += 3) gfx_line(c, bx - a, by, bx, by - a, gc);
    }
}

static void draw_fullscreen(surface_t* c, int ox, int oy, int id) {
    const win_t* w = &win[id];
    int fx, fy, dw, dh; geom(w, &fx, &fy, &dw, &dh);
    gfx_rect(c, 0, 0, c->w, c->h, 0x0004070E);
    blit_content(c, fx + ox, fy + oy, dw, dh, w);
}

static void draw_pill(surface_t* c, int ox, int oy, int fs, int show) {
    const win_t* w = &win[fs];
    if (!show) { ui_round_rect(c, scr->w / 2 - 24 + ox, 4 + oy, 48, 4, 2, TH_CYAN, 110); return; }
    int x, y, pw, ph, n; pill_rect(w, &x, &y, &pw, &ph, &n);
    x += ox; y += oy;
    ui_round_rect(c, x, y, pw, ph, 18, TH_BAR0, 240);
    ui_round_frame(c, x, y, pw, ph, 18, ui_mix(TH_BORDER, TH_CYAN, 120), 255);
    static const uint32_t col[3] = { TH_CLOSE, TH_MIN, TH_FULL };
    for (int k = 0; k < n; k++) ui_disc(c, x + 12 + k * 36 + 18, y + 18, 8, col[k], 255);
}

static void meter_color(unsigned pct, uint32_t* c) { *c = pct < 60 ? TH_GREEN : pct < 85 ? TH_AMBER : TH_RED; }

static void draw_topbar(surface_t* c, int ox, int oy) {
    int y = oy, W = scr->w;
    ui_vgrad(c, ox, y, W, WM_TOP_H, TH_BAR0, TH_BAR1);
    for (int i = 0; i < W; i++) ui_blend(c, ox + i, y + WM_TOP_H - 1, TH_CYAN, (unsigned)(i < W / 2 ? 30 + i * 60 / W : 90 - i * 60 / W));
    icon_draw(c, ox + 30, y + 20, 30, ICON_BRAIN, TH_CYAN, 1);
    ui_text_smooth(c, ox + 52, y + 8, "ОС Кира", 24, TH_TEXT);
    int act = active_tab();
    for (int i = 0; i < tab_n; i++) {
        int tx = tab_x(i) + ox, tw = tab_w(i);
        if (i == act) {
            ui_round_rect(c, tx, y + 7, tw, TAB_H, 7, TH_CYAN, 46);
            ui_round_frame(c, tx, y + 7, tw, TAB_H, 7, TH_CYAN, 120);
        } else if (i == tab_hover) ui_round_rect(c, tx, y + 7, tw, TAB_H, 7, TH_CYAN, 20);
        gfx_text(c, tx + 11, y + 12, tabs[i].label, i == act ? TH_CYAN : (i == tab_hover ? TH_TEXT : TH_TEXT_DIM), -1);
    }
    /* справа налево: здоровье, память, ядра, время, дата */
    int rx = W - 14 + ox;
    if (m_health >= 0) {
        char t[8]; int n = 0, v = m_health; char d[4]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3);
        while (k) t[n++] = d[--k];
        t[n] = 0;
        uint32_t hc = m_health >= 70 ? TH_GREEN : m_health >= 50 ? TH_AMBER : TH_RED;
        rx -= gfx_text_width(t);
        ui_text_bold(c, rx, y + 12, t, hc);
        rx -= 14;
        ui_disc(c, rx + 4, y + 20, 4, hc, 255);
        ui_disc(c, rx + 4, y + 20, 7, hc, 50);
        rx -= 16;
    }
    if (m_mem >= 0) {                                  /* память: «батарейка» с заполнением */
        int bw = 26, bh = 12, bx = rx - bw, by = y + 14;
        uint32_t mc; meter_color((unsigned)m_mem, &mc);
        ui_round_frame(c, bx, by, bw, bh, 3, TH_TEXT_DIM, 200);
        gfx_rect(c, bx + bw, by + 3, 2, 6, TH_TEXT_DIM);
        int fw = (bw - 4) * m_mem / 100; if (fw < 1) fw = 1;
        ui_round_rect(c, bx + 2, by + 2, fw, bh - 4, 2, mc, 255);
        rx = bx - 8;
        char t[8]; int n = 0, v = m_mem; char d[4]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3);
        while (k) t[n++] = d[--k];
        t[n++] = '%'; t[n] = 0;
        rx -= gfx_text_width(t);
        gfx_text(c, rx, y + 12, t, TH_TEXT_DIM, -1);
        rx -= 14;
    }
    if (m_cores > 0) {                                 /* ядра: столбики, как уровень сигнала */
        int shown = m_cores > 16 ? 16 : m_cores, per = (m_cores + shown - 1) / shown;
        int bw = 4, gap = 2, bx = rx - shown * (bw + gap);
        for (int i = 0; i < shown; i++) {
            unsigned sum = 0, cnt = 0;
            for (int k = i * per; k < (i + 1) * per && k < m_cores; k++) { sum += m_busy[k]; cnt++; }
            unsigned p = cnt ? sum / cnt : 0;
            uint32_t col; meter_color(p, &col);
            gfx_rect(c, bx + i * (bw + gap), y + 10, bw, 20, 0x00142238);
            int hgt = (int)(p * 20 / 100); if (hgt < 2) hgt = 2;
            gfx_rect(c, bx + i * (bw + gap), y + 30 - hgt, bw, hgt, col);
        }
        rx = bx - 16;
    }
    if (clock_hm[0]) {
        rx -= gfx_text_width(clock_hm);
        ui_text_bold(c, rx, y + 12, clock_hm, TH_TEXT);
        rx -= 20;
        gfx_rect(c, rx + 9, y + 12, 1, 16, TH_BORDER);
        const char* d = clock_date;
        int left_limit = tab_n ? tab_x(tab_n - 1) + tab_w(tab_n - 1) + 16 + ox : 200 + ox;
        while (*d && rx - gfx_text_width(d) < left_limit) {                /* не хватает места: без дня недели */
            const char* comma = d; while (*comma && *comma != ',') comma++;
            if (!*comma) { d = ""; break; }
            d = comma + 2;
        }
        rx -= gfx_text_width(d);
        gfx_text(c, rx, y + 12, d, TH_TEXT_DIM, -1);
    }
}

static void draw_side(surface_t* c, int ox, int oy) {
    if (!side_n) return;
    int x = SIDE_X + ox, y = WM_TOP_H + 10 + oy, w = TILE + 12, h = side_panel_h();
    ui_round_rect(c, x, y, w, h, 16, TH_BAR0, 255);
    ui_round_frame(c, x, y, w, h, 16, ui_mix(TH_BORDER, TH_CYAN, 60), 255);
    int top = wm_top();
    for (int i = 0; i < side_n; i++) if (side[i].temp && !side[i].bottom) {
        int pinned = 0; for (int j = 0; j < i; j++) if (!side[j].bottom && !side[j].temp) pinned = 1;
        if (pinned) gfx_rect(c, SIDE_X + 12 + ox, side_item_y(i) - SEP_GAP / 2 - TILE_GAP / 2 + oy, TILE - 12, 1, ui_mix(TH_BORDER, TH_CYAN, 90));
        break;
    }
    for (int i = 0; i < side_n; i++) {
        int id = wm_find(side[i].tag);
        int running = id >= 0, shown = running && win[id].visible, focused = shown && id == top;
        uint32_t col = side[i].color;
        int tx = SIDE_X + 6 + ox, ty = side_item_y(i) + oy;
        uint32_t fill = i == side_hover ? ui_mix(TH_TILE, col, 40) : TH_TILE;
        if (focused) ui_glow(c, tx, ty, TILE, TILE, 12, 3, col, 140);
        ui_round_rect(c, tx, ty, TILE, TILE, 12, fill, 255);
        ui_round_frame(c, tx, ty, TILE, TILE, 12, focused ? col : ui_mix(TH_BORDER, col, i == side_hover ? 160 : 70), 255);
        icon_draw(c, tx + TILE / 2, ty + TILE / 2, 26, side[i].icon, running ? col : ui_mix(col, TH_TEXT_FAINT, 90), 1);
        if (side[i].badge) {                                /* буква программы в углу: одинаковые значки различимы */
            char b[2] = { side[i].badge, 0 };
            ui_disc(c, tx + TILE - 9, ty + TILE - 9, 8, TH_BAR0, 255);
            ui_ring(c, tx + TILE - 9, ty + TILE - 9, 7, 8, col, 255);
            gfx_text(c, tx + TILE - 13, ty + TILE - 17, b, TH_TEXT, -1);
        }
        if (running) ui_round_rect(c, SIDE_X + 1 + ox, ty + TILE / 2 - (shown ? 8 : 3), 3, shown ? 16 : 6, 2, shown ? col : TH_TEXT_FAINT, 255);
    }
    if (side_hover >= 0) {                                   /* подпись к значку под курсором */
        const char* hint = side[side_hover].hint;
        int th = hint ? 44 : 26;
        int ty = side_item_y(side_hover) + oy + TILE / 2 - th / 2;
        int lw = gfx_text_width(side[side_hover].label) + 20;
        if (hint && gfx_text_width(hint) + 20 > lw) lw = gfx_text_width(hint) + 20;
        int lx = SIDE_X + TILE + 22 + ox;
        ui_round_rect(c, lx, ty, lw, th, 8, TH_BAR0, 245);
        ui_round_frame(c, lx, ty, lw, th, 8, side[side_hover].color, 200);
        gfx_text(c, lx + 10, ty + 5, side[side_hover].label, TH_TEXT, -1);
        if (hint) gfx_text(c, lx + 10, ty + 23, hint, TH_TEXT_FAINT, -1);
    }
}

static void draw_cursor(surface_t* c, int ox, int oy) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            char ch = cursor_art[j][i];
            if (ch == '#') gfx_pixel(c, cx + i + ox, cy + j + oy, 0x00051018);
            else if (ch == '.') gfx_pixel(c, cx + i + ox, cy + j + oy, 0x00FFFFFF);
        }
}

static int cull_enabled = 1;
void wm_set_culling(int on) { cull_enabled = on; }

/* Признаки, меняющие всю картинку (полноэкранный режим, плашка, вкладка), - до сборки изменённых мест. */
static int compose_prepare(void) {
    int fs = fs_top();
    if (fs != last_fs) { last_fs = fs; damage(0, 0, scr->w, scr->h); }
    int pill = pill_visible(fs);
    if (pill != last_pill) { last_pill = pill; damage_pill(); }
    int at = active_tab();
    if (at != last_tab) { last_tab = at; damage_top(); }
    return fs;
}

/* Собрать прямоугольник экрана [x0,x1)x[y0,y1). Только читает состояние оконного менеджера: несколько
   прямоугольников собираются параллельно, пока поток дисплея ждёт (wm_compose_par). */
static void compose_rect(int fs, int x0, int y0, int x1, int y1) {
    int rw = x1 - x0, rh = y1 - y0;
    int pill = last_pill;
    surface_t clip = { scr->px + (long)y0 * scr->pitch + x0, rw, rh, scr->pitch };
    int ox = -x0, oy = -y0;

    /* Ищем самое верхнее окно, которое целиком закрывает изменённую область: всё, что под ним, рисовать незачем.
       Окно сверху «задевает» область вместе со своим свечением. */
    int seq[WM_MAX_WIN], ns = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < zcount; i++) {
            const win_t* wn = &win[zorder[i]];
            if (wn->visible && wn->pinned == (pass == 0)) seq[ns++] = zorder[i];
        }
    int start = -1, content_only = 0;
    if (fs < 0 && cull_enabled) {
        for (int k = ns - 1; k >= 0 && start < 0; k--) {
            const win_t* wn = &win[seq[k]];
            int ccx = wn->x + WM_PAD, ccy = wn->y + WM_HEAD, cw = disp_w(wn), ch = disp_h(wn);
            int above_hits = 0;
            for (int m = k + 1; m < ns && !above_hits; m++) {
                const win_t* up = &win[seq[m]];
                if (x0 < up->x + outer_w(up) + WM_GLOW && x1 > up->x - WM_GLOW &&
                    y0 < up->y + outer_h(up) + WM_GLOW && y1 > up->y - WM_GLOW) above_hits = 1;
            }
            if (above_hits) continue;
            if (x0 >= ccx && y0 >= ccy && x1 <= ccx + cw && y1 <= ccy + ch) { start = k; content_only = 1; }
            else if (x0 >= wn->x + PANEL_R && y0 >= wn->y + PANEL_R &&
                     x1 <= wn->x + outer_w(wn) - PANEL_R && y1 <= wn->y + outer_h(wn) - PANEL_R) start = k;
            else if (x0 < wn->x + outer_w(wn) + WM_GLOW && x1 > wn->x - WM_GLOW && y0 < wn->y + outer_h(wn) + WM_GLOW && y1 > wn->y - WM_GLOW) break;
        }
    }
    if (content_only) {
        const win_t* wn = &win[seq[start]];
        blit_content(&clip, wn->x + WM_PAD + ox, wn->y + WM_HEAD + oy, disp_w(wn), disp_h(wn), wn);
    } else {
        if (start < 0) {
            if (bg) gfx_blit(&clip, 0, 0, &bgs, x0, y0, rw, rh);
            else gfx_rect(&clip, 0, 0, rw, rh, TH_BG0);
            if (fs < 0) brainfx_draw(&clip, ox, oy, anim_ms);
            start = 0;
        }
        for (int k = start; k < ns; k++) {
            const win_t* wn = &win[seq[k]];
            if (wn->full) draw_fullscreen(&clip, ox, oy, seq[k]);
            else draw_panel(&clip, ox, oy, seq[k]);
        }
    }
    if (fs < 0) {
        if (y0 < WM_TOP_H) draw_topbar(&clip, ox, oy);
        if (x0 < WM_SIDE_W + 300) draw_side(&clip, ox, oy);
    } else draw_pill(&clip, ox, oy, fs, pill);
    draw_cursor(&clip, ox, oy);
}

static void tiles_clear(void) {
    int tw = (scr->w + DTILE - 1) / DTILE, th = (scr->h + DTILE - 1) / DTILE;
    if (tw > TMAX_X) tw = TMAX_X;
    if (th > TMAX_Y) th = TMAX_Y;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) tdirty[y][x] = 0;
}

/* Одна охватывающая область (проверки и простые случаи). */
int wm_compose(int* x, int* y, int* w, int* h) {
    int fs = compose_prepare();
    if (!dmg_valid) return 0;
    int x0 = dx0, y0 = dy0, x1 = dx1, y1 = dy1;
    dmg_valid = 0;
    tiles_clear();
    compose_rect(fs, x0, y0, x1, y1);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

/* ---- параллельная сборка ---- */
static wm_runner_t runner;
void wm_set_runner(wm_runner_t r) { runner = r; }

typedef struct { int16_t x0, y0, x1, y1; } job_t;
static job_t jobs[TMAX_X * TMAX_Y / 2 + TMAX_Y];
static uint32_t njobs;
static int job_fs;
static void (*job_present)(int x, int y, int w, int h);

static void run_job(void* ctx, uint32_t i) {
    (void)ctx;
    const job_t* j = &jobs[i];
    compose_rect(job_fs, j->x0, j->y0, j->x1, j->y1);
    if (job_present) job_present(j->x0, j->y0, j->x1 - j->x0, j->y1 - j->y0);   /* полоса - сразу на экран, тем же ядром */
}

uint32_t wm_compose_par(void (*present)(int x, int y, int w, int h), uint64_t* pixels) {
    int fs = compose_prepare();
    if (!dmg_valid) return 0;
    dmg_valid = 0;
    int tw = (scr->w + DTILE - 1) / DTILE, th = (scr->h + DTILE - 1) / DTILE;
    if (tw > TMAX_X) tw = TMAX_X;
    if (th > TMAX_Y) th = TMAX_Y;
    njobs = 0;
    uint64_t px = 0;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; ) {
            if (!tdirty[ty][tx]) { tx++; continue; }
            int a = tx;
            while (tx < tw && tdirty[ty][tx]) tdirty[ty][tx++] = 0;
            job_t j = { (int16_t)(a * DTILE), (int16_t)(ty * DTILE), (int16_t)(tx * DTILE > scr->w ? scr->w : tx * DTILE),
                        (int16_t)((ty + 1) * DTILE > scr->h ? scr->h : (ty + 1) * DTILE) };
            /* та же полоса столбцов, что у строки выше, - одно задание выше ростом (меньше заданий, меньше накладных) */
            if (njobs && jobs[njobs - 1].x0 == j.x0 && jobs[njobs - 1].x1 == j.x1 && jobs[njobs - 1].y1 == j.y0 &&
                jobs[njobs - 1].y1 - jobs[njobs - 1].y0 < 4 * DTILE) jobs[njobs - 1].y1 = j.y1;
            else if (njobs < sizeof jobs / sizeof jobs[0]) jobs[njobs++] = j;
            px += (uint64_t)(j.x1 - j.x0) * (uint64_t)(j.y1 - j.y0);
        }
    }
    job_fs = fs; job_present = present;
    if (runner) runner(run_job, 0, njobs);
    else for (uint32_t i = 0; i < njobs; i++) run_job(0, i);
    if (pixels) *pixels = px;
    return njobs;
}
