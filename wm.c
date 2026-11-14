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
#define WM_SPATIAL_DOCK_H 76
#define WM_SPATIAL_TAB_H 40
#define WM_SPATIAL_TAB_GAP 8
#define PILL_ZONE 56               /* в полноэкранном режиме панель кнопок показывается, когда курсор выше этой черты */
#define LIGHT_R 6                  /* «светофор» окна */

typedef struct {
    int used, visible, closable, pinned, user, full, input;
    int workspace;
    uint32_t generation;
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
static wm_layout_mode_t layout_mode = WM_LAYOUT_LEGACY;
static uint32_t win_generation;
static int overview_open, overview_page, overview_n, overview_release_guard;
static int system_panel_open;
static int dock_page;
static int current_workspace;
#define WM_NOTIFICATION_MAX 8
typedef struct { char title[40]; char message[112]; uint32_t created_ms; uint8_t priority, unread; } wm_notification_t;
static wm_notification_t notifications[WM_NOTIFICATION_MAX];
static unsigned notification_head, notification_count;
static int notification_center_open, notification_toast_active;
static int modal_release_guard;
static uint32_t notification_toast_until;
static void str_copy(char* d, const char* s, int cap);
static int overview_ids[WM_MAX_WIN];
static uint32_t overview_generations[WM_MAX_WIN];
static void overview_click(int px, int py);
static int window_on_workspace(const win_t* w) { return w->pinned || w->workspace == current_workspace; }
static void workspace_changed(void);
static void draw_system_panel(surface_t* c, int ox, int oy);
static void damage_system_panel(void);
static void overview_damage_window(int id);
static uint32_t* bg;
static surface_t bgs;
static char clock_date[48], clock_hm[8];
static uint8_t m_busy[64];
static int m_cores, m_mem = -1, m_health = -1;
static int m_network = -1, m_audio_volume = -1;
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

#define WM_SYSTEM_PANEL_W 288
#define WM_SYSTEM_PANEL_H 204
static void system_panel_rect(int* x, int* y, int* w, int* h) {
    *w = scr->w - 24; if (*w > WM_SYSTEM_PANEL_W) *w = WM_SYSTEM_PANEL_W;
    if (*w < 160) *w = scr->w - 8;
    /* Reserve room below the floating status capsule even on 320x240 screens. */
    *h = scr->h - WM_TOP_H - 16;
    if (*h > WM_SYSTEM_PANEL_H) *h = WM_SYSTEM_PANEL_H;
    if (*h < 112) *h = scr->h - WM_TOP_H - 8;
    *x = scr->w - *w - 12; *y = WM_TOP_H + 8;
}
static void damage_system_panel(void) {
    int x, y, w, h; system_panel_rect(&x, &y, &w, &h);
    damage(x, y, x + w, y + h); damage(scr->w - 48, 0, scr->w, WM_TOP_H);
}
int wm_system_panel_is_open(void) { return system_panel_open; }

static void notification_toast_rect(int* x, int* y, int* w, int* h) {
    *w = scr->w - 24; if (*w > 320) *w = 320;
    if (*w < 160) *w = scr->w - 8;
    *h = 72; *x = scr->w - *w - 12; *y = WM_TOP_H + 8;
}
static void notification_center_rect(int* x, int* y, int* w, int* h) {
    *w = scr->w - 24; if (*w > 360) *w = 360;
    if (*w < 160) *w = scr->w - 8;
    *h = scr->h - WM_TOP_H - 24; if (*h > 360) *h = 360;
    if (*h < 100) *h = scr->h - WM_TOP_H - 8;
    *x = scr->w - *w - 12; *y = WM_TOP_H + 8;
}
static int notification_unread(void) {
    int n = 0;
    for (unsigned i = 0; i < notification_count; i++) {
        unsigned at = (notification_head + WM_NOTIFICATION_MAX - 1 - i) % WM_NOTIFICATION_MAX;
        if (notifications[at].unread) n++;
    }
    return n;
}
int wm_notification_count(void) { return (int)notification_count; }
int wm_notification_unread_count(void) { return notification_unread(); }
int wm_notification_center_is_open(void) { return notification_center_open; }
int wm_notification_toast_is_visible(void) { return notification_toast_active; }
int wm_notification_post(const char* title, const char* message, int priority) {
    if (!title || !message || layout_mode != WM_LAYOUT_SPATIAL_MINIMAL) return -1;
    unsigned at = notification_head;
    wm_notification_t* n = &notifications[at];
    str_copy(n->title, title, sizeof n->title);
    str_copy(n->message, message, sizeof n->message);
    n->priority = (uint8_t)(priority < WM_NOTIFY_INFO ? WM_NOTIFY_INFO : priority > WM_NOTIFY_CRITICAL ? WM_NOTIFY_CRITICAL : priority);
    n->unread = 1; n->created_ms = anim_ms;
    notification_head = (notification_head + 1) % WM_NOTIFICATION_MAX;
    if (notification_count < WM_NOTIFICATION_MAX) notification_count++;
    notification_toast_active = !system_panel_open;
    notification_toast_until = anim_ms + 5000u;
    int x, y, w, h;
    notification_toast_rect(&x, &y, &w, &h);
    damage(x, y, x + w, y + h);
    damage_top();
    if (notification_center_open) {
        notification_toast_active = 0;
        for (unsigned i = 0; i < notification_count; i++) {
            unsigned ix = (notification_head + WM_NOTIFICATION_MAX - 1 - i) % WM_NOTIFICATION_MAX;
            notifications[ix].unread = 0;
        }
        damage(0, 0, scr->w, scr->h);
    }
    return 0;
}
void wm_notification_center_toggle(void) {
    if (layout_mode != WM_LAYOUT_SPATIAL_MINIMAL) return;
    notification_center_open = !notification_center_open;
    if (notification_center_open) {
        overview_open = 0; system_panel_open = 0; notification_toast_active = 0;
        for (unsigned i = 0; i < notification_count; i++) {
            unsigned at = (notification_head + WM_NOTIFICATION_MAX - 1 - i) % WM_NOTIFICATION_MAX;
            notifications[at].unread = 0;
        }
    }
    capture = -1; dragging = -1; resizing = -1;
    damage(0, 0, scr->w, scr->h);
}
void wm_notification_clear(void) {
    notification_count = 0; notification_head = 0;
    notification_center_open = 0; notification_toast_active = 0;
    damage(0, 0, scr->w, scr->h);
}


static int fs_top(void) {
    for (int i = zcount - 1; i >= 0; i--) {
        const win_t* w = &win[zorder[i]];
        if (w->visible && window_on_workspace(w) && !w->pinned) return w->full ? zorder[i] : -1;
    }
    return -1;
}

/* --- боковая панель --- */

static int side_panel_h(void) { return scr->h - WM_TOP_H - 20; }

/* Spacious desktop rail on normal displays; icon-only rail on narrow Limbo/mobile modes. */
static int spatial_rail_width(void) {
    if (scr->w >= 760) return WM_SPATIAL_RAIL_W;
    if (scr->w >= 480) return 168;
    return 64;
}
static int spatial_nav_top(void) {
    return scr->h < 360 ? WM_TOP_H + 12 : WM_TOP_H + 36;
}

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

#define WM_DOCK_TOUCH_TILE 40
#define WM_DOCK_TOUCH_GAP 8
#define WM_DOCK_CONTROL_W 44

static int spatial_dock_count(void) {
    int n = 0;
    for (int i = 0; i < side_n; i++) if (!side[i].temp && !side[i].bottom) n++;
    return n;
}

static int spatial_dock_slot(int item) {
    int slot = 0;
    for (int i = 0; i < item; i++) if (!side[i].temp && !side[i].bottom) slot++;
    return slot;
}

/* On compact displays keep 40px touch tiles and page the pins instead of shrinking them. */
static int spatial_dock_page_size(void) {
    int n = spatial_dock_count();
    int available = scr->w - 48;
    if (n <= 0) return 0;
    if (n * TILE + (n - 1) * TILE_GAP <= available - 24) return n;
    int inner = available - 24 - 2 * WM_DOCK_CONTROL_W;
    int fit = (inner + WM_DOCK_TOUCH_GAP) / (WM_DOCK_TOUCH_TILE + WM_DOCK_TOUCH_GAP);
    if (fit < 1) fit = 1;
    if (fit > n) fit = n;
    return fit;
}

static int spatial_dock_pages(void) {
    int n = spatial_dock_count(), page_size = spatial_dock_page_size();
    return page_size > 0 ? (n + page_size - 1) / page_size : 0;
}

static int spatial_dock_page_first(void) {
    int pages = spatial_dock_pages(), page = dock_page;
    if (page < 0) page = 0;
    if (pages > 0 && page >= pages) page = pages - 1;
    return page * spatial_dock_page_size();
}

static int spatial_dock_visible_count(void) {
    int n = spatial_dock_count(), first = spatial_dock_page_first();
    int remaining = n - first, page_size = spatial_dock_page_size();
    return remaining < page_size ? remaining : page_size;
}

static int spatial_dock_tile(void) {
    return spatial_dock_pages() > 1 ? WM_DOCK_TOUCH_TILE : TILE;
}

static int spatial_dock_gap(void) {
    return spatial_dock_pages() > 1 ? WM_DOCK_TOUCH_GAP : TILE_GAP;
}

static int spatial_dock_width(void) {
    int count = spatial_dock_visible_count(), tile = spatial_dock_tile(), gap = spatial_dock_gap();
    int controls = spatial_dock_pages() > 1 ? 2 * WM_DOCK_CONTROL_W : 0;
    return count ? count * tile + (count - 1) * gap + controls + 24 : 0;
}

static int spatial_dock_x(int item) {
    int slot = spatial_dock_slot(item), first = spatial_dock_page_first();
    int count = spatial_dock_visible_count(), tile = spatial_dock_tile(), gap = spatial_dock_gap();
    if (slot < first || slot >= first + count) return -10000;
    int controls = spatial_dock_pages() > 1 ? WM_DOCK_CONTROL_W : 0;
    int left = (scr->w - spatial_dock_width()) / 2 + 12 + controls;
    return left + (slot - first) * (tile + gap);
}

static int spatial_dock_y(void) { return scr->h - spatial_dock_tile() - 20; }

static int spatial_dock_control_center(int next, int* x, int* y) {
    if (spatial_dock_pages() <= 1) return -1;
    int width = spatial_dock_width(), left = (scr->w - width) / 2;
    *x = next ? left + width - WM_DOCK_CONTROL_W / 2 - 2 : left + WM_DOCK_CONTROL_W / 2 + 2;
    *y = spatial_dock_y() + spatial_dock_tile() / 2;
    return 0;
}

int wm_dock_page_control_center(int next, int* x, int* y) {
    if (!x || !y || (next != 0 && next != 1)) return -1;
    if (spatial_dock_control_center(next, x, y) != 0) return -1;
    if (next && dock_page + 1 >= spatial_dock_pages()) return -1;
    if (!next && dock_page <= 0) return -1;
    return 0;
}

static int spatial_dock_control_hit(int px, int py) {
    int x, y;
    if (spatial_dock_pages() <= 1) return 0;
    if (spatial_dock_control_center(0, &x, &y) == 0 && px >= x - 22 && px < x + 22 && py >= y - 22 && py < y + 22) return -1;
    if (spatial_dock_control_center(1, &x, &y) == 0 && px >= x - 22 && px < x + 22 && py >= y - 22 && py < y + 22) return 1;
    return 0;
}

/* Keep the complete navigation rail above the Dock on short Limbo/mobile modes. */
static int spatial_tab_height(void) {
    if (scr->h < 320) return 14;
    if (scr->h < 400) return 24;
    if (scr->h < 520) return 36;
    return 42;
}

static int spatial_tab_step(void) {
    int height = spatial_tab_height();
    int step = height + (spatial_rail_width() >= 160 ? 8 : 2);
    if (tab_n > 1) {
        int available = spatial_dock_y() - 16 - spatial_nav_top() - height;
        int fit = available / (tab_n - 1);
        if (fit < step) step = fit;
    }
    if (step < height) step = height;
    return step;
}

static int spatial_tab_y(int i) { return spatial_nav_top() + i * spatial_tab_step(); }

static void damage_spatial_tab(int i) {
    if (i < 0 || i >= tab_n) return;
    int y = spatial_tab_y(i);
    int rail_w = spatial_rail_width();
    damage(0, y, rail_w, y + spatial_tab_height());
    if (rail_w < 160) {
        int label_w = gfx_text_width(tabs[i].label) + 20;
        int label_x = rail_w + 8;
        damage(label_x, y + 6, label_x + label_w, y + 34);
    }
}

static void damage_side(void) {
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL)
        damage(0, spatial_dock_y() - 40, scr->w, scr->h);
    else
        damage(0, WM_TOP_H, WM_SIDE_W + 300, scr->h);
}

