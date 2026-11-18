#include "wm.h"
#include "gfxui.h"
#include "icons.h"
#include "brainfx.h"
#include "wallpaper.h"
#include "theme.h"
#include "input.h"

#define CUR_W 12
#define CUR_H 19
#define PANEL_R 10
#define TILE 48                    /* плитка значка Dock */
#define TILE_GAP 12
#define PILL_ZONE 56               /* в полноэкранном режиме панель кнопок показывается, когда курсор выше этой черты */
#define LIGHT_R 6                  /* «светофор» окна */

typedef struct {
    int used, visible, closable, pinned, user, full, input;
    int resizable, zoomable;
    int zoom16;
    int cap_w, cap_h, min_w, min_h, step_w, step_h, norm_w, norm_h;
    int x, y;
    char title[64];
    char sub[24];
    char tag[16];
    surface_t content;
    const volatile uint32_t* seq;
    uint32_t* shadow;                 /* копия содержимого, как его видит композитор (для wm_invalidate_diff) */
    int shadow_ok, sh_w, sh_h;
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
static int last_dmg[4];
void wm_last_damage(int* b) { for (int i = 0; i < 4; i++) b[i] = last_dmg[i]; }     /* охватывающая рамка последнего собранного кадра (замеры) */
static wm_side_item_t side[WM_SIDE_MAX];
static int side_n, side_hover = -1, side_hover_desk;
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
static int m_bat_on, m_bat_pct = -1, m_bat_chg;
static int fx_level = 3;
static uint32_t anim_ms;
#define WM_NOTIFY_MAX 3
#define WM_NOTIFY_TITLE 40
#define WM_NOTIFY_TEXT 112
typedef struct {
    char title[WM_NOTIFY_TITLE];
    char text[WM_NOTIFY_TEXT];
    uint32_t expires_ms;
    uint8_t level;
} wm_notice_t;
static wm_notice_t notices[WM_NOTIFY_MAX];
static int notice_count;
static int brain_cx, brain_cy;

#define SYS_ZONE_W 200
static void damage_home(void);
static int topbar_right(surface_t* c, int ox, int oy, int dry);
static int home_open = 1;                                /* обзор системы: по клику на системную зону, по умолчанию скрыт */

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
/* В плитке помнится не флаг, а рамка изменённого внутри неё: курсор (12x19 точек) перерисовывает свои точки, а не плитку 64x64. */
typedef struct { int16_t x0, y0, x1, y1; } tbox_t;       /* x1 <= x0 - плитка чистая */
static tbox_t tdirty[TMAX_Y][TMAX_X];

static struct { uintptr_t ra; uint32_t n; } inv_tab[16];                   /* кто чаще всего даёт крупные изменения (замеры) */
static void inv_note(uintptr_t ra) {
    int k = 0, free_i = -1;
    for (; k < 16; k++) { if (inv_tab[k].n && inv_tab[k].ra == ra) break; if (!inv_tab[k].n && free_i < 0) free_i = k; }
    if (k < 16) inv_tab[k].n++;
    else if (free_i >= 0) { inv_tab[free_i].ra = ra; inv_tab[free_i].n = 1; }
}
__attribute__((noinline)) static void damage(int x0, int y0, int x1, int y1) {
    if ((long)(x1 - x0) * (y1 - y0) > 8000) inv_note((uintptr_t)__builtin_return_address(0));
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
            for (int tx = x0 / DTILE; tx <= tx1; tx++) {
                tbox_t* b = &tdirty[ty][tx];
                int ax = tx * DTILE > x0 ? tx * DTILE : x0, ay = ty * DTILE > y0 ? ty * DTILE : y0;
                int bx = (tx + 1) * DTILE < x1 ? (tx + 1) * DTILE : x1, by = (ty + 1) * DTILE < y1 ? (ty + 1) * DTILE : y1;
                if (tx == TMAX_X - 1 && x1 > bx) bx = x1;               /* край экрана шире карты: последняя плитка берёт остаток */
                if (ty == TMAX_Y - 1 && y1 > by) by = y1;
                if (b->x1 <= b->x0) { b->x0 = (int16_t)ax; b->y0 = (int16_t)ay; b->x1 = (int16_t)bx; b->y1 = (int16_t)by; }
                else {
                    if (ax < b->x0) b->x0 = (int16_t)ax;
                    if (ay < b->y0) b->y0 = (int16_t)ay;
                    if (bx > b->x1) b->x1 = (int16_t)bx;
                    if (by > b->y1) b->y1 = (int16_t)by;
                }
            }
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
/* Свечение и стеклянность рамки зависят от того, какое окно в фокусе: сменился верхний - перерисовать оба. */
static void focus_moved(int prev_top) {
    int now = wm_top();
    if (now == prev_top) return;
    if (prev_top >= 0 && win[prev_top].used) damage_win(&win[prev_top]);
    if (now >= 0) damage_win(&win[now]);
}
static void damage_cursor(void) { damage(cx, cy, cx + CUR_W, cy + CUR_H); }
static void damage_top(void) { damage(0, 0, scr->w, WM_TOP_H); }
/* Левый край правой капсулы (время, здоровье, батарея) без рисования. */
static int top_cap_left(void) { return topbar_right(0, 0, 0, 1) - 16; }
/* Часы, батарея и метры меняют только правую капсулу: перерисовываем её (старую и новую ширину), а не всю панель. */
static void damage_top_right(int old_left) {
    int l = top_cap_left();
    if (old_left < l) l = old_left;
    l -= 6;
    if (l < 0) l = 0;
    damage(l, 0, scr->w, WM_TOP_H);
}

static int fs_top(void) {
    for (int i = zcount - 1; i >= 0; i--) {
        const win_t* w = &win[zorder[i]];
        if (w->visible && !w->pinned) return w->full ? zorder[i] : -1;
    }
    return -1;
}

/* --- Dock (плавающая стеклянная панель значков снизу по центру) и значки рабочего стола слева --- */

#define SEP_GAP 14                 /* черта между группами: закреплённые, идущие, прижатые вправо */
#define TILE_MIN 30                /* наименьшая плитка, когда значков много */
#define GAP_MIN 6
#define DOCK_PAD 14                /* поля внутри стеклянной панели */
#define DOCK_BOT 14                /* отступ панели от нижнего края экрана */
#define DESK_MAX 5                 /* значков рабочего стола слева */
#define DESK_TILE 48
#define DESK_CELL_W 76
#define DESK_PITCH 92
static int side_ts = TILE, side_gap = TILE_GAP;     /* текущая плитка и промежуток: сжимаются, если значки не помещаются */
static int side_from_desk;                          /* последний side_hit попал в значок рабочего стола, а не в Dock */

static int side_group(int i) { return side[i].bottom ? 2 : side[i].temp ? 1 : 0; }
static int side_groups_present(void) {
    int has[3] = {0, 0, 0};
    for (int i = 0; i < side_n; i++) has[side_group(i)] = 1;
    return has[0] + has[1] + has[2];
}
static int side_seps_before(int i) {
    int has[3] = {0, 0, 0}, n = 0;
    for (int j = 0; j < side_n; j++) has[side_group(j)] = 1;
    for (int g = 0; g < side_group(i); g++) n += has[g];
    return n;
}
static int side_order(int i) {                       /* место значка слева направо: по группам, внутри - по порядку */
    int k = 0, g = side_group(i);
    for (int j = 0; j < side_n; j++) { int gj = side_group(j); if (gj < g || (gj == g && j < i)) k++; }
    return k;
}
static int side_seps(void) { int g = side_groups_present(); return g > 0 ? g - 1 : 0; }

/* Подбор размера плитки: от TILE вниз до TILE_MIN, пока все значки не поместятся по ширине экрана. */
static void side_layout(void) {
    int avail = scr->w - 40 - 2 * DOCK_PAD - side_seps() * SEP_GAP;
    side_ts = TILE; side_gap = TILE_GAP;
    for (int ts = TILE; ts >= TILE_MIN; ts--) {
        int gap = ts * TILE_GAP / TILE; if (gap < GAP_MIN) gap = GAP_MIN;
        side_ts = ts; side_gap = gap;
        if (side_n * ts + (side_n > 0 ? side_n - 1 : 0) * gap <= avail) break;
    }
}

int wm_side_capacity(void) {
    if (!scr) return 0;
    int avail = scr->w - 40 - 2 * DOCK_PAD - 2 * SEP_GAP;
    int n = (avail + GAP_MIN) / (TILE_MIN + GAP_MIN);
    return n > WM_SIDE_MAX ? WM_SIDE_MAX : n;
}
int wm_side_tile(void) { return side_ts; }

static int dock_w(void) {
    if (side_n <= 0) return 0;
    return 2 * DOCK_PAD + side_n * side_ts + (side_n - 1) * side_gap + side_seps() * SEP_GAP;
}
static int dock_h(void) { return side_ts + 20; }
static int dock_x(void) { return (scr->w - dock_w()) / 2; }
static int dock_y(void) { return scr->h - DOCK_BOT - dock_h(); }
static int side_ty(void) { return dock_y() + 10; }
static int side_item_x(int i) { return dock_x() + DOCK_PAD + side_order(i) * (side_ts + side_gap) + side_seps_before(i) * SEP_GAP; }

static int desk_on(void) { return scr->w >= 800 && scr->h >= 600; }
static int desk_item(int k) {                        /* k-й значок рабочего стола -> индекс в side[], -1 нет */
    int n = 0;
    for (int i = 0; i < side_n; i++) if (!side[i].temp && !side[i].bottom) { if (n == k) return i; n++; }
    return -1;
}
static int desk_count(void) {
    int n = 0;
    if (!desk_on()) return 0;
    while (n < DESK_MAX && desk_item(n) >= 0) n++;
    return n;
}
static int desk_cx(void) { return 8 + DESK_CELL_W / 2; }
static int desk_ty(int k) { return WM_TOP_H + 26 + k * DESK_PITCH; }

/* Область, которую нужно перерисовать при смене подсвеченного значка: весь Dock с подписью над ним и колонка слева. */
static void damage_side_full(void) {
    damage(0, scr->h - WM_DOCK_H - 96, scr->w, scr->h);
    if (desk_on()) damage(0, WM_TOP_H, WM_SIDE_W + 20, WM_TOP_H + 26 + DESK_MAX * DESK_PITCH);
}
static void damage_side(void) { damage_side_full(); }

/* Наведение мыши на значок: перерисовывается сам значок и его подпись, а не весь Dock шириной в экран
   (раньше каждый переход курсора по значкам давал около 170 тысяч точек, на медленной машине - десятки миллисекунд). */
/* Размер и место подписи над значком Dock; одно и то же считают рисование и перерисовка наведения.
   stl - строка состояния (0 - нет). */
static void tip_geom(int i, const char* stl, int* out_lx, int* out_ty, int* out_lw, int* out_th) {
    const char* desc = side[i].desc; const char* hint = side[i].hint;
    int lines = 1 + (desc && *desc) + (stl != 0) + (hint && *hint);
    int th = 12 + lines * 18;
    int lw = gfx_text_width(side[i].label);
    if (desc && gfx_text_width(desc) > lw) lw = gfx_text_width(desc);
    if (hint && gfx_text_width(hint) > lw) lw = gfx_text_width(hint);
    if (stl && gfx_text_width(stl) > lw) lw = gfx_text_width(stl);
    lw += 20;
    if (lw > 440) lw = 440;
    if (lw > scr->w - 8) lw = scr->w - 8;
    int lx = side_item_x(i) + side_ts / 2 - lw / 2;
    if (lx < 4) lx = 4;
    if (lx + lw > scr->w - 4) lx = scr->w - 4 - lw;
    *out_lx = lx; *out_ty = dock_y() - 10 - th; *out_lw = lw; *out_th = th;
}
#define TIP_STL_MAX "свёрнуто - щелчок вернёт"      /* самая длинная строка состояния: запас по размеру при перерисовке */
static void damage_hover_item(int i, int from_desk) {
    if (i < 0 || i >= side_n) return;
    if (from_desk) {
        for (int k = 0, n = desk_count(); k < n; k++)
            if (desk_item(k) == i) { int x = desk_cx() - DESK_CELL_W / 2, y = desk_ty(k); damage(x - 8, y - 8, x + DESK_CELL_W + 8, y + DESK_TILE + 30); return; }
        damage_side_full();
        return;
    }
    int x = side_item_x(i), y = side_ty();
    damage(x - 12, y - 12, x + side_ts + 12, y + side_ts + 12);               /* значок с подсветкой */
    int lx, ty, lw, th;
    tip_geom(i, TIP_STL_MAX, &lx, &ty, &lw, &th);
    damage(lx - 4, ty - 4, lx + lw + 4, dock_y() - 4);
}

static int side_hit(int px, int py) {
    side_from_desk = 0;
    for (int i = 0; i < side_n; i++) {
        int x = side_item_x(i), y = side_ty();
        if (px >= x && px < x + side_ts && py >= y && py < y + side_ts) return i;
    }
    for (int k = 0, n = desk_count(); k < n; k++) {
        int x = desk_cx() - DESK_CELL_W / 2, y = desk_ty(k);
        if (px >= x && px < x + DESK_CELL_W && py >= y && py < y + DESK_TILE + 22) { side_from_desk = 1; return desk_item(k); }
    }
    return -1;
}

int wm_side_center(int i, int* x, int* y) {
    if (i < 0 || i >= side_n) return -1;
    *x = side_item_x(i) + side_ts / 2; *y = side_ty() + side_ts / 2;
    return 0;
}

/* --- верхняя панель --- */

#define TAB_X0 206
#define TAB_H 26
#define TAB_GAP 6

/* Панель разделов занимает только свободное место между логотипом и системными индикаторами.
   На узких экранах ячейки равномерно сжимаются, а подписи обрезаются по UTF-8, не налезая на часы. */
static int tab_right(void) {
    int reserve = scr->w < 1024 ? 110 : scr->w < 1280 ? 170 : 390;
    int right = scr->w - reserve;
    return right < TAB_X0 ? TAB_X0 : right;
}
static int tab_natural_w(int i) { return gfx_text_width(tabs[i].label) + 22; }
static int tab_compact(void) {
    if (tab_n <= 0) return 0;
    int avail = tab_right() - TAB_X0 - (tab_n - 1) * TAB_GAP;
    int natural = 0;
    for (int i = 0; i < tab_n; i++) natural += tab_natural_w(i);
    return natural > avail;
}
static int tab_w(int i) {
    int natural = tab_natural_w(i);
    if (!tab_compact()) return natural;
    int avail = tab_right() - TAB_X0 - (tab_n - 1) * TAB_GAP;
    if (avail < 0) avail = 0;
    int base = avail / tab_n, rem = avail % tab_n;
    return base + (i < rem);
}
static int tab_x(int i) {
    int x = TAB_X0;
    for (int k = 0; k < i; k++) x += tab_w(k) + TAB_GAP;
    return x;
}

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

static int bg_fancy;                                       /* 1 - прежняя заставка «мозг» (по умолчанию гладкий нейтральный фон) */
static const uint8_t* wp_rgb; static int wp_w, wp_h;      /* обои из картинки (см. wallpaper.h); без них - заставка brainfx */

void wm_desk_fancy(int on) { bg_fancy = on != 0; }      /* до wm_init: 1 - прежняя заставка «мозг» */
int wm_wallpaper_set(const uint8_t* kwp, uint64_t len) {
    const uint8_t* rgb; int w, h;
    if (wallpaper_parse(kwp, (size_t)len, &w, &h, &rgb)) return -1;
    wp_rgb = rgb; wp_w = w; wp_h = h;
    return 0;
}

static void bg_render(void) {
    int W = scr->w, H = scr->h;
    bg = (uint32_t*)ops.alloc((uint64_t)W * H * 4);
    if (!bg) return;
    bgs.px = bg; bgs.w = W; bgs.h = H; bgs.pitch = W;
    if (wp_rgb) { wallpaper_scale(bg, W, H, wp_rgb, wp_w, wp_h); return; }
    if (!bg_fancy) { for (long i = 0; i < (long)W * H; i++) bg[i] = TH_SURF_DESK; return; }   /* рабочий стол без украшений */
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
    if (s[i]) {                                       /* обрезано: не оставлять половину многобайтной буквы UTF-8 */
        int k = i; while (k > 0 && ((uint8_t)s[k] & 0xC0) == 0x80) k--;
        d[k] = 0;
    }
}

static int notice_slots(void) {
    int n = (scr->h - WM_TOP_H - 20 + 9) / 74;
    if (n < 1) n = 1;
    return n > WM_NOTIFY_MAX ? WM_NOTIFY_MAX : n;
}

static void notice_damage(void) {
    if (!scr) return;
    int bottom = WM_TOP_H + 10 + notice_slots() * 74;
    if (bottom > scr->h) bottom = scr->h;
    damage(0, WM_TOP_H, scr->w, bottom);
}

/* Вызывается из потока дисплея: фиксированная очередь не выделяет память и не блокирует другие потоки. */
void wm_notify(const char* title, const char* message, unsigned level) {
    if (!scr || !title || !message) return;
    int slots = notice_slots();
    if (notice_count >= slots) notice_count = slots - 1;
    for (int i = notice_count; i > 0; i--) notices[i] = notices[i - 1];
    wm_notice_t* n = &notices[0];
    __builtin_memset(n, 0, sizeof *n);
    str_copy(n->title, title, sizeof n->title);
    str_copy(n->text, message, sizeof n->text);
    n->level = (uint8_t)(level > WM_NOTIFY_ERROR ? WM_NOTIFY_ERROR : level);
    n->expires_ms = anim_ms + 5000;
    if (notice_count < slots) notice_count++;
    notice_damage();
}

void wm_notify_clear(void) {
    if (!scr || !notice_count) return;
    notice_count = 0;
    __builtin_memset(notices, 0, sizeof notices);
    notice_damage();
}

void wm_init(surface_t* screen, const wm_ops_t* o) {
    scr = screen;
    ops = *o;
    for (int i = 0; i < WM_MAX_WIN; i++) win[i].used = 0;
    zcount = 0; dragging = -1; resizing = -1; capture = -1; prev_buttons = 0;
    home_open = 1; side_n = 0; side_ts = TILE; side_gap = TILE_GAP; side_hover = -1; tab_n = 0; tab_hover = -1; light_hover = -1; last_tab = -2;
    last_fs = -1; last_pill = 0; cascade = 0; clock_date[0] = 0; clock_hm[0] = 0; m_cores = 0; m_mem = -1; m_health = -1;
    __builtin_memset(notices, 0, sizeof notices); notice_count = 0;
    cx = scr->w / 2; cy = scr->h / 2;
    dmg_valid = 0;
    bg_render();
    damage(0, 0, scr->w, scr->h);
}

static int fit_zoom16(int cw, int ch) {
    /* Те же границы, что и auto_place: учитывать заголовок и рамку окна, а не только содержимое. */
    int ax = scr->w >= 800 ? WM_SIDE_W + 8 : 8;
    int aw = scr->w - ax - 12;
    int ay = WM_TOP_H + 12;
    int ah = scr->h - ay - 12 - (side_n ? WM_DOCK_H : 0);     /* Dock не должен закрывать окно: подгоняем масштаб */
    int maxw = aw - 2 * WM_PAD;
    int maxh = ah - WM_HEAD - WM_PAD;
    if (maxw < 1) maxw = 1;
    if (maxh < 1) maxh = 1;
    int sx = maxw * 16 / cw, sy = maxh * 16 / ch;
    int z = sx < sy ? sx : sy;
    if (z > 16) z = 16;             /* не увеличивать интерфейс автоматически */
    if (z < ZOOM_MIN) z = ZOOM_MIN; /* ниже половины шрифт и элементы уже плохо читаются */
    return z;
}

static void auto_place(win_t* w) {
    int ow = outer_w(w), oh = outer_h(w);
    int ax = scr->w >= 800 ? WM_SIDE_W + 8 : 8, aw = scr->w - ax - 12;
    int ay = WM_TOP_H + 12, ah = scr->h - ay - 12;
    if (side_n && oh <= ah - WM_DOCK_H) ah -= WM_DOCK_H;     /* окно помещается над Dock - ставим над ним; иначе Dock чуть перекроет низ */
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
    int prev_top = wm_top();
    if (!mem) {
        /* Окна ядра должны иметь парный alloc/release: иначе закрытие окна
           навсегда теряло бы выделенные страницы. Память Ring 3 приходит извне. */
        if (!ops.alloc || !ops.release) return -1;
        mem = (uint32_t*)ops.alloc((uint64_t)capw * (uint64_t)caph * sizeof(uint32_t));
        if (!mem) return -1;
        user = 0;
    }
    win_t* w = &win[id];
    w->used = 1; w->visible = 1; w->full = 0; w->seq = 0;
    w->shadow = 0; w->shadow_ok = 0; w->sh_w = w->sh_h = 0;
    w->closable = (flags & WM_CLOSABLE) != 0; w->pinned = (flags & WM_PINNED) != 0; w->user = user;
    w->input = (flags & WM_INPUT) != 0;
    str_copy(w->title, title, sizeof(w->title));
    str_copy(w->tag, title, sizeof(w->tag));
    w->sub[0] = 0;
    w->resizable = (flags & WM_RESIZABLE) != 0 && !user;
    w->zoomable = (flags & WM_ZOOMABLE) != 0 || user;
    w->zoom16 = w->zoomable ? fit_zoom16(cw, ch) : 16;
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
    focus_moved(prev_top);
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
    int prev_top = wm_top();
    damage_win(&win[id]);
    win_t* w = &win[id];
    uint32_t* owned = w->user ? 0 : w->content.px;
    uint64_t owned_bytes = (uint64_t)(unsigned)w->cap_w * (uint64_t)(unsigned)w->cap_h * sizeof(uint32_t);
    w->used = 0; w->visible = 0;
    if (owned && ops.release) ops.release(owned, owned_bytes);
    if (w->shadow && ops.release) ops.release(w->shadow, owned_bytes);
    w->shadow = 0; w->shadow_ok = 0;
    int j = 0;
    for (int i = 0; i < zcount; i++) if (zorder[i] != id) zorder[j++] = zorder[i];
    zcount = j;
    if (dragging == id) dragging = -1;
    if (resizing == id) resizing = -1;
    if (capture == id) capture = -1;
    if (light_hover == id) light_hover = -1;
    focus_moved(prev_top);
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
    side_layout();
    if (side_hover >= n) { damage_side_full(); side_hover = -1; }      /* значка под курсором больше нет */
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
    int prev_top = wm_top();
    win[id].visible = v;
    win[id].shadow_ok = 0;                     /* пока окно было скрыто, содержимое могло уйти от копии */
    if (v) wm_raise(id);
    damage_win(&win[id]);
    focus_moved(prev_top);
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

void wm_invalidate_report(void (*out)(uintptr_t ra, uint32_t n)) { for (int i = 0; i < 16; i++) { if (inv_tab[i].n) out(inv_tab[i].ra, inv_tab[i].n); inv_tab[i].n = 0; } }
void wm_invalidate(int id) {
    if (!wm_visible(id)) return;
    win_t* w = &win[id];
    int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
    damage(ox, oy, ox + dw, oy + dh);
}


/* Перерисовать только то, что изменилось в содержимом окна. Приложения ядра (Разум, Монитор, Проводник, Настройки)
   рисуют окно целиком каждый раз, а меняется обычно пара строк; без сравнения композитор пересобирал бы весь
   прямоугольник окна (сотни тысяч точек, ~60 нс на точку). Здесь содержимое сравнивается с копией того, что уже
   показано, и в композитор уходят только изменившиеся полосы строк, каждая со своими границами по x.
   Если копии нет, размер окна менялся или окно было скрыто - перерисовывается всё окно, как раньше. */
#define DIFF_GAP 6                 /* неизменные строки меньше этого не разрывают полосу */
void wm_invalidate_diff(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    win_t* w = &win[id];
    if (!w->visible) { w->shadow_ok = 0; return; }
    int cw = w->content.w, ch = w->content.h;
    if (!w->shadow && ops.alloc && ops.release)
        w->shadow = (uint32_t*)ops.alloc((uint64_t)w->cap_w * (uint64_t)w->cap_h * sizeof(uint32_t));
    uint32_t* sh = w->shadow;
    int spitch = w->cap_w;
    if (!sh) { wm_invalidate(id); return; }
    if (!w->shadow_ok || w->sh_w != cw || w->sh_h != ch) {          /* нечего сравнивать: всё окно */
        for (int j = 0; j < ch; j++) __builtin_memcpy(sh + (long)j * spitch, w->content.px + (long)j * w->content.pitch, (size_t)cw * 4);
        w->shadow_ok = 1; w->sh_w = cw; w->sh_h = ch;
        wm_invalidate(id);
        return;
    }
    int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
    int scaled = dw != cw || dh != ch;
    int bs = -1, be = 0, bx0 = 0, bx1 = 0;                           /* текущая полоса: строки [bs,be), столбцы [bx0,bx1) */
    for (int j = 0; j <= ch; j++) {
        int x0 = 0, x1 = 0, diff = 0;
        if (j < ch) {
            const uint32_t* a = w->content.px + (long)j * w->content.pitch;
            uint32_t* b = sh + (long)j * spitch;
            int i = 0;
            while (i < cw && a[i] == b[i]) i++;
            if (i < cw) {
                int k = cw - 1;
                while (a[k] == b[k]) k--;
                x0 = i; x1 = k + 1; diff = 1;
                __builtin_memcpy(b + i, a + i, (size_t)(x1 - x0) * 4);
            }
        }
        if (diff) {
            if (bs < 0) { bs = j; bx0 = x0; bx1 = x1; }
            else { if (x0 < bx0) bx0 = x0; if (x1 > bx1) bx1 = x1; }
            be = j + 1;
        }
        if (bs >= 0 && (j == ch || (!diff && j - be >= DIFF_GAP))) {   /* полоса закончилась */
            int m = scaled ? 1 : 0;
            damage(ox + bx0 * dw / cw - m, oy + bs * dh / ch - m,
                   ox + (bx1 * dw + cw - 1) / cw + m, oy + (be * dh + ch - 1) / ch + m);
            bs = -1;
        }
    }
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
    int l0 = top_cap_left();
    str_copy(clock_date, date, sizeof clock_date);
    str_copy(clock_hm, hm, sizeof clock_hm);
    damage_top_right(l0);
    damage_home();
}

void wm_set_battery(int present, int percent, int charging) {
    present = present ? 1 : 0;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = -1;
    if (!present) { percent = -1; charging = 0; }
    if (present == m_bat_on && percent == m_bat_pct && (charging ? 1 : 0) == m_bat_chg) return;
    int l0 = top_cap_left();
    m_bat_on = present; m_bat_pct = percent; m_bat_chg = charging ? 1 : 0;
    damage_top_right(l0);
}

void wm_set_meters(const uint8_t* busy, int ncores, int mem_pct, int health) {
    if (ncores > 64) ncores = 64;
    int changed = ncores != m_cores || mem_pct != m_mem || health != m_health;
    for (int i = 0; i < ncores && !changed; i++) if (m_busy[i] / 8 != busy[i] / 8) changed = 1;   /* шаг столбика ~ 8% */
    if (!changed) return;
    int l0 = top_cap_left();
    for (int i = 0; i < ncores; i++) m_busy[i] = busy[i];
    m_cores = ncores; m_mem = mem_pct; m_health = health;
    damage_top_right(l0);
    damage_home();
}

void wm_set_fx(int level) { fx_level = level < 0 ? 0 : level > 3 ? 3 : level; }

static void anim_damage(int x0, int y0, int x1, int y1) { damage(x0, y0, x1, y1); }

static void notices_expire(uint32_t now_ms) {
    int old = notice_count, out = 0;
    for (int i = 0; i < notice_count; i++) {
        if ((int32_t)(now_ms - notices[i].expires_ms) < 0) {
            if (out != i) notices[out] = notices[i];
            out++;
        }
    }
    if (out == old) return;
    for (int i = out; i < old; i++) __builtin_memset(&notices[i], 0, sizeof notices[i]);
    notice_count = out;
    notice_damage();
}

void wm_anim(uint32_t now_ms, unsigned activity) {
    anim_ms = now_ms;
    notices_expire(now_ms);
    if (fs_top() >= 0) return;                        /* во весь экран фон не виден */
    if (!wp_rgb && bg_fancy) brainfx_anim(now_ms, fx_level, activity, anim_damage);
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

/* Сколько окон с такой меткой (одна программа может открыть несколько окон: у каждого процесса своё). */
static int tag_count(const char* tag) {
    int n = 0;
    if (!tag || !*tag) return 0;
    for (int i = 0; i < WM_MAX_WIN; i++) if (win[i].used && streq(win[i].tag, tag)) n++;
    return n;
}

/* Щелчок по значку: нет окна - запуск; окно свёрнуто или не сверху - показать и вывести вперёд; окно сверху -
   свернуть (если окон программы несколько - перейти к следующему). */
static void side_click(int i) {
    if (i < 0 || i >= side_n) return;
    int id = wm_find(side[i].tag);
    if (id < 0) { if (ops.launch) ops.launch(i); return; }
    int top = wm_top();
    if (top >= 0 && streq(win[top].tag, side[i].tag)) {
        int next = -1;
        for (int k = 1; k < WM_MAX_WIN && next < 0; k++) {
            int j = (top + k) % WM_MAX_WIN;
            if (win[j].used && streq(win[j].tag, side[i].tag)) next = j;
        }
        if (next < 0 || next == top) { wm_minimize(top); return; }
        id = next;
    } else {
        for (int j = 0; j < WM_MAX_WIN; j++)             /* из нескольких окон - сначала уже показанное */
            if (win[j].used && win[j].visible && streq(win[j].tag, side[i].tag)) { id = j; break; }
    }
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

static void launcher_click(int px, int py);
static void click(int px, int py) {
    if (wm_launcher_active()) { launcher_click(px, py); return; }       /* меню запуска открыто: щелчок только ему */
    int fs = fs_top();
    if (fs >= 0) {
        if (pill_visible(fs) && click_pill(fs, px, py)) return;
        if (gets_input(&win[fs])) capture = fs;
        return;
    }
    if (py < WM_TOP_H) {
        int t = tab_hit(px, py);
        if (t >= 0 && ops.tab) ops.tab(t);
        else if (px < 150) wm_launcher_toggle();                                   /* логотип: поиск и запуск */
        else if (px >= scr->w - SYS_ZONE_W) { damage_home(); home_open = !home_open; damage_home(); }   /* системная зона: время и состояние - вызов обзора */
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
    if (sh != side_hover) {
        damage_hover_item(side_hover, side_hover_desk);                       /* был подсвечен тот - перерисовать */
        side_hover = sh; side_hover_desk = sh >= 0 ? side_from_desk : 0;
        damage_hover_item(side_hover, side_hover_desk);                       /* и новый */
    }
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

void wm_mouse_abs(uint16_t ax, uint16_t ay, unsigned buttons) {
    if (!scr) return;
    int tx = (int)((uint32_t)ax * (uint32_t)(scr->w - 1) / 32767u);
    int ty = (int)((uint32_t)ay * (uint32_t)(scr->h - 1) / 32767u);
    wm_mouse(tx - cx, ty - cy, buttons);
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
   Уменьшение - усреднение по площади (каждый исходный пиксель входит с весом своей доли в целевом): тонкие линии букв
   не пропадают, как при выборе ближайшей точки. Увеличение - ближайшая точка (интерфейс сам не увеличивается). */
static void blit_scaled(surface_t* c, int dx, int dy, int dw, int dh, const surface_t* src) {
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
    /* Уменьшение: усреднение по площади точными долями (как при делении на dw, dh), но без деления на каждую точку:
       границы исходных столбцов идут по шагам (частное и остаток), веса строк считаются один раз на строку,
       итоговое деление на общую площадь - умножением на обратную величину (точно: сумма*площадь < 2^64). */
    enum { SC_ROWS = 64 };
    uint64_t tot = (uint64_t)sw * (uint64_t)sh;
    uint64_t inv = 0xFFFFFFFFFFFFFFFFull / tot + 1;               /* floor(n/tot) == (n*inv)>>64 для n*tot < 2^64 */
    long n0 = (long)(xa - dx) * sw;
    int q0 = (int)(n0 / dw); long r0 = n0 - (long)q0 * dw;           /* левая граница: x0 / dw */
    long n1 = n0 + sw - 1;
    int q1 = (int)(n1 / dw); long r1 = n1 - (long)q1 * dw;           /* правая граница: (x0 + sw - 1) / dw */
    for (int j = ya; j < yb; j++) {
        long y0 = (long)(j - dy) * sh, y1 = y0 + sh;              /* строка назначения покрывает [y0, y1) в долях 1/dh строки источника */
        int ry0 = (int)(y0 / dh), ry1 = (int)((y1 - 1) / dh);
        if (ry1 >= sh) ry1 = sh - 1;
        int nry = ry1 - ry0 + 1;
        uint32_t* dp = &c->px[(long)j * c->pitch];
        uint64_t wyv[SC_ROWS]; const uint32_t* sps[SC_ROWS];
        int fast = nry <= SC_ROWS;
        if (fast)
            for (int k = 0; k < nry; k++) {
                long ry = ry0 + k;
                long ya0 = ry * dh > y0 ? ry * dh : y0;
                long ya1 = (ry + 1) * dh < y1 ? (ry + 1) * dh : y1;
                wyv[k] = (uint64_t)(ya1 - ya0);
                sps[k] = src->px + ry * src->pitch;
            }
        int qa = q0, qb = q1; long ra = r0, rb = r1;
        long x0 = n0;
        for (int i = xa; i < xb; i++, x0 += sw) {
            long x1 = x0 + sw;
            int rx0 = qa, rx1 = qb;
            if (rx1 >= sw) rx1 = sw - 1;
            ra += sw; while (ra >= dw) { ra -= dw; qa++; }
            rb += sw; while (rb >= dw) { rb -= dw; qb++; }
            uint64_t sr = 0, sg = 0, sb = 0;
            for (int k = 0; k < nry; k++) {
                uint64_t wy;
                const uint32_t* sp;
                if (fast) { wy = wyv[k]; sp = sps[k]; }
                else {
                    long ry = ry0 + k;
                    long ya0 = ry * dh > y0 ? ry * dh : y0;
                    long ya1 = (ry + 1) * dh < y1 ? (ry + 1) * dh : y1;
                    wy = (uint64_t)(ya1 - ya0);
                    sp = src->px + ry * src->pitch;
                }
                for (int rx = rx0; rx <= rx1; rx++) {
                    long xa0 = (long)rx * dw > x0 ? (long)rx * dw : x0;
                    long xa1 = (long)(rx + 1) * dw < x1 ? (long)(rx + 1) * dw : x1;
                    uint64_t wgt = wy * (uint64_t)(xa1 - xa0);
                    uint32_t p = sp[rx];
                    sr += wgt * ((p >> 16) & 255); sg += wgt * ((p >> 8) & 255); sb += wgt * (p & 255);
                }
            }
            uint32_t R = (uint32_t)(((unsigned __int128)sr * inv) >> 64);
            uint32_t G = (uint32_t)(((unsigned __int128)sg * inv) >> 64);
            uint32_t B = (uint32_t)(((unsigned __int128)sb * inv) >> 64);
            dp[i] = R << 16 | G << 8 | B;
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
    /* Нейтральные монохромные контролы: действие различается пиктограммой, а не тревожным цветом. */
    for (int k = 0; k < 3; k++) {
        int lx = x + 18 + k * 20, ly = y + WM_HEAD / 2;
        uint32_t edge = hover ? (k == 0 ? TH_ALARM : TH_FG_DIM) : TH_EDGE;     /* закрыть при наведении - красный: действие необратимо */
        uint32_t glyph = hover ? (k == 0 ? TH_ALARM : TH_FG) : TH_FG_DIM;
        ui_disc(c, lx, ly, LIGHT_R, TH_SURF_TRACK, 255);
        ui_ring(c, lx, ly, LIGHT_R - 1, LIGHT_R, edge, 220);
        if (k == 0) { gfx_line(c, lx - 2, ly - 2, lx + 2, ly + 2, glyph); gfx_line(c, lx + 2, ly - 2, lx - 2, ly + 2, glyph); }
        else if (k == 1) gfx_hline(c, lx - 3, ly, 7, glyph);
        else { gfx_line(c, lx - 3, ly - 3, lx + 3, ly - 3, glyph); gfx_line(c, lx - 3, ly + 3, lx + 3, ly + 3, glyph); gfx_line(c, lx - 3, ly - 3, lx - 3, ly + 3, glyph); gfx_line(c, lx + 3, ly - 3, lx + 3, ly + 3, glyph); }
    }
}

static void draw_panel(surface_t* c, int ox, int oy, int id) {
    const win_t* w = &win[id];
    int x = w->x + ox, y = w->y + oy, ow = outer_w(w), oh = outer_h(w);
    int focused = w->pinned || id == wm_top();
    uint32_t accent = accent_of(w);
    /* Единый плоский стиль (docs/UI_DESIGN.md): непрозрачная рамка, без свечения, градиентов и бликов.
       Окно в фокусе отличается цветом заголовка и тонкой линией акцента под ним. */
    (void)accent;
    ui_round_rect_hole(c, x, y, ow, oh, PANEL_R, TH_SURF_WIN, 255, x + WM_PAD, y + WM_HEAD, disp_w(w), disp_h(w));
    ui_round_rect(c, x, y, ow, WM_HEAD, PANEL_R, focused ? TH_SURF_HEAD : TH_SURF_WIN, 255);
    if (focused) gfx_rect(c, x + 1, y + WM_HEAD - 1, ow - 2, 1, TH_ACCENT);
    else gfx_rect(c, x + 1, y + WM_HEAD - 1, ow - 2, 1, TH_EDGE);
    ui_round_frame(c, x, y, ow, oh, PANEL_R, focused ? ui_mix(TH_EDGE, TH_ACCENT, 120) : TH_EDGE, 255);
    if (!w->pinned) draw_lights(c, x, y, light_hover == id);
    int tw = gfx_text_width(w->title);
    int tx = x + (ow - tw) / 2;
    if (!w->pinned && tx < x + 80) tx = x + 80;
    if (w->pinned) tx = x + TH_S3;
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
    if (sn) {                                                                    /* подзаголовок - плашкой */
        int sw = gfx_text_width(sub);
        ui_round_rect(c, x + ow - 22 - sw, y + 7, sw + 16, 22, TH_RADIUS, TH_SURF_TRACK, 255);
        ui_round_frame(c, x + ow - 22 - sw, y + 7, sw + 16, 22, TH_RADIUS, TH_EDGE, 255);
        gfx_text(c, x + ow - 14 - sw, y + 10, sub, focused ? TH_TEXT_DIM : TH_TEXT_FAINT, -1);
    }
    blit_content(c, x + WM_PAD, y + WM_HEAD, disp_w(w), disp_h(w), w);
    if (w->resizable || w->zoomable) {
        uint32_t gc = resizing == id ? TH_ACCENT : TH_FG_FAINT;
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
    for (int k = 0; k < n; k++) {
        int bx = x + 12 + k * 36 + 18, by = y + 18;
        ui_disc(c, bx, by, 8, TH_TRACK, 255);
        ui_ring(c, bx, by, 7, 8, TH_BORDER, 230);
        if (k == 0) { gfx_line(c, bx - 2, by - 2, bx + 2, by + 2, TH_TEXT_DIM); gfx_line(c, bx + 2, by - 2, bx - 2, by + 2, TH_TEXT_DIM); }
        else if (k == 1) gfx_hline(c, bx - 3, by, 7, TH_TEXT_DIM);
        else { gfx_line(c, bx - 3, by - 3, bx + 3, by - 3, TH_TEXT_DIM); gfx_line(c, bx - 3, by + 3, bx + 3, by + 3, TH_TEXT_DIM); gfx_line(c, bx - 3, by - 3, bx - 3, by + 3, TH_TEXT_DIM); gfx_line(c, bx + 3, by - 3, bx + 3, by + 3, TH_TEXT_DIM); }
    }
}

static void meter_color(unsigned pct, uint32_t* c) { *c = pct < 60 ? TH_GREEN : pct < 85 ? TH_AMBER : TH_RED; }

/* Домашняя карточка использует только опубликованные значения; -1 означает «нет данных», а не 0%. */
static void home_metric(surface_t* c, int x, int y, int w, int ox, int oy,
                        const char* label, int value, uint32_t color) {
    gfx_text(c, x + ox, y + oy, label, TH_TEXT_DIM, -1);
    char t[16];
    if (value < 0) {
        gfx_text(c, x + w - 16 - gfx_text_width("нет данных") + ox, y + oy, "нет данных", TH_TEXT_FAINT, -1);
    } else {
        unsigned v = value > 100 ? 100u : (unsigned)value;
        int n = 0;
        if (v >= 100) { t[n++] = '1'; t[n++] = '0'; t[n++] = '0'; }
        else if (v >= 10) { t[n++] = (char)('0' + v / 10); t[n++] = (char)('0' + v % 10); }
        else t[n++] = (char)('0' + v);
        t[n++] = '%'; t[n] = 0;
        gfx_text(c, x + w - 16 - gfx_text_width(t) + ox, y + oy, t, color, -1);
    }
    ui_round_rect(c, x + ox, y + 17 + oy, w, 5, 3, TH_TRACK, 255);
    if (value > 0) {
        unsigned v = value > 100 ? 100u : (unsigned)value;
        int fill = (int)((unsigned)w * v / 100u);
        if (fill > 0) ui_round_rect(c, x + ox, y + 17 + oy, fill, 5, 3, color, 255);
    }
}

int wm_home_open(void) { return home_open; }
static void damage_home(void) {
    if (!scr || scr->w < 900 || scr->h < 620) return;
    damage(scr->w - 318, scr->h - 176, scr->w - 18, scr->h - 18);
}

/* Живая карточка рабочего стола: агрегаты CPU, память и здоровье Mind. */
static void draw_home(surface_t* c, int ox, int oy) {
    if (scr->w < 900 || scr->h < 620) return;
    const int w = 300, h = 158;
    int x = scr->w - 318, y = scr->h - 176;
    ui_round_rect(c, x + ox, y + oy, w, h, TH_RADIUS, TH_SURF_BAR, 255);
    ui_round_frame(c, x + ox, y + oy, w, h, TH_RADIUS, TH_EDGE, 255);
    ui_text_bold(c, x + 16 + ox, y + 11 + oy, "Состояние системы", TH_FG);
    if (clock_hm[0]) gfx_text(c, x + w - 16 - gfx_text_width(clock_hm) + ox, y + 11 + oy, clock_hm, TH_FG_DIM, -1);

    int cpu = -1;
    if (m_cores > 0) {
        unsigned sum = 0;
        for (int i = 0; i < m_cores; i++) sum += m_busy[i] > 100 ? 100u : m_busy[i];
        cpu = (int)(sum / (unsigned)m_cores);
    }
    uint32_t cc = TH_TEXT_FAINT, mc = TH_TEXT_FAINT, hc = TH_TEXT_FAINT;
    if (cpu >= 0) meter_color((unsigned)cpu, &cc);
    if (m_mem >= 0) meter_color((unsigned)m_mem, &mc);
    if (m_health >= 0) hc = m_health >= 70 ? TH_GREEN : m_health >= 50 ? TH_AMBER : TH_RED;   /* здоровье: чем больше, тем лучше */
    home_metric(c, x + 16, y + 39, w - 32, ox, oy, "Процессор", cpu, cc);
    home_metric(c, x + 16, y + 77, w - 32, ox, oy, "Память", m_mem, mc);
    home_metric(c, x + 16, y + 115, w - 32, ox, oy, "Здоровье Разума", m_health, hc);
}

/* Правая стеклянная капсула: здоровье, время, дата. dry - только посчитать, где она начинается (рисовать ещё нечем). */
static int topbar_right(surface_t* c, int ox, int oy, int dry) {
    int W = scr->w, y = oy;
    int rx = W - 22 + ox;
    if (W >= 1024 && m_bat_on) {                          /* батарея: рамка 22x11 с выступом, заливка по заряду, число; зелёный - зарядка */
        char t[8]; int n = 0, k = 0; char d[4];
        if (m_bat_pct >= 0) { int v = m_bat_pct; do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3); while (k) t[n++] = d[--k]; t[n++] = '%'; }
        else t[n++] = '?';
        t[n] = 0;
        uint32_t bc = m_bat_chg ? TH_GREEN : m_bat_pct < 0 ? TH_TEXT_DIM : m_bat_pct <= 15 ? TH_RED : m_bat_pct <= 30 ? TH_AMBER : TH_TEXT;
        rx -= gfx_text_width(t);
        if (!dry) ui_text_bold(c, rx, y + 12, t, bc);
        rx -= 28;
        if (!dry) {
            int bx = rx, by = y + 14;
            gfx_rect(c, bx, by, 22, 1, bc); gfx_rect(c, bx, by + 10, 22, 1, bc);
            gfx_rect(c, bx, by, 1, 11, bc); gfx_rect(c, bx + 21, by, 1, 11, bc);
            gfx_rect(c, bx + 22, by + 3, 2, 5, bc);
            if (m_bat_pct > 0) { int fw = (18 * m_bat_pct + 99) / 100; if (fw < 1) fw = 1; gfx_rect(c, bx + 2, by + 2, fw, 7, bc); }
        }
        rx -= 14;
    }
    if (W >= 1024 && m_health >= 0) {
        char t[8]; int n = 0, v = m_health; char d[4]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3);
        while (k) t[n++] = d[--k];
        t[n] = 0;
        uint32_t hc = m_health >= 70 ? TH_GREEN : m_health >= 50 ? TH_AMBER : TH_RED;
        rx -= gfx_text_width(t);
        if (!dry) ui_text_bold(c, rx, y + 12, t, hc);
        rx -= 14;
        if (!dry) { ui_disc(c, rx + 4, y + 20, 4, hc, 255); ui_disc(c, rx + 4, y + 20, 7, hc, 50); }
        rx -= 16;
    }
    if (clock_hm[0]) {
        rx -= gfx_text_width(clock_hm);
        if (!dry) ui_text_bold(c, rx, y + 12, clock_hm, TH_TEXT);
        if (W >= 1280) {
            rx -= 20;
            if (!dry) gfx_rect(c, rx + 9, y + 12, 1, 16, TH_BORDER);
            const char* d = clock_date;
            int left_limit = tab_n ? tab_x(tab_n - 1) + tab_w(tab_n - 1) + 36 + ox : 200 + ox;
            while (*d && rx - gfx_text_width(d) < left_limit) {
                const char* comma = d; while (*comma && *comma != ',') comma++;
                if (!*comma) { d = ""; break; }
                d = comma + 2;
            }
            rx -= gfx_text_width(d);
            if (*d && !dry) gfx_text(c, rx, y + 12, d, TH_TEXT_DIM, -1);
        }
    }
    return rx;
}

static void draw_topbar(surface_t* c, int ox, int oy) {
    int y = oy, W = scr->w;
    /* Бренд слева, стеклянная пилюля с разделами, капсула с временем справа. Полосы на всю ширину нет: обои видны целиком. */
    ui_round_rect(c, ox + 10, y + 3, 184, 34, TH_RADIUS, TH_SURF_BAR, 255);
    ui_round_frame(c, ox + 10, y + 3, 184, 34, TH_RADIUS, TH_EDGE, 255);
    icon_draw(c, ox + 31, y + 20, 28, ICON_BRAIN, TH_CYAN, 1);
    ui_text_smooth(c, ox + 52, y + 4, "KIRA OS", 18, TH_TEXT);
    gfx_text(c, ox + 52, y + 21, "Рабочий стол", TH_FG_FAINT, -1);
    int px0 = TAB_X0 - 8, px1 = tab_right() + ox;
    if (tab_n && px1 > px0) {
        int last = tab_x(tab_n - 1) + tab_w(tab_n - 1) + 8;
        if (last < px1) px1 = last;
        ui_round_rect(c, px0 + ox, y + 3, px1 - px0, TAB_H + 8, TH_RADIUS, TH_SURF_BAR, 255);
        ui_round_frame(c, px0 + ox, y + 3, px1 - px0, TAB_H + 8, TH_RADIUS, TH_EDGE, 255);
    }
    int act = active_tab();
    for (int i = 0; i < tab_n; i++) {
        int tx = tab_x(i) + ox, tw = tab_w(i);
        if (i == act) {
            ui_round_rect(c, tx, y + 7, tw, TAB_H, TH_RADIUS, ui_state_bg(UI_ST_SELECTED, TH_SURF_BAR), 255);
            gfx_rect(c, tx + 2, y + 7 + TAB_H - 2, tw - 4, 2, TH_ACCENT);
        } else if (i == tab_hover) {
            ui_round_rect(c, tx, y + 7, tw, TAB_H, TH_RADIUS, ui_state_bg(UI_ST_HOVER, TH_SURF_BAR), 255);
        }
        int lw = gfx_text_width(tabs[i].label), room = tw - 16;
        if (lw > room) lw = room;
        if (tw > 20) ui_text_fit(c, tx + (tw - lw) / 2, y + 12, tabs[i].label, room,
                                      i == act ? TH_FG : (i == tab_hover ? TH_FG : TH_FG_DIM));
    }
    int left = topbar_right(c, ox, oy, 1) - 16;
    if (left > W - 40 + ox) left = W - 40 + ox;
    ui_round_rect(c, left, y + 3, W - 8 + ox - left, 34, TH_RADIUS, TH_SURF_BAR, 255);
    ui_round_frame(c, left, y + 3, W - 8 + ox - left, 34, TH_RADIUS, TH_EDGE, 255);
    topbar_right(c, ox, oy, 0);
}

/* Состояние значка: 0 - окна нет, 1 - свёрнуто, 2 - открыто, 3 - в фокусе (окно сверху). id - окно или -1. */
static int side_state(int i, int top, int* id_out) {
    int id = wm_find(side[i].tag);
    if (id < 0) { if (id_out) *id_out = -1; return 0; }
    int st = 1;                                          /* есть окно с этой меткой: берём лучшее состояние */
    for (int j = 0; j < WM_MAX_WIN; j++) {
        if (!win[j].used || !streq(win[j].tag, side[i].tag)) continue;
        if (win[j].visible && st < 2) { st = 2; id = j; }
        if (j == top) { st = 3; id = j; }
    }
    if (id_out) *id_out = id;
    return st;
}

static void draw_side_tile(surface_t* c, int i, int tx, int ty, int ts, int top, int dock) {
    int st = side_state(i, top, 0);
    int running = st > 0, shown = st >= 2, focused = st == 3;
    /* значки монохромные: цвет только у выбранного (рамка акцента) */
    int big = ts >= 36, hov = i == side_hover && side_from_desk == !dock;
    uint32_t fill = ui_state_bg(hov ? UI_ST_HOVER : UI_ST_NORMAL, TH_SURF_HEAD);
    ui_round_rect(c, tx, ty, ts, ts, TH_RADIUS, fill, 255);
    ui_round_frame(c, tx, ty, ts, ts, TH_RADIUS, focused ? TH_ACCENT : TH_EDGE, 255);
    icon_draw(c, tx + ts / 2, ty + ts / 2, ts * 26 / TILE, side[i].icon, running ? TH_FG : TH_FG_FAINT, 1);
    if (side[i].badge && big) {                         /* буква программы в углу: одинаковые значки различимы */
        char b[2] = { side[i].badge, 0 };
        ui_disc(c, tx + ts - 9, ty + ts - 9, 8, TH_SURF_BAR, 255);
        ui_ring(c, tx + ts - 9, ty + ts - 9, 7, 8, TH_EDGE, 255);
        gfx_text(c, tx + ts - 13, ty + ts - 17, b, TH_FG, -1);
    }
    int cnt = running ? tag_count(side[i].tag) : 0;
    if (cnt > 1) {                                      /* несколько окон одной программы: число в верхнем углу */
        char b[4] = { (char)(cnt > 9 ? '+' : '0' + cnt), 0, 0, 0 };
        ui_disc(c, tx + ts - 7, ty + 7, 7, TH_ACCENT, 255);
        gfx_text(c, tx + ts - 11, ty - 1, b, TH_SURF_DESK, -1);
    }
    /* индикатор под плиткой Dock: длинная яркая полоса - открыто, в фокусе (+ свечение); короткая тусклая точка - свёрнуто */
    if (dock && running) ui_round_rect(c, tx + ts / 2 - (shown ? ts * 8 / TILE : 3), ty + ts + 3, shown ? ts * 16 / TILE : 6, 3, 1, shown ? TH_ACCENT : TH_FG_FAINT, 255);
}

static void draw_side(surface_t* c, int ox, int oy) {
    int top = wm_top();
    /* значки рабочего стола слева: подпись под значком, с тенью - читается на любых обоях */
    for (int k = 0, n = desk_count(); k < n; k++) {
        int i = desk_item(k), x = desk_cx() - DESK_TILE / 2 + ox, y = desk_ty(k) + oy;
        draw_side_tile(c, i, x, y, DESK_TILE, top, 0);
        const char* lb = side[i].label;
        int lw = gfx_text_width(lb); if (lw > DESK_CELL_W - 4) lw = DESK_CELL_W - 4;
        int lx = desk_cx() - lw / 2 + ox;
        ui_text_fit(c, lx + 1, y + DESK_TILE + 6 + 1, lb, DESK_CELL_W - 4, 0x0A0E24);
        ui_text_fit(c, lx, y + DESK_TILE + 6, lb, DESK_CELL_W - 4, TH_TEXT);
    }
    if (!side_n) return;
    int ts = side_ts;
    int x = dock_x() + ox, y = dock_y() + oy, w = dock_w(), h = dock_h();
    ui_round_rect(c, x, y, w, h, TH_RADIUS * 2, TH_SURF_BAR, 255);
    ui_round_frame(c, x, y, w, h, TH_RADIUS * 2, TH_EDGE, 255);
    for (int g = 1; g < 3; g++) {                          /* чёрточки между группами */
        int first = -1;
        for (int i = 0; i < side_n; i++) if (side_group(i) == g && (first < 0 || side_order(i) < side_order(first))) first = i;
        if (first < 0 || side_order(first) == 0) continue;
        gfx_rect(c, side_item_x(first) - side_gap / 2 - SEP_GAP / 2 + ox, y + 12, 1, h - 24, TH_EDGE);
    }
    for (int i = 0; i < side_n; i++) draw_side_tile(c, i, side_item_x(i) + ox, side_ty() + oy, ts, top, 1);
    if (side_hover >= 0 && !side_from_desk) {                /* подпись над значком под курсором */
        int hi; int st = side_state(side_hover, top, &hi);
        const char* hint = side[side_hover].hint;
        const char* desc = side[side_hover].desc;
        const char* stl = st == 3 ? "в фокусе" : st == 2 ? "открыто" : st == 1 ? "свёрнуто - щелчок вернёт" : 0;
        int lx, ty, lw, th;
        tip_geom(side_hover, stl, &lx, &ty, &lw, &th);
        lx += ox; ty += oy;
        ui_round_rect(c, lx, ty, lw, th, TH_RADIUS, TH_SURF_HEAD, 255);
        ui_round_frame(c, lx, ty, lw, th, TH_RADIUS, TH_EDGE, 255);
        int ly = ty + 5;
        ui_text_fit(c, lx + 10, ly, side[side_hover].label, lw - 20, TH_TEXT); ly += 18;
        if (desc && *desc) { ui_text_fit(c, lx + 10, ly, desc, lw - 20, TH_TEXT_DIM); ly += 18; }
        if (stl) { gfx_text(c, lx + 10, ly, stl, st >= 2 ? TH_ACCENT : TH_FG_FAINT, -1); ly += 18; }
        if (hint && *hint) ui_text_fit(c, lx + 10, ly, hint, lw - 20, TH_TEXT_FAINT);
    }
}

static void draw_notifications(surface_t* c, int ox, int oy) {
    static const uint32_t accents[] = { TH_FG_DIM, TH_OK, TH_WARN, TH_ALARM };   /* сведение - нейтральное, остальное по смыслу */
    int width = scr->w - 32;
    if (width > 360) width = 360;
    if (width < 180) width = scr->w - 16;
    if (width < 80) width = scr->w;
    int x = scr->w - width - 12;
    if (x < 4) x = 4;
    for (int i = 0; i < notice_count; i++) {
        int y = WM_TOP_H + 10 + i * 74, h = 64;
        if (y + h > scr->h - 4) break;
        uint32_t accent = accents[notices[i].level];
        ui_round_rect(c, x + ox, y + oy, width, h, TH_RADIUS, TH_SURF_HEAD, 255);
        ui_round_frame(c, x + ox, y + oy, width, h, TH_RADIUS, TH_EDGE, 255);
        ui_round_rect(c, x + 7 + ox, y + 9 + oy, 3, h - 18, 1, accent, 255);
        ui_text_fit(c, x + 18 + ox, y + 9 + oy, notices[i].title, width - 30, accent);
        ui_text_fit(c, x + 18 + ox, y + 34 + oy, notices[i].text, width - 30, TH_TEXT_DIM);
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

/* ---------------- Launcher: поиск и запуск по имени (UI-3) ----------------
   Открывается по F9 или клику на логотип; печатаем часть имени (латиница и кириллица, регистр не важен), стрелки выбирают,
   Enter запускает или поднимает окно, Esc закрывает. Ничего не висит на экране, пока не вызван. */
#define LQ_MAX 40
#define LROW_H 26                                           /* строка меню (пункт или заголовок группы) */
#define LLAUNCH_MAX 48
static int launcher_open, lq_n, l_sel, l_nm;
static char lq[LQ_MAX];
static int l_match[LLAUNCH_MAX > WM_SIDE_MAX ? LLAUNCH_MAX : WM_SIDE_MAX];
static wm_launch_item_t litem[LLAUNCH_MAX]; static int l_nitems;
static const char* lgroup[8]; static int l_ngroups;
static int lrow[LLAUNCH_MAX + 8], l_nrows;                  /* что показано: >= 0 - пункт, < 0 - заголовок группы -(g+1) */

void wm_launcher_set(const wm_launch_item_t* items, int n, const char* const* groups, int ngroups) {
    l_nitems = 0; l_ngroups = 0;
    for (int i = 0; i < n && i < LLAUNCH_MAX; i++) litem[l_nitems++] = items[i];
    for (int g = 0; g < ngroups && g < 8; g++) lgroup[l_ngroups++] = groups[g];
}

static unsigned u8next(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    unsigned c = s[0];
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    *p += 1; return c;
}
static unsigned cpfold(unsigned c) {                        /* латиница и кириллица без учёта регистра */
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    return c == 0x401 ? 0x451 : c;
}
static int has_fold(const char* hay, const char* nd) {
    if (!nd[0]) return 1;
    if (!hay) return 0;
    for (const char* h = hay; *h; ) {
        const char* a = h; const char* b = nd; int ok = 1;
        while (*b) { if (!*a) { ok = 0; break; } if (cpfold(u8next(&a)) != cpfold(u8next(&b))) { ok = 0; break; } }
        if (ok) return 1;
        u8next(&h);
    }
    return 0;
}
/* Геометрия меню: общая для рисования, мыши и повреждений экрана. */
static int launcher_vis_rows(void) {
    int total = l_nitems ? l_nrows : (l_nm ? l_nm : 1);
    int cap = (scr->h - WM_TOP_H - 60 - 60) / LROW_H; if (cap < 3) cap = 3; if (cap > 14) cap = 14;
    return total < cap ? total : cap;
}
static int launcher_pw(void) { int w = scr->w - 40 < 560 ? scr->w - 40 : 560; return w; }
static void launcher_geom(int* px, int* py, int* pw, int* ph) {
    *pw = launcher_pw(); *px = (scr->w - *pw) / 2; *py = WM_TOP_H + 40;
    *ph = 8 + 32 + 8 + launcher_vis_rows() * LROW_H + 8;
}
static void launcher_damage(void) {
    int px, py, pw, ph; launcher_geom(&px, &py, &pw, &ph);
    damage(px - 8, py - 4, px + pw + 12, py + ph + 12);
    damage(0, WM_TOP_H, scr->w, WM_TOP_H + 40 + 8 + 32 + 8 + 14 * LROW_H + 24);
}
static void launcher_rebuild(void) {
    l_nm = 0; l_nrows = 0;
    if (l_nitems) {
        for (int i = 0; i < l_nitems; i++)
            if (has_fold(litem[i].label, lq) || has_fold(litem[i].tag, lq) || has_fold(litem[i].desc, lq)) l_match[l_nm++] = i;
        if (!lq_n) {                                        /* без запроса - по группам с заголовками */
            for (int g = 0; g < l_ngroups; g++) {
                int any = 0;
                for (int k = 0; k < l_nm; k++) if (litem[l_match[k]].group == g) { if (!any) lrow[l_nrows++] = -(g + 1); any = 1; lrow[l_nrows++] = l_match[k]; }
            }
            int m = 0;                                      /* порядок выбора стрелками - как на экране */
            for (int r = 0; r < l_nrows; r++) if (lrow[r] >= 0) l_match[m++] = lrow[r];
            l_nm = m;
        } else for (int k = 0; k < l_nm; k++) lrow[l_nrows++] = l_match[k];
    } else {
        for (int i = 0; i < side_n; i++) {
            if (!side[i].label || !side[i].label[0]) continue;
            if (has_fold(side[i].label, lq) || has_fold(side[i].tag, lq) || has_fold(side[i].desc, lq)) l_match[l_nm++] = i;
        }
    }
    if (l_sel >= l_nm) l_sel = l_nm ? l_nm - 1 : 0;
}
void wm_launcher_toggle(void) {
    launcher_damage();
    launcher_open = !launcher_open;
    lq_n = 0; lq[0] = 0; l_sel = 0;
    if (launcher_open) launcher_rebuild();
    launcher_damage();
}
int wm_launcher_active(void) { return launcher_open; }
int wm_launcher_count(void) { return l_nm; }

/* Запуск выбранного пункта: окно уже есть - показать (как значок панели), иначе запросить запуск. */
static void launcher_run(int k) {
    if (k < 0 || k >= l_nm) return;
    launcher_open = 0; launcher_damage();
    if (!l_nitems) { side_click(l_match[k]); return; }
    const char* tag = litem[l_match[k]].tag;
    for (int i = 0; i < side_n; i++) if (side[i].tag && tag && streq(side[i].tag, tag) && side[i].tag[0]) { side_click(i); return; }
    if (ops.launch_tag) ops.launch_tag(tag);
}

void wm_launcher_key(int key, int ch) {
    if (!launcher_open) return;
    launcher_damage();
    if (key == KEY_ESC) launcher_open = 0;
    else if (key == KEY_ENTER) {
        if (l_nm) { launcher_run(l_sel); return; }
    } else if (key == KEY_UP) { if (l_sel > 0) l_sel--; }
    else if (key == KEY_DOWN) { if (l_sel + 1 < l_nm) l_sel++; }
    else if (key == KEY_BACKSPACE) {
        while (lq_n > 0 && (lq[lq_n - 1] & 0xC0) == 0x80) lq_n--;     /* хвост символа UTF-8 */
        if (lq_n > 0) lq_n--;
        lq[lq_n] = 0; l_sel = 0; launcher_rebuild();
    } else if (ch >= 32 && ch != 127) {
        int need = ch < 0x80 ? 1 : 2;
        if (lq_n + need < LQ_MAX) {
            if (need == 1) lq[lq_n++] = (char)ch;
            else { lq[lq_n++] = (char)(0xC0 | (ch >> 6)); lq[lq_n++] = (char)(0x80 | (ch & 0x3F)); }
            lq[lq_n] = 0; l_sel = 0; launcher_rebuild();
        }
    }
    launcher_damage();
}

/* Первая показанная строка так, чтобы выбранный пункт был виден. */
static int launcher_top(int vis) {
    int total = l_nitems ? l_nrows : l_nm, selrow = l_sel;
    if (l_nitems && l_nm) { for (int r = 0; r < l_nrows; r++) if (lrow[r] == l_match[l_sel]) { selrow = r; break; } }
    int top = selrow - vis + 1; if (top < 0) top = 0;
    if (l_nitems && !lq_n && top > 0 && selrow >= vis && lrow[top] >= 0 && lrow[top - 1] < 0) top--;   /* заголовок группы остаётся над первым пунктом */
    if (top > total - vis) top = total - vis;
    return top < 0 ? 0 : top;
}

/* Щелчок при открытом меню: пункт запускает, мимо панели - закрывает. Всегда поглощает щелчок. */
static void launcher_click(int px, int py) {
    int x, y, w, h; launcher_geom(&x, &y, &w, &h);
    if (px >= x && px < x + w && py >= y + 48 && py < y + h) {
        int vis = launcher_vis_rows(), row = launcher_top(vis) + (py - y - 48) / LROW_H;
        if (l_nitems) {
            if (row >= 0 && row < l_nrows && lrow[row] >= 0) for (int k = 0; k < l_nm; k++) if (l_match[k] == lrow[row]) { launcher_run(k); return; }
        } else if (row >= 0 && row < l_nm) { launcher_run(row); return; }
        return;
    }
    if (px >= x && px < x + w && py >= y && py < y + h) return;
    launcher_damage(); launcher_open = 0; launcher_damage();
}

static void draw_launcher(surface_t* c, int ox, int oy) {
    int px, py, pw, ph; launcher_geom(&px, &py, &pw, &ph);
    if (pw < 200) return;
    ui_round_rect(c, px + 3 + ox, py + 5 + oy, pw, ph, TH_RADIUS, 0x00000000, 110);           /* тень */
    ui_round_rect(c, px + ox, py + oy, pw, ph, TH_RADIUS, TH_SURF_WIN, 255);
    ui_round_frame(c, px + ox, py + oy, pw, ph, TH_RADIUS, TH_EDGE, 255);
    ui_input(c, px + 8 + ox, py + 8 + oy, pw - 16, 32, lq_n ? lq : "", 1);
    if (!lq_n) gfx_text(c, px + 16 + ox, py + 8 + (32 - 16) / 2 + oy, "Найти программу...", TH_FG_FAINT, -1);
    if (!l_nm) { gfx_text(c, px + 16 + ox, py + 56 + oy, "Ничего не найдено", TH_FG_DIM, -1); return; }
    int vis = launcher_vis_rows(), top = launcher_top(vis), nrows = l_nitems ? l_nrows : l_nm;
    for (int r = 0; r < vis && top + r < nrows; r++) {
        int ry = py + 48 + r * LROW_H, row = top + r;
        if (l_nitems && lrow[row] < 0) {                                                 /* заголовок группы */
            gfx_text(c, px + 16 + ox, ry + 5 + oy, lgroup[-lrow[row] - 1], TH_FG_FAINT, -1);
            gfx_rect(c, px + 16 + gfx_text_width(lgroup[-lrow[row] - 1]) + 8 + ox, ry + LROW_H / 2 + oy, pw - 32 - gfx_text_width(lgroup[-lrow[row] - 1]) - 8, 1, TH_EDGE);
            continue;
        }
        int item = l_nitems ? lrow[row] : l_match[row];
        int sel = l_nitems ? (l_match[l_sel] == item) : (row == l_sel);
        const char* label = l_nitems ? litem[item].label : side[item].label;
        const char* tag = l_nitems ? litem[item].tag : side[item].tag;
        const char* desc = l_nitems ? litem[item].desc : side[item].desc;
        int icon = l_nitems ? litem[item].icon : side[item].icon;
        if (sel) { gfx_rect(c, px + 4 + ox, ry + oy, pw - 8, LROW_H, ui_state_bg(UI_ST_SELECTED, TH_SURF_WIN)); gfx_rect(c, px + 4 + ox, ry + oy, 3, LROW_H, TH_ACCENT); }
        icon_draw(c, px + 26 + ox, ry + LROW_H / 2 + oy, 16, icon, sel ? TH_FG : TH_FG_DIM, 0);
        ui_text_fit(c, px + 44 + ox, ry + 5 + oy, label, pw / 3, sel ? TH_FG : TH_FG_DIM);
        if (desc) ui_text_fit(c, px + 44 + pw / 3 + 10 + ox, ry + 5 + oy, desc, pw - pw / 3 - 90, TH_FG_FAINT);
        if (tag && tag[0] && wm_find(tag) >= 0) gfx_text(c, px + pw - 28 + ox, ry + 5 + oy, "\xE2\x97\x8F", TH_OK, -1);   /* запущено */
    }
}

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
            else if ((wn->full || !wp_rgb) &&                       /* рамка окна полупрозрачна (стекло над живым фоном): под ней нужно рисовать нижние слои */
                     x0 >= wn->x + PANEL_R && y0 >= wn->y + PANEL_R &&
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
            if (fs < 0) {
                if (!wp_rgb && bg_fancy) brainfx_draw(&clip, ox, oy, anim_ms);
                if (home_open) draw_home(&clip, ox, oy);
            }
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
        draw_side(&clip, ox, oy);
        if (notice_count) draw_notifications(&clip, ox, oy);
        if (launcher_open) draw_launcher(&clip, ox, oy);
    } else draw_pill(&clip, ox, oy, fs, pill);
    draw_cursor(&clip, ox, oy);
}

static void tiles_clear(void) {
    int tw = (scr->w + DTILE - 1) / DTILE, th = (scr->h + DTILE - 1) / DTILE;
    if (tw > TMAX_X) tw = TMAX_X;
    if (th > TMAX_Y) th = TMAX_Y;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) tdirty[y][x] = (tbox_t){ 0, 0, 0, 0 };
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
    last_dmg[0] = dx0; last_dmg[1] = dy0; last_dmg[2] = dx1; last_dmg[3] = dy1;
    dmg_valid = 0;
    int tw = (scr->w + DTILE - 1) / DTILE, th = (scr->h + DTILE - 1) / DTILE;
    if (tw > TMAX_X) tw = TMAX_X;
    if (th > TMAX_Y) th = TMAX_Y;
    njobs = 0;
    uint64_t px = 0;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; ) {
            if (tdirty[ty][tx].x1 <= tdirty[ty][tx].x0) { tx++; continue; }
            job_t j = { tdirty[ty][tx].x0, tdirty[ty][tx].y0, tdirty[ty][tx].x1, tdirty[ty][tx].y1 };
            while (tx < tw && tdirty[ty][tx].x1 > tdirty[ty][tx].x0) {            /* полоса подряд идущих грязных плиток: рамка - объединение их рамок */
                const tbox_t* b = &tdirty[ty][tx];
                if (b->x0 < j.x0) j.x0 = b->x0;
                if (b->y0 < j.y0) j.y0 = b->y0;
                if (b->x1 > j.x1) j.x1 = b->x1;
                if (b->y1 > j.y1) j.y1 = b->y1;
                tdirty[ty][tx++] = (tbox_t){ 0, 0, 0, 0 };
            }
            if (j.x1 > scr->w) j.x1 = (int16_t)scr->w;
            if (j.y1 > scr->h) j.y1 = (int16_t)scr->h;
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
