// Хост-тест программы cube3d: её настоящий код (рендерер, самопроверка, главный цикл) против подменённых системных вызовов.
// Заглушку включаем первой (тот же охранный макрос KIRA_USER_H), поэтому настоящий userland/kira.h не подключается.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "stub/kira.h"
char stub_log[4096];
#include "../userland/cube3d.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t hash(const u32* p, int n) { uint32_t h = 0; for (int i = 0; i < n; i++) h = h * 31 + p[i]; return h; }

static void save_ppm(const char* path, const surf_t* s) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        u32 p = s->px[y * s->pitch + x]; unsigned char c[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

int main(void) {
    mkdir("/tmp/kira_gfx", 0777);
    CHECK(selftest() == 0);

    enum { PW = 360, PH = 270 };
    static u32 px[PW * PH];
    surf_t s = { px, PW, PH, PW };
    u32* z = calloc((size_t)PW * PH, 4);
    CHECK(scene_init(z, PW, PH) == 0);
    render_scene(&s, 3000000000ULL);
    CHECK(stats.tris_drawn >= 6 && stats.tris_drawn <= 20 && stats.pixels_written > 5000);
    u32 h1 = hash(px, PW * PH);
    render_scene(&s, 3000000000ULL);
    CHECK(h1 == hash(px, PW * PH));                                    // детерминирован: время - единственный вход
    render_scene(&s, 9000000000ULL);
    CHECK(h1 != hash(px, PW * PH));                                    // время действительно вращает сцену
    render_scene(&s, 3000000000ULL);
    save_ppm("/tmp/kira_gfx/scene.ppm", &s);

    // окно в подповерхности экрана (pitch != w) совпадает с отдельным рендером и не выходит за свои границы
    static u32 big[1024 * 768];
    surf_t scr = { big, 1024, 768, 1024 };
    surf_t sub = { big + 40 * 1024 + 480, PW, PH, 1024 };
    for (int i = 0; i < 1024 * 768; i++) big[i] = 0x123456;
    render_scene(&sub, 3000000000ULL);
    int same = 1; for (int y = 0; y < PH && same; y++) for (int x = 0; x < PW; x++) if (big[(40 + y) * 1024 + 480 + x] != px[y * PW + x]) { same = 0; break; }
    CHECK(same);
    CHECK(big[39 * 1024 + 480] == 0x123456 && big[(40 + PH) * 1024 + 480] == 0x123456 && big[40 * 1024 + 479] == 0x123456);
    (void)scr;

    // защита от неверных данных: нет z-буфера, нулевой размер, окно другого размера - ничего не рисуется
    CHECK(scene_init(0, PW, PH) == -1 && scene_init(z, 0, PH) == -1 && scene_init(z, PW, -5) == -1);
    CHECK(scene_init(z, PW, PH) == 0);
    static u32 other[100 * 100];
    surf_t wrong = { other, 100, 100, 100 };
    render_scene(&wrong, 1000);
    int untouched = 1; for (int i = 0; i < 100 * 100; i++) if (other[i]) untouched = 0;
    CHECK(untouched);

    // главная функция: окно, куча 95 страниц, цикл до закрытия окна, итог в журнал
    stub_gfx_ok = 1; stub_close_at = 25; stub_ev_calls = 0; stub_presents = 0; stub_log[0] = 0;
    CHECK(umain() == 0);
    CHECK(strstr(stub_log, "selftest PASS") && strstr(stub_log, "software renderer running"));
    CHECK(stub_last_heap_pages == 95);                                  // 360*270*4 = 388800 байт
    CHECK(stub_presents >= 20 && stub_presents <= 25);
    CHECK(strstr(stub_log, " frames, ") && strstr(stub_log, " triangles, "));
    // отказ окна и отказ памяти - понятное сообщение и код 1
    stub_gfx_ok = 0; stub_log[0] = 0;
    CHECK(umain() == 1 && strstr(stub_log, "cannot create window"));
    stub_gfx_ok = 1; stub_heap_ok = 0; stub_log[0] = 0;
    CHECK(umain() == 1 && strstr(stub_log, "z-buffer"));

    free(z);
    if (fails) { printf("cube3d: %d FAILED\n", fails); return 1; }
    printf("cube3d: self-test, render (deterministic, sub-surface, bounds), z-buffer from heap, program loop and failure paths passed\n");
    return 0;
}