static int side_hit(int px, int py) {
    for (int i = 0; i < side_n; i++) {
        if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && (side[i].temp || side[i].bottom || spatial_dock_x(i) < 0)) continue;
        int tile = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? spatial_dock_tile() : TILE;
        int x = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? spatial_dock_x(i) : SIDE_X + 6;
        int y = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? spatial_dock_y() : side_item_y(i);
        if (px >= x && px < x + tile && py >= y && py < y + tile) return i;
    }
    return -1;
}

int wm_side_center(int i, int* x, int* y) {
    if (i < 0 || i >= side_n) return -1;
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        if (side[i].temp || side[i].bottom) return -1;
        int dx = spatial_dock_x(i);
        if (dx < 0) return -1;
        int tile = spatial_dock_tile();
        *x = dx + tile / 2; *y = spatial_dock_y() + tile / 2;
    } else { *x = SIDE_X + 6 + TILE / 2; *y = side_item_y(i) + TILE / 2; }
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
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        if (px < 8 || px >= spatial_rail_width() - 8) return -1;
        for (int i = 0; i < tab_n; i++) {
            int y = spatial_tab_y(i);
            if (py >= y && py < y + spatial_tab_height()) return i;
        }
        return -1;
    }
    if (py < 7 || py >= 7 + TAB_H) return -1;
    for (int i = 0; i < tab_n; i++) if (px >= tab_x(i) && px < tab_x(i) + tab_w(i)) return i;
    return -1;
}

int wm_tab_center(int i, int* x, int* y) {
    if (i < 0 || i >= tab_n) return -1;
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        *x = spatial_rail_width() / 2; *y = spatial_tab_y(i) + spatial_tab_height() / 2;
    } else { *x = tab_x(i) + tab_w(i) / 2; *y = 7 + TAB_H / 2; }
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

void wm_init_mode(surface_t* screen, const wm_ops_t* o, wm_layout_mode_t mode) {
    layout_mode = (mode == WM_LAYOUT_SPATIAL_MINIMAL) ? WM_LAYOUT_SPATIAL_MINIMAL : WM_LAYOUT_LEGACY;
    overview_open = 0; overview_page = 0; overview_n = 0; overview_release_guard = 0;
    system_panel_open = 0; dock_page = 0; current_workspace = 0;
    notification_head = notification_count = 0; notification_center_open = notification_toast_active = 0; notification_toast_until = 0; modal_release_guard = 0;
    win_generation = 0;
    scr = screen;
    ops = *o;
    for (int i = 0; i < WM_MAX_WIN; i++) win[i].used = 0;
    zcount = 0; dragging = -1; resizing = -1; capture = -1; prev_buttons = 0;
    side_n = 0; side_hover = -1; tab_n = 0; tab_hover = -1; light_hover = -1; last_tab = -2;
    last_fs = -1; last_pill = 0; cascade = 0; clock_date[0] = 0; clock_hm[0] = 0; m_cores = 0; m_mem = -1; m_health = -1; m_network = -1; m_audio_volume = -1;
    cx = scr->w / 2; cy = scr->h / 2;
    dmg_valid = 0;
    bg = 0; bgs.px = 0; bgs.w = bgs.h = bgs.pitch = 0;
    if (layout_mode == WM_LAYOUT_LEGACY) bg_render();
    damage(0, 0, scr->w, scr->h);
}

void wm_init(surface_t* screen, const wm_ops_t* o) {
    wm_init_mode(screen, o, WM_LAYOUT_LEGACY);
}

static void auto_place(win_t* w) {
    int ow = outer_w(w), oh = outer_h(w);
    int left = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? spatial_rail_width() + 8 : (scr->w >= 800 ? WM_SIDE_W + 8 : 8);
    int ax = left, aw = scr->w - ax - 12;
    int ay = WM_TOP_H + 12, ah = scr->h - ay - (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? WM_SPATIAL_DOCK_H : 12);
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
    w->workspace = (flags & WM_PINNED) ? -1 : current_workspace;
    w->generation = ++win_generation;
    if (!w->generation) w->generation = ++win_generation;
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
    overview_damage_window(id);
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
    if (dock_page >= spatial_dock_pages()) dock_page = spatial_dock_pages() > 0 ? spatial_dock_pages() - 1 : 0;
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
    for (int i = zcount - 1; i >= 0; i--)
        if (win[zorder[i]].visible && !win[zorder[i]].pinned && window_on_workspace(&win[zorder[i]])) return zorder[i];
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
    if (win[id].workspace != current_workspace) wm_workspace_switch(win[id].workspace);
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
    if (win[id].visible == v) { if (v && !win[id].pinned) wm_raise(id); return; }
    win[id].visible = v;
    if (v) wm_raise(id);
    damage_win(&win[id]);
    damage_side();
    damage_top();
    overview_damage_window(id);
}

int wm_visible(int id) { return id >= 0 && id < WM_MAX_WIN && win[id].used && win[id].visible && window_on_workspace(&win[id]); }

void wm_minimize(int id) {
    if (!wm_visible(id) || win[id].pinned) return;
    if (win[id].full) wm_set_fullscreen(id, 0);
    wm_set_visible(id, 0);
    if (ops.notify) ops.notify(id, 0);
}

void wm_minimize_all(void) {
    for (int i = 0; i < WM_MAX_WIN; i++) if (wm_visible(i) && !win[i].pinned) wm_minimize(i);
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
    if (on) { system_panel_open = 0; notification_center_open = 0; notification_toast_active = 0; }
    damage_win(&win[id]);
    win[id].full = on;
    if (win[id].resizable) {
        if (on) {
            win[id].norm_w = win[id].content.w; win[id].norm_h = win[id].content.h;
            int nw = scr->w < win[id].cap_w ? scr->w : win[id].cap_w, nh = scr->h < win[id].cap_h ? scr->h : win[id].cap_h;
            owner_resize(id, nw / win[id].step_w * win[id].step_w, nh / win[id].step_h * win[id].step_h);
        } else owner_resize(id, win[id].norm_w, win[id].norm_h);
    }
    if (wm_visible(id)) wm_raise(id);
    damage_win(&win[id]);
    damage(0, 0, scr->w, scr->h);
}

