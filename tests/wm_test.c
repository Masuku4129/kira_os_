#include "wm.h"
#include "theme.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static int notes, last_id, last_vis, launches, last_launch, tabs_hit, last_tab;
static int in_n, in_id, in_type, in_x, in_y, in_b;
static void inp(int id, int type, int x, int y, unsigned b) { in_n++; in_id = id; in_type = type; in_x = x; in_y = y; in_b = (int)b; }
static void* al(uint64_t n) { return calloc(1, n); }
static void nt(int id, int v) { notes++; last_id = id; last_vis = v; }
static void lc(int i) { launches++; last_launch = i; }
static void tb(int i) { tabs_hit++; last_tab = i; }
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


/* Док: состояния значка (нет окна / свёрнуто / открыто / в фокусе), щелчки, несколько окон одной программы,
   сжатие плиток, когда значков много. Полоса слева от плитки: высокая цветная - открыто, короткая тусклая - свёрнуто. */
static uint32_t ind(surface_t* sc, int i) { int sx, sy; wm_side_center(i, &sx, &sy); frame(); return px(sc, 11, sy); }
static void dock_states(void) {
    static char tags[24][8], labels[24][8];
    surface_t sc = { calloc(1024 * 768, 4), 1024, 768, 1024 };
    wm_ops_t ops = { al, nt, inp, lc, rz, tb };
    wm_init(&sc, &ops);
    wm_side_item_t it[24];
    for (int i = 0; i < 24; i++) { snprintf(tags[i], 8, "p%d", i); snprintf(labels[i], 8, "P%d", i); it[i] = (wm_side_item_t){ labels[i], tags[i], TH_PINK, ICON_NET, 0, 0, 0, 0, 0 }; }
    it[5] = (wm_side_item_t){ "+", "", TH_TEXT_DIM, ICON_FOLDER, 1, 0, 0, 0, 0 };
    wm_side_set(it, 6);                                              /* p0..p4 и «+» внизу */
    frame();
    uint32_t col = TH_PINK, faint = TH_TEXT_FAINT;
    CHECK(ind(&sc, 1) != col && ind(&sc, 1) != faint);               /* окна нет - полосы нет */
    int w1 = wm_create("Рисование", 300, 100, 200, 100, WM_CLOSABLE); wm_set_tag(w1, "p1");
    int w2 = wm_create("Другое", 340, 140, 200, 100, WM_CLOSABLE); wm_set_tag(w2, "p2");
    CHECK(ind(&sc, 1) == col && ind(&sc, 2) == col);                 /* оба показаны (открыто); p2 сверху (в фокусе) */
    CHECK(ind(&sc, 3) != col && ind(&sc, 3) != faint);
    click_side(2);                                                   /* сверху: щелчок сворачивает */
    CHECK(!wm_visible(w2) && ind(&sc, 2) == faint && ind(&sc, 1) == col);
    click_side(2);                                                   /* свёрнуто: щелчок возвращает и выводит вперёд */
    CHECK(wm_visible(w2) && wm_top() == w2 && ind(&sc, 2) == col);
    click_side(1);                                                   /* открыто, но не сверху: щелчок выводит вперёд, не сворачивает */
    CHECK(wm_visible(w1) && wm_top() == w1);
    click_side(1);
    CHECK(!wm_visible(w1));
    wm_destroy(w1);                                                  /* окно закрыто - полоса исчезает, плитка остаётся (закреплённая) */
    CHECK(ind(&sc, 1) != col && ind(&sc, 1) != faint);
    /* две программы-окна с одной меткой: щелчок по значку переходит к следующему окну, число окон - в углу */
    int w3 = wm_create("Копия", 380, 180, 200, 100, WM_CLOSABLE); wm_set_tag(w3, "p2");
    CHECK(wm_top() == w3);
    click_side(2); CHECK(wm_top() == w2 && wm_visible(w3));
    click_side(2); CHECK(wm_top() == w3);
    wm_destroy(w3); wm_destroy(w2);
    CHECK(wm_find("p2") < 0 && ind(&sc, 2) != col);

    /* сжатие: плиток много - уменьшаются, но не налезают друг на друга и остаются в панели; сверх предела - нужен «ещё N» */
    int cap = wm_side_capacity();
    CHECK(cap >= 14 && cap < WM_SIDE_MAX + 1);
    for (int n = 4; n <= cap; n += 3) {
        wm_side_item_t all[24];
        for (int i = 0; i < n - 1; i++) { all[i] = (wm_side_item_t){ labels[i % 24], tags[i % 24], TH_PINK, ICON_NET, 0, i >= n / 2, 0, 0, 0 }; }
        all[n - 1] = it[5];
        wm_side_set(all, n);
        int ts = wm_side_tile(), py = -1;
        CHECK(ts >= 26 && ts <= 44);
        if (n * 54 - 10 > 670) CHECK(ts < 44);
        for (int i = 0; i < n; i++) {
            int sx, sy; CHECK(wm_side_center(i, &sx, &sy) == 0);
            CHECK(sy - ts / 2 >= WM_TOP_H + 10 && sy + ts / 2 <= 768 - 10);
            if (i && !all[i].bottom) CHECK(sy - py >= ts);
            if (!all[i].bottom) py = sy;
        }
        launches = 0; click_side(n / 2);
        CHECK(launches == 1 && last_launch == n / 2);                /* щелчок попадает в сжатую плитку */
    }
    wm_side_set(it, 6);
    free(sc.px);
}

