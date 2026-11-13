#include "wm.h"
#include "theme.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static int notes, last_id, last_vis, launches, last_launch, tabs_hit, last_tab, launcher_hits;
static int in_n, in_id, in_type, in_x, in_y, in_b;
static void inp(int id, int type, int x, int y, unsigned b) { in_n++; in_id = id; in_type = type; in_x = x; in_y = y; in_b = (int)b; }
static void* al(uint64_t n) { return calloc(1, n); }
static void nt(int id, int v) { notes++; last_id = id; last_vis = v; }
static void lc(int i) { launches++; last_launch = i; }
static void tb(int i) { tabs_hit++; last_tab = i; }
static void launcher(void) { launcher_hits++; }
static int rz_n, rz_id, rz_w, rz_h;
static void rz(int id, int w, int h) { rz_n++; rz_id = id; rz_w = w; rz_h = h; }
static uint32_t px(surface_t* s, int x, int y) { return s->px[y * s->pitch + x]; }
static int X, Y;
static void move_to(int x, int y, unsigned b) { int cx, cy; wm_cursor(&cx, &cy); wm_mouse(x - cx, y - cy, b); }
static void click(int x, int y) { move_to(x, y, 0); wm_mouse(0, 0, 1); wm_mouse(0, 0, 0); }
static void click_btn(int id, int which) { int bx, by; CHECK(wm_button_center(id, which, &bx, &by) == 0); click(bx, by); }
static void click_side(int i) { int sx, sy; CHECK(wm_side_center(i, &sx, &sy) == 0); click(sx, sy); }
static void frame(void) { wm_compose(&X, &Y, &X, &Y); }

/* Отсечение закрытых слоёв даёт ту же картинку, что полная перерисовка с нуля. */
static void same_as_full(surface_t* scr, const char* what) {
    static uint32_t snap[1024 * 768];
    frame();
    memcpy(snap, scr->px, sizeof snap);
    wm_set_culling(0); wm_invalidate_all(); frame(); wm_set_culling(1);
    if (memcmp(snap, scr->px, sizeof snap)) {
        int n = 0, fx = -1, fy = -1;
        for (int i = 0; i < 1024 * 768; i++) if (snap[i] != scr->px[i]) { if (!n) { fx = i % 1024; fy = i / 1024; } n++; }
        printf("%s: %d px differ, first at %d,%d\n", what, n, fx, fy);
    }
    CHECK(memcmp(snap, scr->px, sizeof snap) == 0);
}