int wm_fullscreen(int id) { return id >= 0 && id < WM_MAX_WIN && win[id].used && win[id].full; }

void wm_invalidate(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    if (overview_open) { overview_damage_window(id); return; }
    if (!wm_visible(id)) return;
    win_t* w = &win[id];
    int ox, oy, dw, dh; geom(w, &ox, &oy, &dw, &dh);
    damage(ox, oy, ox + dw, oy + dh);
}

void wm_invalidate_rows(int id, int y0, int y1) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    if (overview_open) { overview_damage_window(id); return; }
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
    if (layout_mode == WM_LAYOUT_LEGACY) damage_top();
    else if (system_panel_open) damage_system_panel();
}

void wm_set_system_io(int network_link, int audio_volume) {
    if (network_link < -2) network_link = -1;
    if (network_link > 1) network_link = 1;
    if (audio_volume < -1) audio_volume = -1;
    if (audio_volume > 100) audio_volume = 100;
    if (m_network == network_link && m_audio_volume == audio_volume) return;
    m_network = network_link;
    m_audio_volume = audio_volume;
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && system_panel_open) damage_system_panel();
}

void wm_set_layout_mode(wm_layout_mode_t mode) {
    if (mode != WM_LAYOUT_LEGACY && mode != WM_LAYOUT_SPATIAL_MINIMAL) return;
    if (layout_mode == mode) return;
    layout_mode = mode;
    system_panel_open = 0; notification_center_open = 0; notification_toast_active = 0;
    if (mode == WM_LAYOUT_LEGACY && !bg) bg_render();
    side_hover = -1; tab_hover = -1; last_tab = -2;
    if (scr) damage(0, 0, scr->w, scr->h);
}

wm_layout_mode_t wm_get_layout_mode(void) { return layout_mode; }

int wm_workspace_current(void) { return current_workspace; }
int wm_workspace_count(void) { return WM_WORKSPACE_MAX; }

int wm_workspace_center(int workspace, int* x, int* y) {
    if (workspace < 0 || workspace >= WM_WORKSPACE_MAX || !x || !y || layout_mode != WM_LAYOUT_SPATIAL_MINIMAL) return -1;
    *x = scr->w >= 640 ? scr->w - 165 : scr->w - 82; *y = WM_TOP_H / 2;
    return 0;
}

int wm_workspace_switch(int workspace) {
    if (workspace < 0 || workspace >= WM_WORKSPACE_MAX) return -1;
    if (workspace == current_workspace) return 0;
    current_workspace = workspace;
    overview_open = 0; overview_page = 0;
    system_panel_open = 0; notification_center_open = 0; notification_toast_active = 0;
    capture = -1; dragging = -1; resizing = -1;
    overview_release_guard = 0;
    damage(0, 0, scr->w, scr->h);
    workspace_changed();
    return 1;
}

static void workspace_changed(void) {
    if (ops.workspace_changed) ops.workspace_changed(current_workspace);
}

int wm_window_workspace(int id) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used || win[id].pinned) return -1;
    return win[id].workspace;
}

int wm_window_move_workspace(int id, int workspace) {
    if (id < 0 || id >= WM_MAX_WIN || !win[id].used || win[id].pinned ||
        workspace < 0 || workspace >= WM_WORKSPACE_MAX) return -1;
    if (win[id].workspace == workspace) return 0;
    int active = wm_visible(id);
    if (win[id].full) wm_set_fullscreen(id, 0);
    if (active) damage_win(&win[id]);
    win[id].workspace = workspace;
    overview_open = 0; system_panel_open = 0; notification_center_open = 0; notification_toast_active = 0;
    damage(0, 0, scr->w, scr->h);
    if (active) workspace_changed();
    return 1;
}

static int overview_refresh(void) {
    int ids[WM_MAX_WIN], n = 0;
    uint32_t generations[WM_MAX_WIN];
    for (int i = zcount - 1; i >= 0; i--) {
        int id = zorder[i];
        if (id >= 0 && id < WM_MAX_WIN && win[id].used && !win[id].pinned && win[id].workspace == current_workspace) {
            ids[n] = id;
            generations[n] = win[id].generation;
            n++;
        }
    }
    int changed = n != overview_n;
    for (int i = 0; i < n && !changed; i++)
        if (ids[i] != overview_ids[i] || generations[i] != overview_generations[i]) changed = 1;
    if (changed) {
        for (int i = 0; i < n; i++) {
            overview_ids[i] = ids[i];
            overview_generations[i] = generations[i];
        }
        overview_n = n;
        int cols = scr->w >= 900 ? 3 : scr->w >= 600 ? 2 : 1;
        int rows = scr->h >= 560 ? 2 : 1;
        int per_page = cols * rows;
        int pages = n ? (n + per_page - 1) / per_page : 1;
        if (overview_page >= pages) overview_page = pages - 1;
        if (overview_page < 0) overview_page = 0;
    }
    return changed;
}

void wm_window_overview_toggle(void) {
    system_panel_open = 0; notification_center_open = 0; notification_toast_active = 0;
    if (overview_open) overview_open = 0;
    else { overview_page = 0; overview_refresh(); overview_open = 1; }
    capture = -1; dragging = -1; resizing = -1;
    damage(0, 0, scr->w, scr->h);
}

int wm_window_overview_is_open(void) { return overview_open; }

int wm_window_overview_count(void) {
    overview_refresh();
    return overview_n;
}


void wm_set_fx(int level) { fx_level = level < 0 ? 0 : level > 3 ? 3 : level; }

static void anim_damage(int x0, int y0, int x1, int y1) { damage(x0, y0, x1, y1); }

void wm_anim(uint32_t now_ms, unsigned activity) {
    anim_ms = now_ms;
    if (notification_toast_active && (int32_t)(now_ms - notification_toast_until) >= 0) {
        int x, y, w, h; notification_toast_rect(&x, &y, &w, &h);
        notification_toast_active = 0; damage(x, y, x + w, y + h);
    }
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL || fs_top() >= 0) return;                        /* во весь экран фон не виден */
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
    overview_damage_window(id);
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
            if (!w->visible || !window_on_workspace(w) || w->pinned != pass) continue;
            if (hit_rect(w, px, py)) return zorder[i];
        }
    return -1;
}