static void responsive_topbar(void) {
    static const int dims[][2] = { {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1920, 1080} };
    static const wm_tab_t many[] = {
        { "ГЛАВНАЯ", "" }, { "РАЗУМ", "" }, { "ПРОВОДНИК", "" }, { "НАСТРОЙКИ", "" },
        { "ДРАЙВЕРЫ", "" }, { "СЕТЬ", "" }, { "МОНИТОР", "" }, { "ПРИЛОЖЕНИЯ", "" }
    };
    uint8_t busy[16];
    for (int i = 0; i < 16; i++) busy[i] = (uint8_t)(i * 7);
    for (unsigned d = 0; d < sizeof dims / sizeof dims[0]; d++) {
        int w = dims[d][0], h = dims[d][1];
        surface_t sc = { calloc((size_t)w * h, 4), w, h, w };
        CHECK(sc.px != 0);
        wm_ops_t o = { al, nt, inp, lc, rz, tb };
        wm_init(&sc, &o);
        wm_tabs_set(many, (int)(sizeof many / sizeof many[0]));
        int app = wm_create("Проверка окна", WM_AUTO, WM_AUTO, 600, 564, WM_ZOOMABLE);
        CHECK(app >= 0);
        int wx, wy; wm_window_pos(app, &wx, &wy);
        int pct = wm_zoom_percent(app);
        int dw = 600 * pct / 100, dh = 564 * pct / 100;
        CHECK(pct >= 50 && pct <= 100);
        CHECK(wx >= 0 && wy >= WM_TOP_H && wx + dw + 2 * WM_PAD <= w + 4);
        CHECK(wy + dh + WM_HEAD + WM_PAD <= h + 4);
        wm_destroy(app);
        wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:05");
        wm_set_meters(busy, 16, 37, 91);
        frame();
        int prev = 0;
        for (int i = 0; i < (int)(sizeof many / sizeof many[0]); i++) {
            int tx, ty;
            CHECK(wm_tab_center(i, &tx, &ty) == 0);
            CHECK(tx > prev && tx < w - 8 && ty >= 7 && ty < WM_TOP_H);
            prev = tx;
            tabs_hit = 0;
            click(tx, ty);
            CHECK(tabs_hit == 1 && last_tab == i);
        }
        free(sc.px);
    }
}