int main(void) {
    surface_t scr = { calloc(1024 * 768, 4), 1024, 768, 1024 };
    wm_ops_t ops = { al, nt, inp, lc, rz, tb, 0, launcher };
    wm_init(&scr, &ops);
    CHECK(wm_get_layout_mode() == WM_LAYOUT_LEGACY);
    int x, y, w, h;
    wm_side_item_t side[4] = { { "A", "A", TH_CYAN, ICON_TERMINAL, 0 }, { "Z", "zzz", TH_PURPLE, ICON_AI, 0 }, { "S", "set", TH_TEXT_DIM, ICON_GEAR, 1 }, { "RUN", "run", TH_CYAN, ICON_TERMINAL, 0, 1 } };
    wm_side_set(side, 4);
    wm_tab_t tabs[3] = { { "ГЛАВНАЯ", "" }, { "РАЗУМ", "A" }, { "НАСТРОЙКИ", "set" } };
    wm_tabs_set(tabs, 3);

    int a = wm_create("A", 300, 100, 200, 100, WM_CLOSABLE);
    int b = wm_create("B", 350, 140, 200, 100, 0);
    CHECK(a == 0 && b == 1);
    wm_set_tag(a, "A");
    gfx_fill(wm_content(a), 0x00AA0000);
    gfx_fill(wm_content(b), 0x0000BB00);
    CHECK(wm_compose(&x, &y, &w, &h) && x == 0 && y == 0 && w == 1024 && h == 768);   /* первый кадр - весь экран */
    CHECK(!wm_compose(&x, &y, &w, &h));                                               /* без изменений - ничего */
    /* содержимое лежит со смещением (поля, заголовок); B выше A */
    CHECK(px(&scr, 400, 200) == 0x0000BB00);
    CHECK(px(&scr, 320, 140) == 0x00AA0000);
    int ox, oy, sc;
    CHECK(wm_content_origin(a, &ox, &oy, &sc) == 0 && ox == 300 + WM_PAD && oy == 100 + WM_HEAD && sc == 1);
    /* фон нарисован (градиент, сетка, мозг), а не пустой чёрный; верхняя панель и боковая есть */
    int lit = 0; for (int i = 1024 * 100; i < 1024 * 700; i++) if (scr.px[i] != 0) lit++;
    CHECK(lit > 1024 * 590);
    CHECK(px(&scr, 512, 20) != px(&scr, 512, 300));
    int sx0, sy0; wm_side_center(0, &sx0, &sy0);
    CHECK(sx0 < WM_SIDE_W && sy0 > WM_TOP_H);
    int sx2, sy2; wm_side_center(2, &sx2, &sy2);
    CHECK(sy2 > 600);                                                                 /* «внизу» - прижат к низу панели */

    /* перемещение курсора даёт маленький damage */
    move_to(600, 600, 0); frame();
    move_to(610, 605, 0);
    CHECK(wm_compose(&x, &y, &w, &h) && w < 100 && h < 100);
    /* клик по видимой части A поднимает его */
    click(320, 150);
    CHECK(wm_top() == a);
    frame();
    CHECK(px(&scr, 400, 200) == 0x00AA0000);

    /* перетаскивание за заголовок */
    int ax, ay; wm_window_pos(a, &ax, &ay);
    move_to(ax + 120, ay + 10, 0);
    wm_mouse(0, 0, 1); wm_mouse(50, 30, 1); wm_mouse(0, 0, 0);
    int nx, ny; wm_window_pos(a, &nx, &ny);
    CHECK(nx == ax + 50 && ny == ay + 30);
    /* нельзя утащить под верхнюю панель и целиком за экран */
    move_to(nx + 120, ny + 10, 0);
    wm_mouse(0, 0, 1); wm_mouse(-5000, -5000, 1); wm_mouse(0, 0, 0);
    wm_window_pos(a, &nx, &ny);
    CHECK(ny >= WM_TOP_H);
    move_to(nx + 120, ny + 10, 0);
    wm_mouse(0, 0, 1); wm_mouse(9000, 9000, 1); wm_mouse(0, 0, 0);
    wm_window_pos(a, &nx, &ny);
    CHECK(nx <= 1024 - 80 && ny <= 768 - WM_HEAD);
    /* вернём A на видное место */
    move_to(nx + 120, ny + 10, 0);
    wm_mouse(0, 0, 1); wm_mouse(-(nx - 300), 100 - ny, 1); wm_mouse(0, 0, 0);
    wm_window_pos(a, &nx, &ny);
    CHECK(nx == 300 && ny == 100);
    frame();

    /* «светофор»: жёлтая - свернуть */
    notes = 0;
    click_btn(a, 2);
    CHECK(notes == 1 && last_id == a && last_vis == 0 && !wm_visible(a));
    /* боковая панель: значок A возвращает свёрнутое окно, повторный клик сворачивает снова */
    click_side(0);
    CHECK(wm_visible(a) && notes == 2 && last_vis == 1 && wm_top() == a);
    click_side(0);
    CHECK(!wm_visible(a) && notes == 3);
    click_side(0);
    CHECK(wm_visible(a));
    /* значок без окна запускает приложение */
    launches = 0; click_side(1);
    CHECK(launches == 1 && last_launch == 1);
    /* подсказка под курсором над значком рисуется */
    move_to(sx0, sy0, 0);
    CHECK(wm_compose(&x, &y, &w, &h) && x == 0 && w > WM_SIDE_W);
    /* красная у окна ядра без WM_CLOSABLE-приложения сворачивает (вернуть можно значком) */
    wm_raise(b); notes = 0; click_btn(b, 1);
    CHECK(!wm_visible(b) && notes == 1 && last_vis == 0);
    wm_set_visible(b, 1); wm_raise(a);

    /* разделы верхней панели */
    tabs_hit = 0;
    int tx, ty; CHECK(wm_tab_center(2, &tx, &ty) == 0 && ty < WM_TOP_H);
    click(tx, ty);
    CHECK(tabs_hit == 1 && last_tab == 2);
    wm_minimize_all();
    CHECK(!wm_visible(a) && !wm_visible(b) && wm_top() == -1);
    wm_set_visible(b, 1); wm_set_visible(a, 1);
    CHECK(wm_top() == a);

    /* на весь экран (зелёная): содержимое растягивается в целое число раз по центру */
    frame();
    click_btn(a, 3);
    CHECK(wm_fullscreen(a));
    frame();
    CHECK(wm_content_origin(a, &ox, &oy, &sc) == 0 && sc == 5 && ox == (1024 - 1000) / 2 && oy == (768 - 500) / 2);
    CHECK(px(&scr, 512, 384) == 0x00AA0000);               /* центр - содержимое */
    CHECK(px(&scr, 4, 700) == 0x0004070E);                 /* вне содержимого - тёмный фон без панелей */
    /* панель кнопок сверху: курсор в зоне - появляется, третья кнопка - обратно */
    move_to(512, 20, 0); frame();
    click(446 + 12 + 2 * 36 + 18, 26);
    CHECK(!wm_fullscreen(a));
    /* снова: на весь экран и сворачивание кнопкой панели (вторая) */
    wm_set_fullscreen(a, 1); frame();
    move_to(512, 20, 0); frame();
    notes = 0; click(446 + 12 + 36 + 18, 26);
    CHECK(!wm_visible(a) && notes == 1 && last_vis == 0);
    wm_set_fullscreen(a, 0); wm_set_visible(a, 1); frame();

    /* закреплённая панель: не двигается, не берёт фокус, кнопок нет */
    int p = wm_create("P", 90, 50, 100, 100, WM_PINNED);
    gfx_fill(wm_content(p), 0x00123456);
    wm_raise(p);
    CHECK(wm_top() != p);
    CHECK(wm_button_center(p, 1, &x, &y) != 0);
    int ppx, ppy; wm_window_pos(p, &ppx, &ppy);
    move_to(ppx + 40, ppy + 10, 0);
    wm_mouse(0, 0, 1); wm_mouse(100, 100, 1); wm_mouse(0, 0, 0);
    int qx, qy; wm_window_pos(p, &qx, &qy);
    CHECK(qx == ppx && qy == ppy);

    /* окно приложения: чужая память, события мыши в координатах содержимого, красная кнопка не скрывает */
    uint32_t* ext = calloc(100 * 60, 4);
    int u = wm_create_user("app", 600, 300, 100, 60, ext);
    CHECK(u >= 0 && wm_content(u)->px == ext);
    wm_set_tag(u, "app");
    gfx_fill(wm_content(u), 0x00123456);
    frame();
    CHECK(px(&scr, 650, 350) == 0x00123456);
    int ux, uy; wm_window_pos(u, &ux, &uy);
    in_n = 0; move_to(ux + WM_PAD + 10, uy + WM_HEAD + 5, 0);
    CHECK(in_n >= 1 && in_id == u && in_type == 3 && in_x == 10 && in_y == 5);
    in_n = 0; wm_mouse(0, 0, 1);
    CHECK(in_n == 1 && in_type == 4 && in_b == 1);
    in_n = 0; wm_mouse(4, 4, 1);
    CHECK(in_n == 1 && in_type == 3 && in_x == 14 && in_y == 9);
    in_n = 0; wm_mouse(1000, 0, 1);                        /* захват: движение за окном доходит, координата прижата к краю */
    CHECK(in_n == 1 && in_id == u && in_x == 99);
    in_n = 0; wm_mouse(0, 0, 0);
    CHECK(in_n == 1 && in_type == 4 && in_b == 0);
    /* во весь экран мышь пересчитывается: 100x60 -> x10 */
    wm_set_fullscreen(u, 1); frame();
    wm_content_origin(u, &ox, &oy, &sc);
    CHECK(sc == 10 && ox == 12 && oy == 84);
    in_n = 0; move_to(ox + 55, oy + 75, 0);
    CHECK(in_n >= 1 && in_x == 5 && in_y == 7);
    wm_set_fullscreen(u, 0); frame();
    /* красная у окна приложения шлёт запрос закрытия, окно остаётся */
    notes = 0; click_btn(u, 1);
    CHECK(notes == 1 && last_id == u && last_vis == -1 && wm_visible(u));
    CHECK(wm_find("app") == u && wm_find("nope") == -1);
    wm_destroy(u);
    CHECK(!wm_visible(u) && wm_content(u) == 0);
    CHECK(wm_compose(&x, &y, &w, &h));                     /* место окна перерисовано */
    CHECK(px(&scr, 650, 350) != 0x00123456);

    /* окно ядра с WM_INPUT получает мышь, как приложение */
    int k = wm_create("kin", 500, 420, 120, 80, WM_INPUT);
    int kx, ky; wm_window_pos(k, &kx, &ky);
    in_n = 0; move_to(kx + WM_PAD + 7, ky + WM_HEAD + 9, 0);
    CHECK(in_n >= 1 && in_id == k && in_type == 3 && in_x == 7 && in_y == 9);
    in_n = 0; wm_mouse(0, 0, 1); wm_mouse(0, 0, 0);
    CHECK(in_n == 2 && in_type == 4 && in_b == 0);
    /* окно без WM_INPUT - нет */
    in_n = 0; move_to(400, 200, 0);
    CHECK(in_n == 0 || in_id != b);
    wm_destroy(k);

    /* часы, индикаторы и подзаголовок: верхняя панель перерисовывается только при изменении */
    uint8_t busy[4] = { 10, 50, 90, 30 };
    frame();
    wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:05");
    CHECK(wm_compose(&x, &y, &w, &h) && y == 0 && h <= WM_TOP_H + 20);
    wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:05");
    CHECK(!wm_compose(&x, &y, &w, &h));
    wm_set_meters(busy, 4, 37, 91);
    CHECK(wm_compose(&x, &y, &w, &h));
    wm_set_meters(busy, 4, 37, 91);
    CHECK(!wm_compose(&x, &y, &w, &h));
    wm_set_subtitle(a, "ядро");
    CHECK(wm_compose(&x, &y, &w, &h));

    /* окно, созданное с WM_AUTO, помещается на экран правее боковой панели */
    int au = wm_create("auto", WM_AUTO, WM_AUTO, 680, 432, 0);
    int axx, ayy; wm_window_pos(au, &axx, &ayy);
    CHECK(axx >= WM_SIDE_W && ayy >= WM_TOP_H && axx + 680 + 2 * WM_PAD <= 1024 && ayy + 432 + WM_HEAD + WM_PAD <= 768);
    wm_destroy(au);

    /* изменение размера уголком: приложение масштабируется (прилипание к 200%), мышь пересчитывается */
    uint32_t* zb = calloc(100 * 60, 4);
    int z = wm_create_user("zoom", 300, 300, 100, 60, zb);
    gfx_fill(wm_content(z), 0x00445566);
    wm_raise(z); frame();
    int zx, zy; wm_window_pos(z, &zx, &zy);
    CHECK(wm_zoom_percent(z) == 100);
    int ow = 100 + 2 * WM_PAD, oh = 60 + WM_HEAD + WM_PAD;
    move_to(zx + ow - 8, zy + oh - 8, 0);
    wm_mouse(0, 0, 1);
    wm_mouse(100, 60, 1);
    CHECK(wm_zoom_percent(z) == 200);
    wm_mouse(0, 0, 0);
    frame();
    CHECK(px(&scr, zx + WM_PAD + 150, zy + WM_HEAD + 90) == 0x00445566);
    in_n = 0; move_to(zx + WM_PAD + 100, zy + WM_HEAD + 60, 0);
    CHECK(in_n >= 1 && in_id == z && in_x == 50 && in_y == 30);
    wm_window_pos(z, &zx, &zy);
    move_to(zx + 200 + 2 * WM_PAD - 8, zy + 120 + WM_HEAD + WM_PAD - 8, 0);
    wm_mouse(0, 0, 1); wm_mouse(-170, -100, 1); wm_mouse(0, 0, 0);
    CHECK(wm_zoom_percent(z) < 100 && wm_zoom_percent(z) >= 50);
    wm_destroy(z);

    /* окно с изменяемым размером (терминал): владелец получает размер, кратный шагу, в пределах [min, cap] */
    int cid = wm_create_ex("con", 100, 100, 320, 160, WM_RESIZABLE, 1024, 768);
    wm_set_resize_limits(cid, 160, 96, 8, 16);
    wm_raise(cid); frame();
    int cwx, cwy; wm_window_pos(cid, &cwx, &cwy);
    rz_n = 0;
    move_to(cwx + 320 + 2 * WM_PAD - 8, cwy + 160 + WM_HEAD + WM_PAD - 8, 0);
    wm_mouse(0, 0, 1); wm_mouse(103, 61, 1); wm_mouse(0, 0, 0);
    CHECK(rz_n >= 1 && rz_id == cid && rz_w % 8 == 0 && rz_h % 16 == 0 && rz_w >= 400 && rz_h >= 208);
    int cw_, ch_; wm_content_size(cid, &cw_, &ch_);
    CHECK(cw_ == rz_w && ch_ == rz_h);
    move_to(cwx + cw_ + 2 * WM_PAD - 8, cwy + ch_ + WM_HEAD + WM_PAD - 8, 0);
    wm_mouse(0, 0, 1); wm_mouse(-3000, -3000, 1); wm_mouse(0, 0, 0);
    wm_content_size(cid, &cw_, &ch_);
    CHECK(cw_ == 160 && ch_ == 96);
    wm_set_fullscreen(cid, 1);
    wm_content_size(cid, &cw_, &ch_);
    CHECK(cw_ == 1024 && ch_ == 768 && rz_w == 1024 && rz_h == 768);
    wm_set_fullscreen(cid, 0);
    wm_content_size(cid, &cw_, &ch_);
    CHECK(cw_ == 160 && ch_ == 96);
    wm_set_content_size(cid, 200, 100);
    wm_content_size(cid, &cw_, &ch_);
    CHECK(cw_ == 200 && ch_ == 100);
    wm_destroy(cid);

    /* живой фон: вспышки узлов отмечают свои области; при выключенной анимации - тишина */
    frame();
    wm_set_fx(3);
    wm_anim(1000, 0);
    wm_anim(1200, 2);                                       /* решение Разума: цепочка импульсов */
    CHECK(wm_compose(&x, &y, &w, &h));
    wm_set_fx(0);
    for (uint32_t t = 3000; t < 6000; t += 16) wm_anim(t, 0);   /* старые вспышки догорают */
    frame();
    wm_anim(6100, 5);
    CHECK(!wm_compose(&x, &y, &w, &h));
    wm_set_fx(3);
    wm_anim(7000, 1); wm_anim(7300, 1);

    /* отсечение закрытых слоёв = полная перерисовка: окна, свечение фокуса, вспышки фона под окнами */
    {
        int ids[2] = { a, b };
        same_as_full(&scr, "start");
        for (int round = 0; round < 6; round++) {
            for (int q = 0; q < 2; q++) {
                int id = ids[q];
                uint32_t colr = 0x00100000u * (unsigned)(round + 1) + (unsigned)(q * 77);
                if (round & 1) gfx_rect(wm_content(id), 0, 5, wm_content(id)->w, 35, colr); else gfx_fill(wm_content(id), colr);
                if (round & 1) wm_invalidate_rows(id, 5, 40); else wm_invalidate(id);
                same_as_full(&scr, "content update");
            }
            wm_raise(ids[round & 1]);
            same_as_full(&scr, "after raise");
            wm_anim(7400 + (uint32_t)round * 50, 1);
            same_as_full(&scr, "pulses");
        }
    }
    /* Spatial Minimal is a real, opt-in layout; legacy remains the default. */
    wm_set_layout_mode(WM_LAYOUT_SPATIAL_MINIMAL);
    CHECK(wm_get_layout_mode() == WM_LAYOUT_SPATIAL_MINIMAL);
    frame();
    /* Spatial keeps a compact system bar, not the Legacy tab/telemetry bar. */
    CHECK(px(&scr, 500, 20) == TH_BAR0);
    CHECK(px(&scr, 500, WM_TOP_H - 1) == TH_BORDER);
    int nav_x, nav_y, dock_x, dock_y;
    CHECK(wm_tab_center(1, &nav_x, &nav_y) == 0 && nav_x < WM_SIDE_W && nav_y >= WM_TOP_H);
    CHECK(wm_side_center(0, &dock_x, &dock_y) == 0 && dock_y > 768 - 100);
    CHECK(wm_side_center(2, &dock_x, &dock_y) == -1); /* bottom action is not a Dock pin */
    CHECK(wm_side_center(3, &dock_x, &dock_y) == -1); /* running, unpinned apps are not in the Dock */
    tabs_hit = 0; launcher_hits = 0;
    click(240, 20);
    CHECK(launcher_hits == 0 && tabs_hit == 0); /* application launcher is no longer duplicated in the top bar */
    for (int tab = 0; tab < 3; tab++) {
        CHECK(wm_tab_center(tab, &nav_x, &nav_y) == 0 && nav_x < WM_SPATIAL_RAIL_W && nav_y >= WM_TOP_H);
        click(nav_x, nav_y);
        CHECK(last_tab == tab);
    }
    CHECK(tabs_hit == 3);
    wm_set_visible(a, 0);
    click_side(0);
    CHECK(wm_top() == a);

    /* Window overview includes minimized windows, excludes pinned widgets, and restores the selected window. */
    int ov_input = wm_create("overview-input", 600, 400, 100, 60, WM_INPUT);
    wm_set_visible(a, 0);
    wm_window_overview_toggle();
    CHECK(wm_window_overview_is_open());
    CHECK(wm_window_overview_count() == 3); /* a, b and overview-input; pinned widget is excluded */
    frame();
    same_as_full(&scr, "Spatial window overview");
    int ovx, ovy; wm_window_pos(ov_input, &ovx, &ovy);
    in_n = 0;
    move_to(ovx + WM_PAD + 10, ovy + WM_HEAD + 10, 0);
    CHECK(in_n == 0); /* overlay must not leak pointer input to the underlying app */
    CHECK(wm_window_overview_item_center(1, &nav_x, &nav_y) == 0); /* hidden a follows the frontmost input window */
    click(nav_x, nav_y);
    CHECK(in_n == 0); /* including the release edge, no event reaches the covered app */
    CHECK(!wm_window_overview_is_open() && wm_visible(a) && wm_top() == a);
    wm_destroy(ov_input);

    /* The visible Spatial top-bar affordance opens the same overlay; close button exits without selecting. */
    click(140, 20);
    CHECK(wm_window_overview_is_open());
    frame();
    click(1024 - 68, 66);
    CHECK(!wm_window_overview_is_open());
    frame();
    same_as_full(&scr, "Spatial Minimal");

    /* Spatial system status is modal: it must not leak pointer input to a covered app. */
    int panel_under = wm_create("panel-underlay", 740, 50, 120, 100, WM_INPUT);
    wm_raise(panel_under);
    int panel_under_top = wm_top();
    in_n = 0;
    click(1024 - 26, 20);
    CHECK(wm_system_panel_is_open());
    move_to(780, 70, 0);
    CHECK(in_n == 0);
    click(780, 70);
    CHECK(in_n == 0 && wm_system_panel_is_open());
    frame();
    uint8_t panel_busy[2] = { 20, 80 };
    wm_set_meters(panel_busy, 2, 65, 75);
    CHECK(wm_compose(&x, &y, &w, &h)); /* live telemetry invalidates the open panel */
    frame();
    CHECK(px(&scr, 780, 70) == TH_BAR0);
    click(500, 300);
    CHECK(!wm_system_panel_is_open() && wm_top() == panel_under_top);
    wm_destroy(panel_under);
    frame();
    click(1024 - 26, 20);
    CHECK(wm_system_panel_is_open());
    click(1024 - 26, 20);
    CHECK(!wm_system_panel_is_open());
    frame();

    /* Hidden app content can still refresh its overview thumbnail without exposing the window. */
    wm_set_visible(a, 0);
    wm_window_overview_toggle();
    CHECK(wm_window_overview_count() == 2);
    frame();
    gfx_fill(wm_content(a), 0x00334455);
    wm_invalidate(a);
    same_as_full(&scr, "Spatial overview hidden content update");
    CHECK(wm_window_overview_item_center(0, &nav_x, &nav_y) == 0);
    click(nav_x, nav_y);
    CHECK(wm_visible(a) && wm_top() == a);

    /* A destroyed window ID may be reused before the next compose; stale overview cards must not select the replacement. */
    wm_window_overview_toggle();
    CHECK(wm_window_overview_count() == 2);
    frame();
    CHECK(wm_window_overview_item_center(1, &nav_x, &nav_y) == 0); /* second card is b */
    wm_destroy(b);
    int replacement = wm_create("replacement", 500, 300, 100, 60, WM_INPUT);
    CHECK(replacement == b); /* exercise immediate slot reuse */
    wm_raise(a);
    click(nav_x, nav_y);
    CHECK(!wm_window_overview_is_open() && wm_top() == a);
    wm_destroy(replacement);
    frame(); /* consume window lifecycle damage before isolating telemetry invalidation */

    uint8_t spatial_busy[1] = { 99 };
    wm_set_meters(spatial_busy, 1, 88, 20);
    CHECK(!wm_compose(&x, &y, &w, &h)); /* hidden telemetry must not invalidate the idle shell */
    wm_set_clock("13 НОЯБРЯ", "13:05");
    CHECK(wm_compose(&x, &y, &w, &h));
    wm_set_layout_mode(WM_LAYOUT_LEGACY);
    CHECK(wm_get_layout_mode() == WM_LAYOUT_LEGACY);
    frame();
    same_as_full(&scr, "Legacy after Spatial Minimal");

    /* Short-screen Spatial rail must fit above the pinned-only Dock. */
    surface_t small = { calloc(800 * 400, 4), 800, 400, 800 };
    CHECK(small.px != 0);
    wm_init_mode(&small, &ops, WM_LAYOUT_SPATIAL_MINIMAL);
    wm_tab_t small_tabs[7] = {
        { "ГЛАВНАЯ", "" }, { "ПРИЛОЖЕНИЯ", "Launcher" },
        { "ПРОВОДНИК", "Files" }, { "KIRA AI", "Mind" },
        { "МОНИТОР", "Monitor" }, { "НАСТРОЙКИ", "Settings" },
        { "ТЕРМИНАЛ", "Console" }
    };
    wm_tabs_set(small_tabs, 7);
    wm_side_set(side, 4);
    frame();
    int last_nav_x, last_nav_y, pin_x, pin_y;
    CHECK(wm_tab_center(6, &last_nav_x, &last_nav_y) == 0);
    CHECK(wm_side_center(0, &pin_x, &pin_y) == 0);
    CHECK(last_nav_y + 16 < pin_y - 30); /* nav hit target ends above Dock surface */
    tabs_hit = 0;
    click(last_nav_x, last_nav_y);
    CHECK(tabs_hit == 1 && last_tab == 6);

    /* Narrow Spatial viewport: all ten pinned Dock entries stay visible and clickable. */
    surface_t narrow = { calloc(320 * 240, 4), 320, 240, 320 };
    CHECK(narrow.px != 0);
    wm_init_mode(&narrow, &ops, WM_LAYOUT_SPATIAL_MINIMAL);
    static const char* pin_labels[10] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J" };
    static const char* pin_tags[10] = { "P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9" };
    wm_side_item_t narrow_pins[10];
    for (int i = 0; i < 10; i++)
        narrow_pins[i] = (wm_side_item_t){ pin_labels[i], pin_tags[i], TH_CYAN, ICON_TERMINAL, 0, 0, 0, 0 };
    wm_side_set(narrow_pins, 10);
    frame();
    int previous_x = -1;
    for (int i = 0; i < 10; i++) {
        int px_, py_;
        CHECK(wm_side_center(i, &px_, &py_) == 0);
        CHECK(px_ >= 0 && px_ < 320 && py_ >= 0 && py_ < 240);
        CHECK(px_ > previous_x);
        previous_x = px_;
    }
    launches = 0;
    for (int i = 0; i < 10; i++) click_side(i);
    CHECK(launches == 10 && last_launch == 9);

    printf("wm: shell top bar (tabs, clock, meters), side launcher, traffic-light buttons, z-order, damage, drag, "
           "fullscreen, pinned, app and kernel input, zoom, owner resize, live background, culling = full redraw passed\n");
    return 0;
}