/* «Светофор»: 1 - закрыть (красный), 2 - свернуть (жёлтый), 3 - на весь экран (зелёный). */
static int light_x(const win_t* w, int which) {
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) return w->x + 22 + (which - 1) * 36;
    return w->x + 18 + (which - 1) * 20;
}
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
        if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
            if (dx >= -16 && dx < 16 && dy >= -16 && dy < 16) return k;
        } else if (dx * dx + dy * dy <= 9 * 9) return k;
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
    if (wm_visible(id) && wm_top() == id) { wm_minimize(id); return; }
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
    int target = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? 36 : 22;
    return px >= w->x + outer_w(w) - target && py >= w->y + outer_h(w) - target;
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
    if (overview_open) { overview_click(px, py); return; }
    if (notification_center_open) {
        int nx, ny, nw, nh; notification_center_rect(&nx, &ny, &nw, &nh);
        if (px >= nx && px < nx + nw && py >= ny && py < ny + nh) {
            if (px >= nx + nw - 100 && py >= ny + 8 && py < ny + 42) wm_notification_clear();
            return;
        }
        notification_center_open = 0; damage(0, 0, scr->w, scr->h);
        return; /* dismiss without activating the covered window */
    }
    if (notification_toast_active) {
        int tx, ty, tw, th; notification_toast_rect(&tx, &ty, &tw, &th);
        if (px >= tx && px < tx + tw && py >= ty && py < ty + th) {
            notification_toast_active = 0; damage(tx, ty, tx + tw, ty + th); return;
        }
    }
    int fs = fs_top();
    if (fs >= 0) {
        if (pill_visible(fs) && click_pill(fs, px, py)) return;
        if (gets_input(&win[fs])) capture = fs;
        return;
    }
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && system_panel_open) {
        int sx, sy, sw, sh; system_panel_rect(&sx, &sy, &sw, &sh);
        if (px >= sx && px < sx + sw && py >= sy + 48 && py < sy + 72) {
            wm_notification_center_toggle(); return;
        }
        if (m_audio_volume >= 0 && ops.system_volume &&
            px >= sx + 16 && px < sx + sw - 16 && py >= sy + 134 && py < sy + 162) {
            int pct = (px - (sx + 24)) * 100 / (sw - 48);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            ops.system_volume(pct);
            return;
        }
        if (px >= sx && px < sx + sw && py >= sy && py < sy + sh) return;
        system_panel_open = 0; damage_system_panel();
        return; /* dismissing the overlay must not activate a covered window */
    }
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && py < WM_TOP_H) {
        if (scr->w >= 640 && px >= scr->w - 262 && px < scr->w - 194 && py >= 10 && py < 38) {
            wm_window_overview_toggle(); return;
        }
        if (scr->w >= 640 && px >= scr->w - 192 && px < scr->w - 130 && py >= 10 && py < 38) {
            wm_workspace_switch((current_workspace + 1) % WM_WORKSPACE_MAX); return;
        }
        if (scr->w < 640 && px >= scr->w - 102 && px < scr->w - 60 && py >= 10 && py < 38) {
            wm_workspace_switch((current_workspace + 1) % WM_WORKSPACE_MAX); return;
        }
        if (px >= scr->w - 52 && px < scr->w - 12 && py >= 10 && py < 38) {
            system_panel_open = !system_panel_open;
            if (system_panel_open && notification_toast_active) {
                int tx, ty, tw, th; notification_toast_rect(&tx, &ty, &tw, &th);
                notification_toast_active = 0; damage(tx, ty, tx + tw, ty + th);
            }
            damage_system_panel(); return;
        }
        return;
    }
    if (layout_mode == WM_LAYOUT_LEGACY && py < WM_TOP_H) {
        int t = tab_hit(px, py);
        if (t >= 0 && ops.tab) ops.tab(t);
        return;
    }
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && py < WM_TOP_H) return;
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL && tab_hit(px, py) >= 0) {
        int t = tab_hit(px, py);
        if (t >= 0 && ops.tab) ops.tab(t);
        return;
    }
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        int page_control = spatial_dock_control_hit(px, py);
        if (page_control) {
            int pages = spatial_dock_pages();
            if (page_control < 0 && dock_page > 0) dock_page--;
            if (page_control > 0 && dock_page + 1 < pages) dock_page++;
            side_hover = -1;
            damage_side();
            return;
        }
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
    if (th != tab_hover) {
        int old_tab = tab_hover;
        tab_hover = th;
        if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
            damage_spatial_tab(old_tab);
            damage_spatial_tab(th);
        } else damage_top();
    }
    int lh = -1;
    int controls_w = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? 116 : 72;
    if (fs < 0 && cy >= WM_TOP_H) {
        int id = hit_window(cx, cy);
        if (id >= 0 && !win[id].pinned && !win[id].full &&
            cy < win[id].y + WM_HEAD && cx >= win[id].x && cx < win[id].x + controls_w) lh = id;
    }
    if (lh != light_hover) {
        if (light_hover >= 0 && win[light_hover].used)
            damage(win[light_hover].x, win[light_hover].y, win[light_hover].x + controls_w, win[light_hover].y + WM_HEAD);
        light_hover = lh;
        if (lh >= 0) damage(win[lh].x, win[lh].y, win[lh].x + controls_w, win[lh].y + WM_HEAD);
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
    if (overview_release_guard) {
        overview_release_guard = left;
        capture = -1; dragging = -1; resizing = -1;
        prev_buttons = buttons;
        return; /* consume the release belonging to the overlay click */
    }
    if (modal_release_guard) {
        modal_release_guard = left;
        capture = -1; dragging = -1; resizing = -1;
        prev_buttons = buttons;
        return; /* consume release after a modal/toast click, even if it closed the surface */
    }
    if (overview_open) {
        if (left && !was) {
            click(cx, cy);
            if (!overview_open) overview_release_guard = 1;
        }
        capture = -1; dragging = -1; resizing = -1;
        prev_buttons = buttons;
        return; /* overlay owns input; never leak clicks or pointer events to an underlying app */
    }
    if (notification_center_open && layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        if (left && !was) { click(cx, cy); modal_release_guard = 1; }
        capture = -1; dragging = -1; resizing = -1;
        prev_buttons = buttons;
        return; /* history is modal to the pointer, but never changes keyboard focus */
    }
    if (notification_toast_active && left && !was) {
        int tx, ty, tw, th; notification_toast_rect(&tx, &ty, &tw, &th);
        if (cx >= tx && cx < tx + tw && cy >= ty && cy < ty + th) {
            notification_toast_active = 0; damage(tx, ty, tx + tw, ty + th);
            modal_release_guard = 1; prev_buttons = buttons; return; /* dismiss toast without delivering click to covered app */
        }
    }
    if (system_panel_open && layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        if (left && !was) { click(cx, cy); modal_release_guard = 1; }
        capture = -1; dragging = -1; resizing = -1;
        prev_buttons = buttons;
        return; /* status panel is modal: do not forward pointer movement or buttons underneath */
    }
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
        int min_visible_x = layout_mode == WM_LAYOUT_SPATIAL_MINIMAL ? spatial_rail_width() : 80;
        if (nx < min_visible_x - outer_w(w)) nx = min_visible_x - outer_w(w);
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
    /* Spatial glass controls: one neutral shape language, no legacy traffic-light dots. */
    for (int k = 0; k < 3; k++) {
        int lx = x + 22 + k * 36;
        int ly = y + WM_HEAD / 2;
        int bx = lx - 13, by = ly - 10;
        uint32_t fill = hover ? ui_mix(TH_TILE, TH_CYAN, 38) : TH_TILE;
        ui_round_rect(c, bx, by, 26, 20, 7, fill, hover ? 220 : 178);
        ui_round_frame(c, bx, by, 26, 20, 7, TH_BORDER, 210);
        uint32_t glyph = hover ? TH_TEXT : TH_TEXT_DIM;
        if (k == 0) {
            gfx_line(c, lx - 3, ly - 3, lx + 3, ly + 3, glyph);
            gfx_line(c, lx + 3, ly - 3, lx - 3, ly + 3, glyph);
        } else if (k == 1) {
            gfx_hline(c, lx - 4, ly + 3, 9, glyph);
        } else {
            gfx_rect(c, lx - 4, ly - 4, 8, 8, glyph);
            gfx_line(c, lx + 1, ly - 4, lx + 4, ly - 4, glyph);
            gfx_line(c, lx + 4, ly - 4, lx + 4, ly - 1, glyph);
        }
    }
}

static void draw_panel(surface_t* c, int ox, int oy, int id) {
    const win_t* w = &win[id];
    int x = w->x + ox, y = w->y + oy, ow = outer_w(w), oh = outer_h(w);
    int focused = w->pinned || id == wm_top();
    uint32_t accent = accent_of(w);

    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        /* Spatial windows are quiet work surfaces: no neon perimeter glow or
           decorative title stripe. Keep the Legacy renderer below untouched. */
        ui_round_rect(c, x, y, ow, oh, TH_RADIUS_SURFACE, TH_PANEL1, 218);
        ui_round_rect(c, x, y, ow, WM_HEAD, TH_RADIUS_SURFACE, TH_BAR0, 224);
        /* Keep the title surface translucent; avoid an opaque rectangular fill. */
        if (!w->pinned) draw_lights(c, x, y, light_hover == id);
        gfx_rect(c, x + 12, y + WM_HEAD - 1, ow - 24, 1, TH_BORDER);
        ui_round_frame(c, x, y, ow, oh, TH_RADIUS_SURFACE,
                       focused ? ui_mix(TH_BORDER, TH_CYAN, 72) : TH_BORDER, 255);

        char sub[24]; int sn = 0;
        if (w->zoomable && w->zoom16 != 16) {
            int z = w->zoom16 * 100 / 16; char dg[4]; int k = 0;
            do { dg[k++] = (char)('0' + z % 10); z /= 10; } while (z && k < 4);
            while (k) sub[sn++] = dg[--k];
            sub[sn++] = '%'; sub[sn++] = ' ';
        }
        for (int i = 0; w->sub[i] && sn < 22; i++) sub[sn++] = w->sub[i];
        sub[sn] = 0;

        int title_x = x + (w->pinned ? 22 : 116);
        int right = x + ow - 14;
        if (sn) {
            int sw = gfx_text_width(sub);
            int max_sw = ow - (w->pinned ? 66 : 154);
            if (max_sw < 24) max_sw = 24;
            if (sw > max_sw) sw = max_sw;
            int sx = x + ow - sw - 20;
            ui_round_rect(c, sx, y + 7, sw + 12, 22, TH_RADIUS_CONTROL, TH_TILE, 190);
            ui_text_fit(c, sx + 6, y + 10, sub, sw, TH_TEXT_DIM);
            right = sx - 8;
        }
        int title_w = right - title_x;
        if (title_w < 12) title_w = 12;
        ui_text_fit(c, title_x, y + 10, w->title, title_w,
                    focused ? TH_TEXT : TH_TEXT_DIM);

        blit_content(c, x + WM_PAD, y + WM_HEAD, disp_w(w), disp_h(w), w);
        if (w->resizable || w->zoomable) {
            uint32_t gc = resizing == id ? TH_CYAN : TH_TEXT_FAINT;
            int bx = x + ow - 6, by = y + oh - 6;
            for (int a = 3; a <= 9; a += 3) gfx_line(c, bx - a, by, bx, by - a, gc);
        }
        return;
    }

    if (focused && !w->pinned) ui_glow(c, x, y, ow, oh, PANEL_R, WM_GLOW, accent, 150);
    ui_round_rect(c, x, y, ow, oh, PANEL_R, TH_PANEL1, 255);
    ui_round_rect(c, x, y, ow, WM_HEAD, PANEL_R, TH_PANEL0, 255);              /* заголовок светлее */
    gfx_rect(c, x + 1, y + PANEL_R, ow - 2, WM_HEAD - PANEL_R, TH_PANEL0);
    for (int i = 0; i < ow - 2 * PANEL_R; i++) {                                 /* неоновая черта под заголовком: к краям гаснет */
        int e = i < (ow - 2 * PANEL_R) / 2 ? i : ow - 2 * PANEL_R - 1 - i;       /* расстояние до ближайшего края */
        unsigned fade = e >= 40 ? 256u : (unsigned)e * 256u / 40u;
        ui_blend(c, x + PANEL_R + i, y + WM_HEAD - 1, accent, (focused ? 150u : 40u) * fade / 256u);
        if (focused) ui_blend(c, x + PANEL_R + i, y + WM_HEAD - 2, accent, 45u * fade / 256u);
    }
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
    if (sn) {                                                                    /* подзаголовок - плашкой */
        int sw = gfx_text_width(sub);
        ui_round_rect(c, x + ow - 22 - sw, y + 7, sw + 16, 22, 8, TH_TRACK, 255);
        ui_round_frame(c, x + ow - 22 - sw, y + 7, sw + 16, 22, 8, TH_CARD_EDGE, 255);
        gfx_text(c, x + ow - 14 - sw, y + 10, sub, focused ? TH_TEXT_DIM : TH_TEXT_FAINT, -1);
    }
    blit_content(c, x + WM_PAD, y + WM_HEAD, disp_w(w), disp_h(w), w);
    if (w->resizable || w->zoomable) {
        uint32_t gc = resizing == id ? TH_CYAN : TH_RESIZE_IDLE;
        int bx = x + ow - 6, by = y + oh - 6;
        for (int a = 3; a <= 9; a += 3) gfx_line(c, bx - a, by, bx, by - a, gc);
    }
}

