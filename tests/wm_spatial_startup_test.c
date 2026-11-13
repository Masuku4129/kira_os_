#include "wm.h"
#include "theme.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\\n", __LINE__, #c); return 1; } } while (0)
static unsigned allocations;
static void* alloc_surface(uint64_t bytes) { allocations++; return calloc(1, (size_t)bytes); }
int main(void) {
    surface_t screen = { calloc(1024 * 768, 4), 1024, 768, 1024 };
    CHECK(screen.px != 0);
    wm_ops_t ops = { alloc_surface, 0, 0, 0, 0, 0, 0, 0, 0 };
    wm_init_mode(&screen, &ops, WM_LAYOUT_SPATIAL_MINIMAL);
    CHECK(wm_get_layout_mode() == WM_LAYOUT_SPATIAL_MINIMAL);
    CHECK(allocations == 0);
    wm_tab_t nav[] = {
        { "ГЛАВНАЯ", "" }, { "ПРИЛОЖЕНИЯ", "Launcher" }, { "ПРОВОДНИК", "Files" },
        { "KIRA AI", "Mind" }, { "МОНИТОР", "Monitor" }, { "НАСТРОЙКИ", "Settings" }, { "ТЕРМИНАЛ", "Console" }
    };
    wm_side_item_t pins[] = {
        { "Paint", "paint", TH_CYAN, ICON_TERMINAL, 0, 0, 0, 0 },
        { "Cube", "cube3d", TH_PURPLE, ICON_AI, 0, 0, 0, 0 }
    };
    wm_tabs_set(nav, (int)(sizeof nav / sizeof nav[0]));
    wm_side_set(pins, (int)(sizeof pins / sizeof pins[0]));
    int x, y, w, h;
    CHECK(wm_tab_center(1, &x, &y) == 0 && x < WM_SPATIAL_RAIL_W && y >= WM_TOP_H);
    CHECK(wm_side_center(0, &x, &y) == 0 && y > 768 - 100);
    CHECK(wm_compose(&x, &y, &w, &h) == 1);
    CHECK(allocations == 0);
    free(screen.px);
    puts("Spatial Minimal startup: PASS");
    return 0;
}