int main(void) {
    surface_t scr = { calloc(1024 * 768, 4), 1024, 768, 1024 };
    wm_ops_t ops = { al, nt, inp, lc, rz, tb };
    wm_init(&scr, &ops);
    int x, y, w, h;
    wm_side_item_t side[3] = { { "A", "A", TH_CYAN, ICON_TERMINAL, 0 }, { "Z", "zzz", TH_PURPLE, ICON_AI, 0 }, { "S", "set", TH_TEXT_DIM, ICON_GEAR, 1 } };
    wm_side_set(side, 3);
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
    /* Контролы заголовка нейтральные, а не красно-жёлто-зелёные заливки. */
    for (int bi = 1; bi <= 3; bi++) {
        int bx, by; CHECK(wm_button_center(a, bi, &bx, &by) == 0);
        uint32_t control = px(&scr, bx, by);
        CHECK(control != TH_CLOSE && control != TH_MIN && control != TH_FULL);
    }
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
    /* Домашняя карточка появляется на большом экране и получает живые системные показатели. */
    {
        uint8_t home_busy[4] = { 72, 38, 21, 33 };
        wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:05");
        wm_set_meters(home_busy, 4, 37, 91);
        frame();
        CHECK(px(&scr, 800, 620) == TH_BAR0);
    }
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

    /* Абсолютный указатель (virtio-планшет/сенсор): крайние координаты и одиночное касание. */
    wm_mouse_abs(0, 0, 0);
    { int px_, py_; wm_cursor(&px_, &py_); CHECK(px_ == 0 && py_ == 0); }
    wm_mouse_abs(32767, 32767, 0);
    { int px_, py_; wm_cursor(&px_, &py_); CHECK(px_ == 1023 && py_ == 767); }
    int touch = wm_create("touch", 100, 100, 200, 120, WM_INPUT);
    CHECK(touch >= 0);
    int tax = (100 + WM_PAD + 50) * 32767 / 1023;
    int tay = (100 + WM_HEAD + 40) * 32767 / 767;
    wm_mouse_abs((uint16_t)tax, (uint16_t)tay, 0);
    in_n = 0; wm_mouse_abs((uint16_t)tax, (uint16_t)tay, 1);
    CHECK(in_n >= 1 && in_id == touch && in_type == 4 && in_b == 1);
    CHECK(in_x >= 49 && in_x <= 50 && in_y >= 39 && in_y <= 40);
    wm_mouse_abs((uint16_t)tax, (uint16_t)tay, 0);
    wm_destroy(touch);
    wm_mouse_abs(16384, 16384, 0);
    { int px_, py_; wm_cursor(&px_, &py_); CHECK(px_ >= 511 && px_ <= 512 && py_ >= 383 && py_ <= 384); }

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
    wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:06");
    CHECK(wm_compose(&x, &y, &w, &h) && y == 0 && h > WM_TOP_H + 20); /* верхняя панель + домашняя карточка */
    wm_set_clock("ПЯТНИЦА, 30 ОКТЯБРЯ 2026", "13:06");
    CHECK(!wm_compose(&x, &y, &w, &h));
    wm_set_meters(busy, 4, 37, 91);
    CHECK(wm_compose(&x, &y, &w, &h));
    wm_set_meters(busy, 4, 37, 91);
    CHECK(!wm_compose(&x, &y, &w, &h));
    wm_set_subtitle(a, "ядро");
    CHECK(wm_compose(&x, &y, &w, &h));

    /* Уведомления: отображение поверх окон, ограничение очереди и автоматическое исчезновение. */
    {
        uint32_t before = px(&scr, 800, WM_TOP_H + 40);
        wm_notify("Система", "Проверка уведомлений", WM_NOTIFY_SUCCESS);
        CHECK(wm_compose(&x, &y, &w, &h));
        uint32_t shown = px(&scr, 1000, WM_TOP_H + 18);
        CHECK(shown != before);
        wm_notify("Mind", "Обнаружено событие", WM_NOTIFY_WARNING);
        wm_notify("Драйверы", "Устройство готово", WM_NOTIFY_INFO);
        wm_notify("Сеть", "Подключение восстановлено", WM_NOTIFY_SUCCESS);
        CHECK(wm_compose(&x, &y, &w, &h));
        wm_anim(6000, 0);
        CHECK(wm_compose(&x, &y, &w, &h));
        CHECK(px(&scr, 1000, WM_TOP_H + 18) != shown);
        wm_notify("Система", "Очистка очереди", WM_NOTIFY_INFO);
        wm_notify_clear();
        CHECK(wm_compose(&x, &y, &w, &h));
    }

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
    dock_states();
    responsive_topbar();
    printf("wm: shell top bar (responsive tabs and fitted app windows at 640x480, 800x600, 1024x768, 1280x720, 1920x1080; clock and meters), side launcher, traffic-light buttons, z-order, damage, drag, "
           "fullscreen, pinned, app and kernel input, zoom, owner resize, live background, culling = full redraw, dock states / cycling / compression passed\n");
    return 0;
}
