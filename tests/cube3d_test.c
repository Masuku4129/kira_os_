// Хост-тест программы cube3d: её настоящий код (сцена на движке k3d, самопроверка, главный цикл) против подменённых системных вызовов.
// Заглушку включаем первой (тот же охранный макрос KIRA_USER_H), поэтому настоящий userland/kira.h не подключается.
// Потоки процесса и futex - pthread и futex Linux (tests/stub/kira.h). Сам движок проверяет tests/k3d_test.c; здесь - программа целиком.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "stub/kira.h"
#include "stub/kira_threads.h"
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

static u64 arena_pages(int workers, u32 tris, u32 res) {
    k3d_config_t cfg; k3d_config_default(&cfg, W, H, workers); k3d_kira_sys(&cfg.sys); cfg.max_tris = tris; cfg.res_bytes = res;
    return (k3d_mem_size(&cfg) + 4095) / 4096;
}
static void* arena_for(int workers, u64* bytes) { u64 p = arena_pages(workers, 4096, 1u << 20); *bytes = p * 4096; return calloc(1, (size_t)*bytes); }

int main(void) {
    mkdir("/tmp/kira_gfx", 0777);
    CHECK(selftest() == 0);                                            // тригонометрия, обход граней, растеризатор, глубина, текстура, свет, потоки-помощники

    enum { PW = 360, PH = 270 };
    static u32 px[PW * PH];
    surf_t s = { px, PW, PH, PW };
    u64 ab = 0; void* arena = arena_for(3, &ab);
    CHECK(scene_init(arena, ab, PW, PH, 3) == 0);
    CHECK(render_scene(&s, 3000000000ULL) == 0);
    CHECK(sc.last.tris_drawn >= 100 && sc.last.tris_drawn <= 1500 && sc.last.px_written > 5000);   // куб, октаэдр, планета (сетка), стекло
    CHECK(sc.last.flushes == 1);                                                                   // сцена целиком помещается в пулы
    u32 h1 = hash(px, PW * PH);
    CHECK(render_scene(&s, 3000000000ULL) == 0 && h1 == hash(px, PW * PH));       // детерминирован: время - единственный вход
    CHECK(render_scene(&s, 9000000000ULL) == 0 && h1 != hash(px, PW * PH));       // время действительно вращает сцену
    CHECK(render_scene(&s, 3000000000ULL) == 0);
    save_ppm("/tmp/kira_gfx/scene.ppm", &s);
    CHECK(k3d_workers(sc.c) == 3);

    // в кадре есть всё: фон-градиент, цвета граней куба (кирпич с оттенком), стекло (прозрачность), глубина (перекрытия)
    int bg = 0, nonbg = 0; for (int i = 0; i < PW * PH; i++) { if (px[i] == px[0]) bg++; else nonbg++; }
    CHECK(px[0] != px[(PH - 1) * PW] && nonbg > 8000);

    // те же кадры без помощников дают тот же результат до бита (параллельная растеризация плиток не меняет картинку)
    uint32_t hw[4]; u64 times[4] = { 0, 1500000000ULL, 5200000000ULL, 12300000000ULL };
    for (int k = 0; k < 4; k++) { CHECK(render_scene(&s, times[k]) == 0); hw[k] = hash(px, PW * PH); }
    scene_close();
    u64 ab0 = 0; void* arena0 = arena_for(0, &ab0);
    CHECK(scene_init(arena0, ab0, PW, PH, 0) == 0 && k3d_workers(sc.c) == 0);
    for (int k = 0; k < 4; k++) { CHECK(render_scene(&s, times[k]) == 0); CHECK(hash(px, PW * PH) == hw[k]); }
    scene_close(); free(arena0);

    // окно в подповерхности экрана (pitch != w) совпадает с отдельным рендером и не выходит за свои границы
    CHECK(scene_init(arena, ab, PW, PH, 3) == 0);
    static u32 big[1024 * 768];
    surf_t sub = { big + 40 * 1024 + 480, PW, PH, 1024 };
    for (int i = 0; i < 1024 * 768; i++) big[i] = 0x123456;
    CHECK(render_scene(&sub, 3000000000ULL) == 0);
    CHECK(render_scene(&s, 3000000000ULL) == 0);
    int same = 1; for (int y = 0; y < PH && same; y++) for (int x = 0; x < PW; x++) if (big[(40 + y) * 1024 + 480 + x] != px[y * PW + x]) { same = 0; break; }
    CHECK(same);
    CHECK(big[39 * 1024 + 480] == 0x123456 && big[(40 + PH) * 1024 + 480] == 0x123456 && big[40 * 1024 + 479] == 0x123456 && big[40 * 1024 + 480 + PW] == 0x123456);

    // защита от неверных данных: нет памяти, нулевой размер, окно другого размера - ничего не рисуется
    scene_close();
    CHECK(scene_init(0, ab, PW, PH, 3) == -1 && scene_init(arena, ab, 0, PH, 3) == -1 && scene_init(arena, ab, PW, -5, 3) == -1 && scene_init(arena, 1000, PW, PH, 3) == -1);
    static u32 other[100 * 100];
    surf_t wrong = { other, 100, 100, 100 };
    CHECK(render_scene(&wrong, 1000) == -1);                                         // движок не создан
    CHECK(scene_init(arena, ab, PW, PH, 3) == 0);
    CHECK(render_scene(&wrong, 1000) == -1);                                         // окно другого размера
    int untouched = 1; for (int i = 0; i < 100 * 100; i++) if (other[i]) untouched = 0;
    CHECK(untouched);
    scene_close();
    free(arena);

    // главная функция: окно, куча под движок, цикл до закрытия окна, итог в журнал
    stub_gfx_ok = 1; stub_close_at = 25; stub_ev_calls = 0; stub_presents = 0; stub_log[0] = 0; stub_args[0] = 0;
    CHECK(umain() == 0);
    CHECK(strstr(stub_log, "selftest PASS") && strstr(stub_log, "k3d engine running, 3 helper threads"));
    CHECK(stub_last_heap_pages == arena_pages(3, 4096, 1u << 20));                  // буфер глубины, треугольники, ресурсы, стеки помощников
    CHECK(stub_presents >= 20 && stub_presents <= 25);
    CHECK(strstr(stub_log, " frames, ") && strstr(stub_log, " triangles, ") && strstr(stub_log, " px shaded"));
    // аргумент threads=N
    stub_close_at = 8; stub_ev_calls = 0; stub_log[0] = 0; strcpy(stub_args, "threads=1");
    CHECK(umain() == 0 && strstr(stub_log, "1 helper threads") && stub_last_heap_pages == arena_pages(1, 4096, 1u << 20));
    stub_close_at = 8; stub_ev_calls = 0; stub_log[0] = 0; strcpy(stub_args, "threads=0");
    CHECK(umain() == 0 && strstr(stub_log, "0 helper threads"));
    stub_close_at = 8; stub_ev_calls = 0; stub_log[0] = 0; strcpy(stub_args, "threads=99");
    CHECK(umain() == 0 && strstr(stub_log, "8 helper threads"));
    // мало памяти в куче (малая машина): программа берёт меньший профиль и работает
    stub_close_at = 8; stub_ev_calls = 0; stub_log[0] = 0; stub_args[0] = 0; stub_heap_max_pages = arena_pages(3, 2048, 768u << 10);
    CHECK(umain() == 0 && strstr(stub_log, "k3d engine running") && cfg_tris == 2048 && stub_last_heap_pages == stub_heap_max_pages);
    cfg_tris = 4096; cfg_res = 1u << 20; stub_heap_max_pages = 0;
    // замер: для 0 помощников и для N, итог со ускорением
    stub_log[0] = 0; strcpy(stub_args, "bench threads=2");
    CHECK(umain() == 0 && strstr(stub_log, "bench helpers=0:") && strstr(stub_log, "bench helpers=2:") && strstr(stub_log, "bench speedup x") && strstr(stub_log, " ns/px"));
    stub_args[0] = 0;
    // отказ окна и отказ памяти - понятное сообщение и код 1
    stub_gfx_ok = 0; stub_log[0] = 0;
    CHECK(umain() == 1 && strstr(stub_log, "cannot create window"));
    stub_gfx_ok = 1; stub_heap_ok = 0; stub_log[0] = 0;
    CHECK(umain() == 1 && strstr(stub_log, "z-buffer"));
    stub_heap_ok = 1;

    if (fails) { printf("cube3d: %d FAILED\n", fails); return 1; }
    printf("cube3d: self-test, k3d scene (deterministic, identical with and without helper threads, sub-surface, bounds), engine memory from heap, arguments, bench, program loop and failure paths passed\n");
    return 0;
}