static void draw_fullscreen(surface_t* c, int ox, int oy, int id) {
    const win_t* w = &win[id];
    int fx, fy, dw, dh; geom(w, &fx, &fy, &dw, &dh);
    gfx_rect(c, 0, 0, c->w, c->h, TH_FULLSCREEN_BG);
    blit_content(c, fx + ox, fy + oy, dw, dh, w);
}

static void draw_pill(surface_t* c, int ox, int oy, int fs, int show) {
    const win_t* w = &win[fs];
    if (!show) { ui_round_rect(c, scr->w / 2 - 24 + ox, 4 + oy, 48, 4, 2, TH_CYAN, 110); return; }
    int x, y, pw, ph, n; pill_rect(w, &x, &y, &pw, &ph, &n);
    x += ox; y += oy;
    ui_round_rect(c, x, y, pw, ph, 18, TH_BAR0, 218);
    ui_round_frame(c, x, y, pw, ph, 18, ui_mix(TH_BORDER, TH_CYAN, 120), 230);
    for (int k = 0; k < n; k++) {
        int bx = x + 12 + k * 36 + 5, by = y + 8;
        ui_round_rect(c, bx, by, 26, 20, 7, TH_TILE, 190);
        ui_round_frame(c, bx, by, 26, 20, 7, TH_BORDER, 210);
        uint32_t glyph = TH_TEXT_DIM;
        if (k == 0) { gfx_line(c, bx + 8, by + 6, bx + 18, by + 14, glyph); gfx_line(c, bx + 18, by + 6, bx + 8, by + 14, glyph); }
        else if (k == 1) gfx_hline(c, bx + 7, by + 14, 12, glyph);
        else gfx_rect(c, bx + 8, by + 6, 10, 9, glyph);
    }
}

static void meter_color(unsigned pct, uint32_t* c) { *c = pct < 60 ? TH_GREEN : pct < 85 ? TH_AMBER : TH_RED; }

static void draw_topbar(surface_t* c, int ox, int oy) {
    int y = oy, W = scr->w;
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
        /* No full-width top bar: only a floating context capsule remains. */
        int gx = W >= 640 ? W - 270 : W - 56;
        int gw = W >= 640 ? 260 : 48;
        ui_round_rect(c, gx + ox, y + 7, gw, 34, TH_RADIUS_CONTROL + 4, TH_BAR0, 218);
        ui_round_frame(c, gx + ox, y + 7, gw, 34, TH_RADIUS_CONTROL + 4, TH_BORDER, 220);
        if (W >= 640) {
            ui_round_rect(c, gx + 7 + ox, y + 11, 62, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
            gfx_text(c, gx + 17 + ox, y + 18, "ОКНА", TH_TEXT_DIM, -1);
            ui_round_rect(c, gx + 76 + ox, y + 11, 58, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
            for (int wi = 0; wi < WM_WORKSPACE_MAX; wi++)
                ui_disc(c, gx + 87 + wi * 11 + ox, y + 24,
                        wi == current_workspace ? 3 : 2,
                        wi == current_workspace ? TH_CYAN : TH_TEXT_FAINT, 255);
            if (clock_hm[0]) ui_text_bold(c, gx + 143 + ox, y + 18, clock_hm, TH_TEXT);
            ui_round_rect(c, gx + 216 + ox, y + 11, 36, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
            ui_round_frame(c, gx + 216 + ox, y + 11, 36, 26, TH_RADIUS_CONTROL,
                           system_panel_open ? TH_CYAN : TH_BORDER, 220);
            icon_draw(c, gx + 234 + ox, y + 24, 16, ICON_MONITOR,
                      system_panel_open ? TH_CYAN : TH_TEXT_DIM, 0);
            if (notification_unread()) ui_disc(c, gx + 247 + ox, y + 12, 4, TH_AMBER, 255);
        } else {
            ui_round_rect(c, gx - 46 + ox, y + 11, 40, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
            char workspace_text[8] = { (char)('1' + current_workspace), '/', '4', 0 };
            gfx_text(c, gx - 37 + ox, y + 18, workspace_text, TH_TEXT_DIM, -1);
            ui_round_rect(c, gx + 6 + ox, y + 11, 36, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
            ui_round_frame(c, gx + 6 + ox, y + 11, 36, 26, TH_RADIUS_CONTROL,
                           system_panel_open ? TH_CYAN : TH_BORDER, 220);
            icon_draw(c, gx + 24 + ox, y + 24, 16, ICON_MONITOR,
                      system_panel_open ? TH_CYAN : TH_TEXT_DIM, 0);
            if (notification_unread()) ui_disc(c, gx + 37 + ox, y + 12, 4, TH_AMBER, 255);
        }
        return;
    }
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
    int rx = W - 14 + ox;
    if (m_health >= 0) {
        char t[8]; int n = 0, v = m_health; char d[4]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3);
        while (k) t[n++] = d[--k];
        t[n] = 0;
        uint32_t hc = m_health >= 70 ? TH_GREEN : m_health >= 50 ? TH_AMBER : TH_RED;
        rx -= gfx_text_width(t); ui_text_bold(c, rx, y + 12, t, hc); rx -= 14;
        ui_disc(c, rx + 4, y + 20, 4, hc, 255); ui_disc(c, rx + 4, y + 20, 7, hc, 50); rx -= 16;
    }
    if (m_mem >= 0) {
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
        t[n++] = '%';
        t[n] = 0;
        rx -= gfx_text_width(t); gfx_text(c, rx, y + 12, t, TH_TEXT_DIM, -1); rx -= 14;
    }
    if (m_cores > 0) {
        int shown = m_cores > 16 ? 16 : m_cores, per = (m_cores + shown - 1) / shown;
        int bw = 4, gap = 2, bx = rx - shown * (bw + gap);
        for (int i = 0; i < shown; i++) {
            unsigned sum = 0, cnt = 0;
            for (int k = i * per; k < (i + 1) * per && k < m_cores; k++) { sum += m_busy[k]; cnt++; }
            unsigned p = cnt ? sum / cnt : 0; uint32_t col; meter_color(p, &col);
            gfx_rect(c, bx + i * (bw + gap), y + 10, bw, 20, TH_METER_TRACK);
            int hgt = (int)(p * 20 / 100); if (hgt < 2) hgt = 2;
            gfx_rect(c, bx + i * (bw + gap), y + 30 - hgt, bw, hgt, col);
        }
        rx = bx - 16;
    }
    if (clock_hm[0]) {
        rx -= gfx_text_width(clock_hm); ui_text_bold(c, rx, y + 12, clock_hm, TH_TEXT); rx -= 20;
        gfx_rect(c, rx + 9, y + 12, 1, 16, TH_BORDER);
        const char* d = clock_date;
        int left_limit = tab_n ? tab_x(tab_n - 1) + tab_w(tab_n - 1) + 16 + ox : 200 + ox;
        while (*d && rx - gfx_text_width(d) < left_limit) {
            const char* comma = d; while (*comma && *comma != ',') comma++;
            if (!*comma) { d = ""; break; } d = comma + 2;
        }
        rx -= gfx_text_width(d); gfx_text(c, rx, y + 12, d, TH_TEXT_DIM, -1);
    }
}

static void draw_spatial_shell(surface_t* c, int ox, int oy) {
    int rail_w = spatial_rail_width();
    int rail_x = 8 + ox, rail_y = 8 + oy;
    int rail_h = scr->h - 16;
    int tab_h = spatial_tab_height();
    ui_round_rect(c, rail_x, rail_y, rail_w - 16, rail_h, TH_RADIUS_SURFACE, TH_BAR0, 242);
    ui_round_frame(c, rail_x, rail_y, rail_w - 16, rail_h, TH_RADIUS_SURFACE, TH_BORDER, 220);

    /* Brand and navigation header are part of the left workspace rail, not a top bar. */
    icon_draw(c, rail_x + 28, rail_y + 28, 26, ICON_BRAIN, TH_CYAN, 0);
    ui_text_bold(c, rail_x + 50, rail_y + 17, "KIRA", TH_TEXT);
    if (rail_w >= 160) gfx_text(c, rail_x + 50, rail_y + 36, "WORKSPACE", TH_TEXT_FAINT, -1);
    gfx_rect(c, rail_x + 14, rail_y + 58, rail_w - 44, 1, TH_BORDER);

    int act = active_tab();
    static const int nav_icon[WM_TAB_MAX] = {
        ICON_HOME, ICON_APPS, ICON_FOLDER, ICON_AI, ICON_MONITOR, ICON_GEAR, ICON_TERMINAL, ICON_WAVE
    };
    for (int i = 0; i < tab_n; i++) {
        int ty = spatial_tab_y(i) + oy;
        int tx = rail_x + 6;
        int tw = rail_w - 28;
        int icon_size = rail_w >= 160 ? 20 : 18;
        if (i == act) {
            ui_round_rect(c, tx, ty, tw, tab_h, TH_RADIUS_CONTROL, TH_ACCENT_PRIMARY, 34);
            ui_round_rect(c, tx, ty + 7, 3, tab_h - 14 > 0 ? tab_h - 14 : 4, 2, TH_CYAN, 255);
        } else if (i == tab_hover) {
            ui_round_rect(c, tx, ty, tw, tab_h, TH_RADIUS_CONTROL, TH_TILE, 220);
        }
        int icon_x = rail_w >= 160 ? tx + 25 : tx + tw / 2;
        icon_draw(c, icon_x, ty + tab_h / 2, icon_size, nav_icon[i],
                  i == act ? TH_CYAN : TH_TEXT_DIM, 0);
        if (rail_w >= 160) {
            gfx_text(c, tx + 48, ty + (tab_h - 8) / 2, tabs[i].label,
                     i == act ? TH_TEXT : (i == tab_hover ? TH_TEXT : TH_TEXT_DIM), -1);
        }
    }
    if (rail_w < 160 && tab_hover >= 0 && tab_hover < tab_n) {
        const char* label = tabs[tab_hover].label;
        int lw = gfx_text_width(label) + 20;
        int tx = rail_w + 8 + ox;
        int ty = spatial_tab_y(tab_hover) + oy + 4;
        ui_round_rect(c, tx, ty, lw, 28, TH_RADIUS_CONTROL, TH_BAR0, 255);
        ui_round_frame(c, tx, ty, lw, 28, TH_RADIUS_CONTROL, TH_BORDER, 255);
        gfx_text(c, tx + 10, ty + 8, label, TH_TEXT, -1);
    }

    /* The Dock contains pinned apps only; running unpinned windows never become task buttons. */
    int count = spatial_dock_count();
    if (!count) return;
    int tile = spatial_dock_tile(), pages = spatial_dock_pages();
    int width = spatial_dock_width();
    int dx = (scr->w - width) / 2 + ox, dy = spatial_dock_y() + oy - 8;
    ui_round_rect(c, dx, dy, width, tile + 16, TH_RADIUS_SURFACE, TH_BAR0, 248);
    ui_round_frame(c, dx, dy, width, tile + 16, TH_RADIUS_SURFACE, TH_BORDER, 255);
    if (pages > 1) {
        int cy = spatial_dock_y() + tile / 2 + oy;
        int lx = dx + WM_DOCK_CONTROL_W / 2 + 2;
        int rx = dx + width - WM_DOCK_CONTROL_W / 2 - 2;
        uint32_t lc = dock_page > 0 ? TH_TEXT : TH_TEXT_FAINT;
        uint32_t rc = dock_page + 1 < pages ? TH_TEXT : TH_TEXT_FAINT;
        ui_round_rect(c, lx - 18, cy - 20, 36, 40, TH_RADIUS_CONTROL, TH_TILE, 255);
        ui_round_rect(c, rx - 18, cy - 20, 36, 40, TH_RADIUS_CONTROL, TH_TILE, 255);
        gfx_line(c, lx + 3, cy - 7, lx - 3, cy, lc);
        gfx_line(c, lx - 3, cy, lx + 3, cy + 7, lc);
        gfx_line(c, rx - 3, cy - 7, rx + 3, cy, rc);
        gfx_line(c, rx + 3, cy, rx - 3, cy + 7, rc);
    }
    int top = wm_top();
    for (int i = 0; i < side_n; i++) {
        if (side[i].temp || side[i].bottom || spatial_dock_x(i) < 0) continue;
        int id = wm_find(side[i].tag);
        int running = id >= 0, shown = running && wm_visible(id), focused = shown && id == top;
        uint32_t col = side[i].color;
        int tx = spatial_dock_x(i) + ox, ty = spatial_dock_y() + oy;
        int icon_size = tile >= 40 ? 26 : tile >= 28 ? 20 : 14;
        int radius = tile >= 32 ? TH_RADIUS_CONTROL : 7;
        uint32_t fill = i == side_hover ? ui_mix(TH_TILE, col, 40) : TH_TILE;
        if (focused) ui_glow(c, tx, ty, tile, tile, 10, 3, col, 110);
        ui_round_rect(c, tx, ty, tile, tile, radius, fill, 255);
        ui_round_frame(c, tx, ty, tile, tile, radius, focused ? col : ui_mix(TH_BORDER, col, i == side_hover ? 150 : 70), 255);
        icon_draw(c, tx + tile / 2, ty + tile / 2, icon_size, side[i].icon, running ? col : ui_mix(col, TH_TEXT_FAINT, 90), 0);
        if (shown && tile >= 20) ui_round_rect(c, tx + tile / 3, ty + tile - 3, tile / 3, 2, 1, col, 255);
    }
    if (side_hover >= 0 && side_hover < side_n && !side[side_hover].temp && !side[side_hover].bottom) {
        const char* label = side[side_hover].label;
        int lw = gfx_text_width(label) + 20, tx = spatial_dock_x(side_hover) + tile / 2 - lw / 2 + ox;
        int ty = spatial_dock_y() + oy - 40;
        ui_round_rect(c, tx, ty, lw, 28, TH_RADIUS_CONTROL, TH_BAR0, 250);
        ui_round_frame(c, tx, ty, lw, 28, TH_RADIUS_CONTROL, side[side_hover].color, 180);
        gfx_text(c, tx + 10, ty + 8, label, TH_TEXT, -1);
    }
}

static void system_percent(char* out, int value) {
    if (value < 0) { str_copy(out, "—", 8); return; }
    if (value > 100) value = 100;
    if (value >= 100) { out[0] = '1'; out[1] = '0'; out[2] = '0'; out[3] = '%'; out[4] = 0; }
    else if (value >= 10) { out[0] = (char)('0' + value / 10); out[1] = (char)('0' + value % 10); out[2] = '%'; out[3] = 0; }
    else { out[0] = (char)('0' + value); out[1] = '%'; out[2] = 0; }
}

static void draw_system_panel(surface_t* c, int ox, int oy) {
    if (!system_panel_open || layout_mode != WM_LAYOUT_SPATIAL_MINIMAL) return;
    int x, y, w, h; system_panel_rect(&x, &y, &w, &h);
    ui_round_rect(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, TH_BAR0, 255);
    ui_round_frame(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, TH_BORDER, 255);
    gfx_text(c, x + 16 + ox, y + 12 + oy, "СИСТЕМА", TH_TEXT, -1);
    gfx_text(c, x + 16 + ox, y + 30 + oy, "Состояние устройства", TH_TEXT_DIM, -1);

    ui_round_rect(c, x + 12 + ox, y + 46 + oy, w - 24, 22, TH_RADIUS_CONTROL, TH_TILE, 255);
    gfx_text(c, x + 22 + ox, y + 52 + oy, "УВЕДОМЛЕНИЯ", TH_TEXT, -1);
    char unread_text[8]; int unread = notification_unread(), un = 0;
    if (unread > 99) unread = 99;
    if (unread >= 10) unread_text[un++] = (char)('0' + unread / 10);
    unread_text[un++] = (char)('0' + unread % 10); unread_text[un] = 0;
    ui_text_bold(c, x + w - 28 - gfx_text_width(unread_text) + ox, y + 52 + oy,
                 unread_text, unread ? TH_AMBER : TH_TEXT_FAINT);

    char cpu_text[8], mem_text[8], mind_text[8];
    char net_text[16], audio_text[16];
    int cpu = -1;
    if (m_cores > 0) {
        unsigned sum = 0;
        for (int i = 0; i < m_cores; i++) sum += m_busy[i];
        cpu = (int)(sum / (unsigned)m_cores);
    }
    system_percent(cpu_text, cpu);
    system_percent(mem_text, m_mem);
    system_percent(mind_text, m_health);
    if (m_network == -2) str_copy(net_text, "НЕТ КАРТЫ", sizeof net_text);
    else if (m_network < 0) str_copy(net_text, "СТАТУС ?", sizeof net_text);
    else if (m_network == 0) str_copy(net_text, "НЕТ ЛИНКА", sizeof net_text);
    else str_copy(net_text, "ЛИНК ЕСТЬ", sizeof net_text);
    if (m_audio_volume < 0) str_copy(audio_text, "НЕТ ЗВУКА", sizeof audio_text);
    else {
        int v = m_audio_volume > 100 ? 100 : m_audio_volume;
        int n = 0;
        if (v >= 100) audio_text[n++] = '1';
        if (v >= 10) audio_text[n++] = (char)('0' + (v / 10) % 10);
        audio_text[n++] = (char)('0' + v % 10); audio_text[n++] = '%'; audio_text[n] = 0;
    }

    const char* labels[5] = { "ПРОЦЕССОР", "ПАМЯТЬ", "KIRA MIND", "СЕТЬ", "ЗВУК" };
    const char* values[5] = { cpu_text, mem_text, mind_text, net_text, audio_text };
    int levels[5] = { cpu, m_mem, m_health, m_network < 0 ? -1 : m_network ? 100 : 0, m_audio_volume };
    int columns = (w - 40) / 2;
    if (columns < 56) columns = 56;
    for (int i = 0; i < 5; i++) {
        int row = i / 2, col = i % 2;
        int cx = x + 16 + col * (columns + 8);
        int cy = y + 74 + row * 30;
        int cw = i == 4 ? w - 32 : columns;
        ui_round_rect(c, cx + ox, cy + oy, cw, 28, TH_RADIUS_CONTROL, TH_TILE, 255);
        gfx_text(c, cx + 8 + ox, cy + 4 + oy, labels[i], TH_TEXT_DIM, -1);
        int vw = gfx_text_width(values[i]);
        ui_text_fit(c, cx + cw - 8 - vw + ox, cy + 4 + oy, values[i], vw, 
                    i == 3 ? (m_network > 0 ? TH_GREEN : TH_AMBER) :
                    i == 4 ? (m_audio_volume >= 0 ? TH_CYAN : TH_TEXT_FAINT) :
                    levels[i] < 0 ? TH_TEXT_FAINT : TH_TEXT);
        if (levels[i] >= 0) {
            int level = levels[i] > 100 ? 100 : levels[i];
            uint32_t color;
            if (i == 2) color = level >= 70 ? TH_GREEN : level >= 50 ? TH_AMBER : TH_RED;
            else if (i == 3) color = level ? TH_GREEN : TH_RED;
            else color = i == 4 ? TH_CYAN : (level < 60 ? TH_GREEN : level < 85 ? TH_AMBER : TH_RED);
            ui_round_rect(c, cx + 8 + ox, cy + 20 + oy, cw - 16, 3, 1, TH_BAR1, 255);
            ui_round_rect(c, cx + 8 + ox, cy + 20 + oy, (cw - 16) * level / 100, 3, 1, color, 255);
        }
    }
    gfx_text(c, x + 16 + ox, y + h - 12 + oy, "Нажмите на шкалу звука, чтобы изменить громкость", TH_TEXT_FAINT, -1);
}

static uint32_t notification_color(unsigned priority) {
    return priority >= WM_NOTIFY_CRITICAL ? TH_RED : priority == WM_NOTIFY_WARNING ? TH_AMBER : TH_CYAN;
}
static void draw_notification_toast(surface_t* c, int ox, int oy) {
    if (!notification_toast_active || !notification_count) return;
    unsigned at = (notification_head + WM_NOTIFICATION_MAX - 1) % WM_NOTIFICATION_MAX;
    const wm_notification_t* n = &notifications[at];
    int x, y, w, h; notification_toast_rect(&x, &y, &w, &h);
    uint32_t col = notification_color(n->priority);
    ui_round_rect(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, TH_BAR0, 255);
    ui_round_frame(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, col, 230);
    ui_round_rect(c, x + 10 + ox, y + 12 + oy, 4, h - 24, 2, col, 255);
    ui_text_fit(c, x + 24 + ox, y + 12 + oy, n->title, w - 42, col);
    ui_text_fit(c, x + 24 + ox, y + 38 + oy, n->message, w - 42, TH_TEXT);
}
static void draw_notification_center(surface_t* c, int ox, int oy) {
    if (!notification_center_open) return;
    int x, y, w, h; notification_center_rect(&x, &y, &w, &h);
    ui_round_rect(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, TH_BAR0, 255);
    ui_round_frame(c, x + ox, y + oy, w, h, TH_RADIUS_SURFACE, TH_BORDER, 255);
    gfx_text(c, x + 16 + ox, y + 14 + oy, "УВЕДОМЛЕНИЯ", TH_TEXT, -1);
    gfx_text(c, x + 16 + ox, y + 34 + oy, notification_count ? "Последние системные события" : "Новых событий нет", TH_TEXT_DIM, -1);
    int clear_w = 76;
    ui_round_rect(c, x + w - clear_w - 12 + ox, y + 10 + oy, clear_w, 26, TH_RADIUS_CONTROL, TH_TILE, 255);
    ui_round_frame(c, x + w - clear_w - 12 + ox, y + 10 + oy, clear_w, 26, TH_RADIUS_CONTROL, TH_BORDER, 200);
    gfx_text(c, x + w - clear_w - 2 + ox, y + 18 + oy, "Очистить", TH_TEXT_DIM, -1);
    int visible = (h - 58) / 48; if (visible > 6) visible = 6;
    for (int i = 0; i < visible && (unsigned)i < notification_count; i++) {
        unsigned at = (notification_head + WM_NOTIFICATION_MAX - 1 - (unsigned)i) % WM_NOTIFICATION_MAX;
        const wm_notification_t* n = &notifications[at];
        int ry = y + 48 + i * 48;
        uint32_t col = notification_color(n->priority);
        ui_round_rect(c, x + 12 + ox, ry + oy, w - 24, 42, TH_RADIUS_CONTROL, TH_TILE, 255);
        ui_round_rect(c, x + 12 + ox, ry + 8 + oy, 3, 26, 1, col, 255);
        ui_text_fit(c, x + 24 + ox, ry + 5 + oy, n->title, w - 48, col);
        ui_text_fit(c, x + 24 + ox, ry + 22 + oy, n->message, w - 48, TH_TEXT_DIM);
    }
}
static void draw_side(surface_t* c, int ox, int oy) {
    if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) { draw_spatial_shell(c, ox, oy); return; }
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
        int running = id >= 0, shown = running && wm_visible(id), focused = shown && id == top;
        uint32_t col = side[i].color;
        int tx = SIDE_X + 6 + ox, ty = side_item_y(i) + oy;
        uint32_t fill = i == side_hover ? ui_mix(TH_TILE, col, 40) : TH_TILE;
        if (focused) ui_glow(c, tx, ty, TILE, TILE, 12, 3, col, 140);
        ui_round_rect(c, tx, ty, TILE, TILE, 12, fill, 255);
        ui_round_frame(c, tx, ty, TILE, TILE, 12, focused ? col : ui_mix(TH_BORDER, col, i == side_hover ? 160 : 70), 255);
        icon_draw(c, tx + TILE / 2, ty + TILE / 2, 26, side[i].icon, running ? col : ui_mix(col, TH_TEXT_FAINT, 90), 1);
        if (side[i].badge) {
            char b[2] = { side[i].badge, 0 };
            ui_disc(c, tx + TILE - 9, ty + TILE - 9, 8, TH_BAR0, 255);
            ui_ring(c, tx + TILE - 9, ty + TILE - 9, 7, 8, col, 255);
            gfx_text(c, tx + TILE - 13, ty + TILE - 17, b, TH_TEXT, -1);
        }
        if (running) ui_round_rect(c, SIDE_X + 1 + ox, ty + TILE / 2 - (shown ? 8 : 3), 3, shown ? 16 : 6, 2, shown ? col : TH_TEXT_FAINT, 255);
    }
    if (side_hover >= 0) {
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


static void overview_grid(int* cols, int* rows, int* per, int* left, int* top, int* cw, int* ch) {
    *cols = scr->w >= 900 ? 3 : scr->w >= 600 ? 2 : 1;
    *rows = scr->h >= 560 ? 2 : 1;
    *per = *cols * *rows;
    *left = scr->w < 480 ? 12 : 28;
    *top = scr->h < 360 ? 78 : 100;
    int gap = scr->w < 480 ? 8 : 14;
    int bottom = scr->h < 360 ? 48 : 68;
    *cw = (scr->w - 2 * *left - (*cols - 1) * gap) / *cols;
    *ch = (scr->h - *top - bottom - (*rows - 1) * gap) / *rows;
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

static int overview_card_rect(int slot, int* x, int* y, int* w, int* h) {
    int cols, rows, per, left, top, cw, ch;
    overview_grid(&cols, &rows, &per, &left, &top, &cw, &ch);
    if (slot < 0 || slot >= per || overview_page * per + slot >= overview_n) return -1;
    *x = left + (slot % cols) * (cw + 14);
    *y = top + (slot / cols) * (ch + 14);
    *w = cw; *h = ch;
    return 0;
}

int wm_window_overview_item_center(int item, int* x, int* y) {
    int rx, ry, rw, rh;
    if (!overview_open || !x || !y || overview_card_rect(item, &rx, &ry, &rw, &rh) != 0) return -1;
    *x = rx + rw / 2; *y = ry + rh / 2;
    return 0;
}

static void overview_damage_window(int id) {
    if (!overview_open || id < 0 || id >= WM_MAX_WIN || !win[id].used) return;
    int cols, rows, per, left, top, cw, ch;
    overview_grid(&cols, &rows, &per, &left, &top, &cw, &ch);
    for (int i = 0; i < overview_n; i++) {
        if (overview_ids[i] != id || overview_generations[i] != win[id].generation) continue;
        if (i / per != overview_page) return;
        int x, y, w, h;
        if (overview_card_rect(i % per, &x, &y, &w, &h) == 0) damage(x, y, x + w, y + h);
        return;
    }
}

static int overview_hit_item(int px, int py) {
    int cols, rows, per, left, top, cw, ch;
    overview_grid(&cols, &rows, &per, &left, &top, &cw, &ch);
    for (int i = 0; i < per; i++) {
        int x, y, w, h;
        if (overview_card_rect(i, &x, &y, &w, &h) == 0 &&
            px >= x && px < x + w && py >= y && py < y + h) return overview_page * per + i;
    }
    return -1;
}

static void overview_button(surface_t* c, int ox, int oy, int x, int y, int w, const char* label, int enabled) {
    uint32_t edge = enabled ? TH_CYAN : TH_BORDER;
    ui_round_rect(c, x + ox, y + oy, w, 32, 9, TH_TILE, 255);
    ui_round_frame(c, x + ox, y + oy, w, 32, 9, edge, enabled ? 210 : 90);
    int tw = gfx_text_width(label);
    gfx_text(c, x + ox + (w - tw) / 2, y + oy + 10, label, enabled ? TH_TEXT : TH_TEXT_FAINT, -1);
}

static void draw_overview(surface_t* c, int ox, int oy) {
    gfx_rect(c, 0, 0, c->w, c->h, TH_BG0);
    ui_round_rect(c, 16 + ox, 42 + oy, scr->w - 32, scr->h - 58, 20, TH_BAR0, 255);
    ui_round_frame(c, 16 + ox, 42 + oy, scr->w - 32, scr->h - 58, 20, TH_BORDER, 255);
    gfx_text(c, 36 + ox, 58 + oy, "ОКНА", TH_TEXT, -1);
    if (scr->w >= 480) gfx_text(c, 118 + ox, 60 + oy, "Выберите окно для возврата", TH_TEXT_DIM, -1);
    overview_button(c, ox, oy, scr->w - 104, 50, 72, "Закрыть", 1);

    int cols, rows, per, left, top, cw, ch;
    overview_grid(&cols, &rows, &per, &left, &top, &cw, &ch);
    int first = overview_page * per;
    if (!overview_n) {
        gfx_text(c, scr->w / 2 - 74 + ox, scr->h / 2 + oy, "Нет открытых окон", TH_TEXT_DIM, -1);
    }
    for (int slot = 0; slot < per && first + slot < overview_n; slot++) {
        int id = overview_ids[first + slot];
        if (id < 0 || id >= WM_MAX_WIN || !win[id].used) continue;
        const win_t* w = &win[id];
        int x, y, ww, hh;
        if (overview_card_rect(slot, &x, &y, &ww, &hh) != 0) continue;
        int focused = id == wm_top();
        ui_round_rect(c, x + ox, y + oy, ww, hh, 14, TH_CARD, 255);
        ui_round_frame(c, x + ox, y + oy, ww, hh, 14, focused ? TH_CYAN : TH_CARD_EDGE, focused ? 255 : 180);
        int px = x + 10, py = y + 10, pw = ww - 20, ph = hh - 62;
        if (ph < 32) ph = 32;
        ui_round_rect(c, px + ox, py + oy, pw, ph, 8, TH_CONTENT, 255);
        if (w->content.px && w->content.w > 0 && w->content.h > 0)
            blit_content(c, px + ox, py + oy, pw, ph, w);
        ui_text_fit(c, x + 12 + ox, y + hh - 42 + oy, w->title, ww - 24, TH_TEXT);
        gfx_text(c, x + 12 + ox, y + hh - 23 + oy, w->visible ? "ОТКРЫТО" : "СВЁРНУТО",
                 w->visible ? TH_CYAN : TH_TEXT_FAINT, -1);
        if (focused) ui_round_rect(c, x + ww - 18 + ox, y + hh - 18 + oy, 6, 6, 3, TH_CYAN, 255);
    }
    int pages = overview_n ? (overview_n + per - 1) / per : 1;
    overview_button(c, ox, oy, 32, scr->h - 48, 88, "Назад", overview_page > 0);
    overview_button(c, ox, oy, scr->w - 120, scr->h - 48, 88, "Далее", overview_page + 1 < pages);
}

static void overview_click(int px, int py) {
    int cols, rows, per, left, top, cw, ch;
    overview_grid(&cols, &rows, &per, &left, &top, &cw, &ch);
    int pages = overview_n ? (overview_n + per - 1) / per : 1;
    if (px >= scr->w - 104 && px < scr->w - 32 && py >= 50 && py < 82) {
        overview_open = 0; damage(0, 0, scr->w, scr->h); return;
    }
    if (px >= 32 && px < 120 && py >= scr->h - 48 && py < scr->h - 16) {
        if (overview_page > 0) { overview_page--; damage(0, 0, scr->w, scr->h); }
        return;
    }
    if (px >= scr->w - 120 && px < scr->w - 32 && py >= scr->h - 48 && py < scr->h - 16) {
        if (overview_page + 1 < pages) { overview_page++; damage(0, 0, scr->w, scr->h); }
        return;
    }
    int index = overview_hit_item(px, py);
    if (index >= 0 && index < overview_n) {
        int id = overview_ids[index];
        if (id >= 0 && id < WM_MAX_WIN && win[id].used && !win[id].pinned && win[id].generation == overview_generations[index]) {
            wm_set_visible(id, 1);
            wm_raise(id);
        }
    }
    overview_open = 0;
    damage(0, 0, scr->w, scr->h);
}

static void draw_cursor(surface_t* c, int ox, int oy) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            char ch = cursor_art[j][i];
            if (ch == '#') gfx_pixel(c, cx + i + ox, cy + j + oy, TH_CURSOR_EDGE);
            else if (ch == '.') gfx_pixel(c, cx + i + ox, cy + j + oy, 0x00FFFFFF);
        }
}

static int cull_enabled = 1;
void wm_set_culling(int on) { cull_enabled = on; }

/* Признаки, меняющие всю картинку (полноэкранный режим, плашка, вкладка), - до сборки изменённых мест. */
static int compose_prepare(void) {
    if (overview_open && overview_refresh()) damage(0, 0, scr->w, scr->h);
    int fs = fs_top();
    if (fs != last_fs) { last_fs = fs; damage(0, 0, scr->w, scr->h); }
    int pill = pill_visible(fs);
    if (pill != last_pill) { last_pill = pill; damage_pill(); }
    int at = active_tab();
    if (at != last_tab) {
        int old_tab = last_tab;
        last_tab = at;
        if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
            damage_spatial_tab(old_tab);
            damage_spatial_tab(at);
        } else damage_top();
    }
    return fs;
}

/* Собрать прямоугольник экрана [x0,x1)x[y0,y1). Только читает состояние оконного менеджера: несколько
   прямоугольников собираются параллельно, пока поток дисплея ждёт (wm_compose_par). */
static void compose_rect(int fs, int x0, int y0, int x1, int y1) {
    int rw = x1 - x0, rh = y1 - y0;
    int pill = last_pill;
    surface_t clip = { scr->px + (long)y0 * scr->pitch + x0, rw, rh, scr->pitch };
    int ox = -x0, oy = -y0;

    if (overview_open) {
        draw_overview(&clip, ox, oy);
        draw_cursor(&clip, ox, oy);
        return;
    }

    /* Ищем самое верхнее окно, которое целиком закрывает изменённую область: всё, что под ним, рисовать незачем.
       Окно сверху «задевает» область вместе со своим свечением. */
    int seq[WM_MAX_WIN], ns = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < zcount; i++) {
            const win_t* wn = &win[zorder[i]];
            if (wn->visible && window_on_workspace(wn) && wn->pinned == (pass == 0)) seq[ns++] = zorder[i];
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
            if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) gfx_rect(&clip, 0, 0, rw, rh, TH_BG0);
            else {
                if (bg) gfx_blit(&clip, 0, 0, &bgs, x0, y0, rw, rh);
                else gfx_rect(&clip, 0, 0, rw, rh, TH_BG0);
                if (fs < 0) brainfx_draw(&clip, ox, oy, anim_ms);
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
        /* Both layouts have a top system zone. Spatial draws only its compact
           brand/window-overview/clock strip, never the Legacy tab bar. */
        if (y0 < WM_TOP_H) draw_topbar(&clip, ox, oy);
        if (layout_mode == WM_LAYOUT_SPATIAL_MINIMAL) {
            if (x0 < spatial_rail_width() + 220 || y1 > scr->h - WM_SPATIAL_DOCK_H) draw_side(&clip, ox, oy);
            if (system_panel_open) draw_system_panel(&clip, ox, oy);
        } else if (x0 < WM_SIDE_W + 300) draw_side(&clip, ox, oy);
    } else draw_pill(&clip, ox, oy, fs, pill);
    if (notification_center_open) draw_notification_center(&clip, ox, oy);
    else if (notification_toast_active) draw_notification_toast(&clip, ox, oy);
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
