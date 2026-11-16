// Хост-тест движка k3d (userland/k3d.c): эталонные сцены и попиксельные проверки инвариантов.
// Покрытие против независимого перебора, нет дыр и двойного покрытия, глубина (в обоих порядках), перспектива (по лучу), отсечение,
// обход, свет, текстуры (ближайший/билинейный/зажим/мип), прозрачность, поток команд, ресурсы, потоки (бит-в-бит как без потоков).
// Собирается с -fsanitize=address,undefined; потоки - pthread (tests/k3d_host.h).
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "k3d_host.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef struct { k3d_ctx* c; void* mem; k3d_config_t cfg; int W, H, pitch; k3d_u32* buf; k3d_u32* px; } rig_t;
#define GUARD 0xDEADBEEFu
#define GROW 3                                    // охранные строки сверху и снизу
static k3d_u32 words[1 << 16]; static k3d_cmdbuf_t cb;

static rig_t rig_new(int W, int H, int workers, k3d_u32 max_tris, k3d_u32 max_bins) {
    rig_t r; memset(&r, 0, sizeof r);
    k3d_config_default(&r.cfg, W, H, workers); host_sys(&r.cfg.sys); r.cfg.par_min_cost = 0;
    if (max_tris) r.cfg.max_tris = max_tris;
    if (max_bins) r.cfg.max_bins = max_bins;
    r.W = W; r.H = H; r.pitch = W + 5;            // строка шире окна: хвост строки - охрана
    r.c = host_create(&r.cfg, &r.mem);
    r.buf = (k3d_u32*)malloc((size_t)(H + 2 * GROW) * r.pitch * 4);
    r.px = r.buf + (size_t)GROW * r.pitch;
    for (size_t i = 0; i < (size_t)(H + 2 * GROW) * r.pitch; i++) r.buf[i] = GUARD;
    return r;
}
static void rig_free(rig_t* r) { k3d_destroy(r->c); free(r->mem); free(r->buf); }
static int guard_ok(const rig_t* r) {                // ничего не записано за пределы окна
    for (int y = -GROW; y < r->H + GROW; y++) for (int x = 0; x < r->pitch; x++)
        if ((y < 0 || y >= r->H || x >= r->W) && r->buf[(size_t)(y + GROW) * r->pitch + x] != GUARD) return 0;
    return 1;
}
static void rig_fill(rig_t* r, k3d_u32 v) { for (int y = 0; y < r->H; y++) for (int x = 0; x < r->W; x++) r->px[(size_t)y * r->pitch + x] = v; }
static k3d_u32 at(const rig_t* r, int x, int y) { return r->px[(size_t)y * r->pitch + x]; }
static uint32_t hash_rig(const rig_t* r) { uint32_t h = 2166136261u; for (int y = 0; y < r->H; y++) for (int x = 0; x < r->W; x++) { h ^= at(r, x, y); h *= 16777619u; } return h; }

static void begin(void) { k3d_cb_init(&cb, words, sizeof words / 4); }
static int run(rig_t* r) { return k3d_submit(r->c, words, cb.n, r->px, r->pitch); }

static uint32_t rng_s = 12345;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }
static float rndf(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }

// пиксельные координаты (кратные 1/16) -> координаты отсечения при единичных матрицах (w = 1)
static k3d_vertex_t vpx(const rig_t* r, float px, float py, float z, k3d_u32 color) {
    k3d_vertex_t v; memset(&v, 0, sizeof v);
    v.x = px / (float)r->W * 2.0f - 1.0f; v.y = 1.0f - py / (float)r->H * 2.0f; v.z = z; v.nz = -1; v.color = color; return v;
}
static int mk(rig_t* r, const k3d_vertex_t* v, int nv, const k3d_u16* idx, int ni) {
    k3d_res_reset(r->c);
    int a = k3d_vbuf_create(r->c, v, (k3d_u32)nv), b = k3d_ibuf_create(r->c, idx, (k3d_u32)ni);
    return a == 0 && b == 0;
}
static void bind_all(void) { k3d_cb_bind(&cb, K3D_BIND_VERTEX, 0); k3d_cb_bind(&cb, K3D_BIND_INDEX, 0); }

// эталон покрытия: независимый перебор, координаты 28.4, правило "верх-лево"
static int ref_cover(const long long* X, const long long* Y, int px, int py) {
    long long x[3] = { X[0], X[1], X[2] }, y[3] = { Y[0], Y[1], Y[2] };
    long long a2 = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
    if (a2 == 0) return 0;
    if (a2 < 0) { long long t = x[1]; x[1] = x[2]; x[2] = t; t = y[1]; y[1] = y[2]; y[2] = t; }
    long long sx = (long long)px * 16 + 8, sy = (long long)py * 16 + 8;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        long long dx = x[j] - x[i], dy = y[j] - y[i];
        long long e = dx * (sy - y[i]) - dy * (sx - x[i]);
        int tl = (dy < 0) || (dy == 0 && dx > 0);
        if (e < 0 || (e == 0 && !tl)) return 0;
    }
    return 1;
}
static long long q16(float v) { return (long long)floorf(v * 16.0f + 0.5f); }

// ---------------- математика и команды ----------------
static void test_math_and_commands(void) {
    for (float a = -20.0f; a < 20.0f; a += 0.137f) {
        CHECK(fabsf(k3d_sinf(a) - sinf(a)) < 4e-6f && fabsf(k3d_cosf(a) - cosf(a)) < 4e-6f);
    }
    CHECK(k3d_sinf(1e30f) == 0.0f && k3d_sinf(NAN) == 0.0f);
    k3d_mat4 I = k3d_mat4_identity(), t = k3d_mat4_translate(1, 2, 3), s = k3d_mat4_scale(2, 3, 4);
    k3d_mat4 m = k3d_mat4_mul(&t, &s);                      // сначала масштаб, потом сдвиг
    CHECK(m.m[0] == 2 && m.m[5] == 3 && m.m[10] == 4 && m.m[3] == 1 && m.m[7] == 2 && m.m[11] == 3);
    k3d_mat4 mi = k3d_mat4_mul(&m, &I);
    CHECK(memcmp(&mi, &m, sizeof m) == 0);
    k3d_mat4 p = k3d_mat4_perspective(2.0f, 1.5f, 1.0f, 11.0f);   // z=zn -> 0, z=zf -> 1
    float zn = (p.m[10] * 1 + p.m[11]) / (p.m[14] * 1), zf = (p.m[10] * 11 + p.m[11]) / (p.m[14] * 11);
    CHECK(fabsf(zn) < 1e-6f && fabsf(zf - 1.0f) < 1e-6f);
    k3d_mat4 rx = k3d_mat4_rot_x(1.0f), ry = k3d_mat4_rot_y(1.0f), rz = k3d_mat4_rot_z(1.0f);
    CHECK(fabsf(rx.m[5] - cosf(1.0f)) < 1e-5f && fabsf(ry.m[2] - sinf(1.0f)) < 1e-5f && fabsf(rz.m[1] + sinf(1.0f)) < 1e-5f);

    // формат слов команд (не меняется: на него опирается и аппаратный путь)
    k3d_u32 w[64]; k3d_cmdbuf_t c; k3d_cb_init(&c, w, 64);
    k3d_cb_clear(&c, 0x11223344, 0x55667788, 1.0f); k3d_cb_state(&c, K3D_STATE_DEFAULT, 0x80FFFFFF);
    k3d_cb_bind(&c, K3D_BIND_TEXTURE, 2); k3d_cb_bind(&c, K3D_BIND_TEXTURE, -1); k3d_cb_draw(&c, 3, 6, 7);
    CHECK(c.n == 4 + 3 + 3 + 3 + 4 && c.err == 0);
    CHECK(w[0] == ((1u << 16) | 4) && w[1] == 0x11223344 && w[2] == 0x55667788 && w[3] == 0x3F800000u);
    CHECK(w[4] == ((3u << 16) | 3) && w[5] == (K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_DEPTH_WRITE) && w[6] == 0x80FFFFFFu);
    CHECK(w[7] == ((5u << 16) | 3) && w[8] == 2 && w[9] == 2 && w[12] == K3D_NONE);
    CHECK(w[13] == ((6u << 16) | 4) && w[14] == 3 && w[15] == 6 && w[16] == 7);
    CHECK(k3d_cb_validate(w, c.n) == 0 && k3d_cb_validate(w, 0) == 0);
    CHECK(k3d_cb_validate(w, c.n - 1) == K3D_E_BADCMD);          // обрезан
    k3d_u32 bad[8];
    bad[0] = (99u << 16) | 1; CHECK(k3d_cb_validate(bad, 1) == K3D_E_BADCMD);                    // неизвестный код
    bad[0] = (6u << 16) | 3; CHECK(k3d_cb_validate(bad, 3) == K3D_E_BADCMD);                      // неверная длина
    bad[0] = (6u << 16) | 4; bad[1] = 0; bad[2] = 4; bad[3] = 0; CHECK(k3d_cb_validate(bad, 4) == K3D_E_BADCMD);   // индексов не кратно 3
    bad[0] = (5u << 16) | 3; bad[1] = 9; bad[2] = 0; CHECK(k3d_cb_validate(bad, 3) == K3D_E_BADCMD);             // вид привязки
    bad[0] = 0; CHECK(k3d_cb_validate(bad, 1) == K3D_E_BADCMD);                                                   // нулевой код/длина
    CHECK(k3d_cb_validate(0, 4) == K3D_E_BADCMD);
    k3d_mat4 mm = I; k3d_cb_init(&c, w, 20);                     // переполнение: первая ошибка запоминается, дальше записи игнорируются
    CHECK(k3d_cb_matrix(&c, 0, &mm) == 0 && k3d_cb_matrix(&c, 1, &mm) == K3D_E_FULL && c.n == 18 && c.err == K3D_E_FULL);
    CHECK(k3d_cb_state(&c, 0, 0) == K3D_E_FULL && c.n == 18);
    CHECK(k3d_cb_matrix(&c, 3, &mm) == K3D_E_ARG);
}

// ---------------- покрытие: перебор против движка, дыры и двойное покрытие ----------------
static void test_coverage(void) {
    rig_t r = rig_new(100, 70, 0, 0, 0);
    CHECK(r.c != 0);
    // 1. случайные треугольники (в том числе тонкие и крошечные), плоский белый цвет, обе ориентации: маска совпадает с перебором
    int bad = 0, total = 0;
    for (int it = 0; it < 600; it++) {
        float px[3], py[3];
        for (int i = 0; i < 3; i++) { px[i] = floorf(rndf() * 100 * 16) / 16.0f; py[i] = floorf(rndf() * 70 * 16) / 16.0f; }
        if (it % 3 == 0) { px[1] = px[0] + (float)(rnd() % 40) / 16.0f; py[1] = py[0] + (float)(rnd() % 40) / 16.0f; }   // мелкие
        for (int i = 0; i < 3; i++) { px[i] = fminf(fmaxf(px[i], 0.25f), 99.75f); py[i] = fminf(fmaxf(py[i], 0.25f), 69.75f); }   // целиком в окне: отсечение - другой тест
        k3d_vertex_t v[3] = { vpx(&r, px[0], py[0], 0.5f, 0xFFFFFF), vpx(&r, px[1], py[1], 0.5f, 0xFFFFFF), vpx(&r, px[2], py[2], 0.5f, 0xFFFFFF) };
        k3d_u16 idx[3] = { 0, 1, 2 };
        CHECK(mk(&r, v, 3, idx, 3));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
        CHECK(run(&r) == 0);
        long long X[3] = { q16(px[0]), q16(px[1]), q16(px[2]) }, Y[3] = { q16(py[0]), q16(py[1]), q16(py[2]) };
        for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
            int e = ref_cover(X, Y, x, y), g = at(&r, x, y) == 0xFFFFFF;
            CHECK(at(&r, x, y) == 0xFFFFFF || at(&r, x, y) == 0);
            if (e != g) bad++;
            total += e;
        }
    }
    CHECK(bad == 0 && total > 10000);
    CHECK(guard_ok(&r));
    // 2. вееры многоугольника с прозрачностью 50%: каждый пиксель внутри закрашен РОВНО один раз (двойное покрытие дало бы другой цвет), дыр нет
    for (int it = 0; it < 40; it++) {
        int n = 5 + (int)(rnd() % 12);
        k3d_vertex_t v[20]; k3d_u16 idx[60]; int ni = 0;
        float cx = 50 + rndf() * 4, cy = 35 + rndf() * 4, rad = 20 + rndf() * 12, ph = rndf() * 6.28f;
        v[0] = vpx(&r, floorf(cx * 16) / 16, floorf(cy * 16) / 16, 0.5f, 0x80FFFFFF);
        for (int i = 0; i < n; i++) {
            float a = ph + (float)i * 6.2831853f / (float)n;
            v[1 + i] = vpx(&r, floorf((cx + rad * cosf(a)) * 16) / 16, floorf((cy - rad * sinf(a)) * 16) / 16, 0.5f, 0x80FFFFFF);
        }
        for (int i = 0; i < n; i++) { idx[ni++] = 0; idx[ni++] = (k3d_u16)(1 + i); idx[ni++] = (k3d_u16)(1 + (i + 1) % n); }
        CHECK(mk(&r, v, n + 1, idx, ni));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_BLEND | K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, (k3d_u32)ni, 0);
        CHECK(run(&r) == 0);
        int once = 0, other = 0, holes = 0;
        for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
            k3d_u32 p = at(&r, x, y);
            if (p == 0) { // снаружи ли? проверяем по строгой геометрии выпуклого многоугольника (с запасом 1/8 пикселя)
                int strictly_inside = 1;
                for (int i = 0; i < n && strictly_inside; i++) {
                    int j = (i + 1) % n; float ax = (v[1 + i].x + 1) * 0.5f * r.W, ay = (1 - v[1 + i].y) * 0.5f * r.H, bx = (v[1 + j].x + 1) * 0.5f * r.W, by = (1 - v[1 + j].y) * 0.5f * r.H;
                    float e = (bx - ax) * ((float)y + 0.5f - ay) - (by - ay) * ((float)x + 0.5f - ax);
                    float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)); if (e / len < 0.125f) strictly_inside = 0;   // e>0 - внутри при этом обходе
                }
                if (strictly_inside) holes++;
            } else if (p == at(&r, 0, 0) || p == 0x808080 || p == 0x7F7F7F) once++; else other++;
        }
        // цвет "один раз": взять значение из любого закрашенного пикселя, все остальные закрашенные должны совпадать
        k3d_u32 ref = 0; for (int y = 0; y < r.H && !ref; y++) for (int x = 0; x < r.W; x++) if (at(&r, x, y)) { ref = at(&r, x, y); break; }
        int diff = 0; for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) { k3d_u32 p = at(&r, x, y); if (p && p != ref) diff++; }
        CHECK(holes == 0 && diff == 0 && ((ref & 255) == 128 || (ref & 255) == 127));
        (void)once; (void)other;
    }
    // 3. сетка из двух треугольников на общем ребре: общая диагональ не даёт ни дыр, ни двойного покрытия, по всем окнам
    for (int it = 0; it < 30; it++) {
        float x0 = floorf(rndf() * 30 * 16) / 16, y0 = floorf(rndf() * 20 * 16) / 16, x1 = x0 + 20 + (float)(rnd() % 600) / 16.0f, y1 = y0 + 15 + (float)(rnd() % 500) / 16.0f;
        k3d_vertex_t v[4] = { vpx(&r, x0, y0, 0.5f, 0x80FFFFFF), vpx(&r, x1, y0, 0.5f, 0x80FFFFFF), vpx(&r, x1, y1, 0.5f, 0x80FFFFFF), vpx(&r, x0, y1, 0.5f, 0x80FFFFFF) };
        k3d_u16 idx[6] = { 0, 1, 2, 0, 2, 3 }; if (it & 1) { idx[1] = 3; idx[2] = 1; idx[3] = 1; idx[4] = 3; idx[5] = 2; }
        CHECK(mk(&r, v, 4, idx, 6));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_BLEND | K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&r) == 0);
        long long X[3] = { q16(x0), q16(x1), q16(x1) }, Y[3] = { q16(y0), q16(y0), q16(y1) }, X2[3] = { q16(x0), q16(x1), q16(x0) }, Y2[3] = { q16(y0), q16(y1), q16(y1) };
        int wrong = 0;
        for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
            int inside = ref_cover(X, Y, x, y) || ref_cover(X2, Y2, x, y);
            k3d_u32 p = at(&r, x, y);
            if (inside != (p != 0)) wrong++;
            if (p && (p & 255) != 128 && (p & 255) != 127) wrong++;
        }
        CHECK(wrong == 0);
    }
    rig_free(&r);
}

// ---------------- глубина ----------------
static double plane_z(const long long* X, const long long* Y, const float* z, double px, double py) {   // z в точке (px, py) по вершинам
    double ax = X[0] / 16.0, ay = Y[0] / 16.0, bx = X[1] / 16.0, by = Y[1] / 16.0, cx = X[2] / 16.0, cy = Y[2] / 16.0;
    double det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    double l1 = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) / det, l2 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / det;
    return z[0] + l1 * (z[1] - z[0]) + l2 * (z[2] - z[0]);
}
static void test_depth(void) {
    rig_t r = rig_new(96, 64, 0, 0, 0);
    for (int order = 0; order < 2; order++) for (int it = 0; it < 25; it++) {
        float px[6], py[6], z[6];
        for (int i = 0; i < 6; i++) { px[i] = floorf(rndf() * 96 * 16) / 16; py[i] = floorf(rndf() * 64 * 16) / 16; z[i] = 0.05f + rndf() * 0.9f; }
        k3d_vertex_t v[6];
        for (int i = 0; i < 6; i++) v[i] = vpx(&r, px[i], py[i], z[i], i < 3 ? 0xFF2010 : 0x1040FF);
        k3d_u16 idx[6] = { 0, 1, 2, 3, 4, 5 }; if (order) { idx[0] = 3; idx[1] = 4; idx[2] = 5; idx[3] = 0; idx[4] = 1; idx[5] = 2; }
        CHECK(mk(&r, v, 6, idx, 6));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&r) == 0);
        long long XA[3] = { q16(px[0]), q16(px[1]), q16(px[2]) }, YA[3] = { q16(py[0]), q16(py[1]), q16(py[2]) };
        long long XB[3] = { q16(px[3]), q16(px[4]), q16(px[5]) }, YB[3] = { q16(py[3]), q16(py[4]), q16(py[5]) };
        int wrong = 0, both = 0;
        for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
            int a = ref_cover(XA, YA, x, y), b = ref_cover(XB, YB, x, y);
            k3d_u32 p = at(&r, x, y), want = 0;
            if (a && b) {
                double za = plane_z(XA, YA, z, x + 0.5, y + 0.5), zb = plane_z(XB, YB, z + 3, x + 0.5, y + 0.5);
                if (fabs(za - zb) < 2e-3) continue;                   // почти одна глубина: порядок не определён
                want = za < zb ? 0xFF2010 : 0x1040FF; both++;
            } else if (a) want = 0xFF2010; else if (b) want = 0x1040FF;
            if (p != want) wrong++;
        }
        CHECK(wrong == 0);
        (void)both;
    }
    // тест глубины выключен: побеждает последний нарисованный; запись глубины выключена: глубина в буфере не меняется
    k3d_vertex_t v[6] = { vpx(&r, 0, 0, 0.8f, 0xFF0000), vpx(&r, 90, 0, 0.8f, 0xFF0000), vpx(&r, 0, 60, 0.8f, 0xFF0000),
                          vpx(&r, 0, 0, 0.2f, 0x0000FF), vpx(&r, 90, 0, 0.2f, 0x0000FF), vpx(&r, 0, 60, 0.2f, 0x0000FF) };
    k3d_u16 idx[6] = { 3, 4, 5, 0, 1, 2 };                         // сначала ближний синий, потом дальний красный
    CHECK(mk(&r, v, 6, idx, 6));
    begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
    CHECK(run(&r) == 0 && at(&r, 5, 5) == 0xFF0000);
    CHECK(fabsf(k3d_depth(r.c)[5 * r.W + 5] - 1.0f) < 1e-6f);     // без записи глубина осталась очищенной
    begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
    CHECK(run(&r) == 0 && at(&r, 5, 5) == 0x0000FF && fabsf(k3d_depth(r.c)[5 * r.W + 5] - 0.2f) < 1e-3f);
    // без CLEAR следующий вызов рисует поверх с прежней глубиной: дальний красный не перекроет ближний синий
    begin(); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 3, 3, 0);
    CHECK(run(&r) == 0 && at(&r, 5, 5) == 0x0000FF);
    CHECK(guard_ok(&r));
    rig_free(&r);
}

// ---------------- перспектива: цвет и текстурные координаты по лучу ----------------
static void test_perspective(void) {
    rig_t r = rig_new(160, 120, 0, 0, 0);
    const float F = 1.7320508f, asp = 160.0f / 120.0f;
    k3d_mat4 proj = k3d_mat4_perspective(F, asp, 0.5f, 40.0f);
    k3d_u32* tex = (k3d_u32*)malloc(256 * 256 * 4);
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) tex[y * 256 + x] = ((k3d_u32)x << 16) | ((k3d_u32)y << 8);
    CHECK(k3d_tex_create(r.c, 256, 256, tex, 0) == 0);
    // пол y = -1: x -3..3, z 2..14; u = (x+3)/6, v = (z-2)/12
    k3d_vertex_t v[4]; memset(v, 0, sizeof v);
    float gx[4] = { -3, 3, 3, -3 }, gz[4] = { 2, 2, 14, 14 };
    for (int i = 0; i < 4; i++) { v[i].x = gx[i]; v[i].y = -1; v[i].z = gz[i]; v[i].ny = 1; v[i].u = (gx[i] + 3) / 6; v[i].v = (gz[i] - 2) / 12; v[i].color = 0xFFFFFFFF; }
    k3d_u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
    CHECK(k3d_vbuf_create(r.c, v, 4) == 0 && k3d_ibuf_create(r.c, idx, 6) == 0);
    begin(); k3d_cb_clear(&cb, 0x101010, 0x101010, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj);
    k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_TEXTURE, 0xFFFFFFFF);
    bind_all(); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0); k3d_cb_draw(&cb, 0, 6, 0);
    CHECK(run(&r) == 0);
    int checked = 0, wrong = 0;
    for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
        double e_r[5], e_g[5]; int ok = 1, n = 0;
        const double offs[5][2] = { {0.5, 0.5}, {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0} };
        for (int k = 0; k < 5; k++) {
            double nx = (x + offs[k][0]) / r.W * 2 - 1, ny = 1 - (y + offs[k][1]) / r.H * 2;
            if (ny > -1e-3) { ok = 0; break; }
            double z = -F / ny, px = nx * asp / F * z, u = (px + 3) / 6, vv = (z - 2) / 12;
            if (z < 2.3 || z > 13.5 || fabs(px) > 2.8) { ok = 0; break; }
            e_r[n] = floor(u * 256); e_g[n] = floor(vv * 256); n++;
        }
        if (!ok) continue;
        double rmin = e_r[0], rmax = e_r[0], gmin = e_g[0], gmax = e_g[0];
        for (int k = 1; k < n; k++) { if (e_r[k] < rmin) rmin = e_r[k]; if (e_r[k] > rmax) rmax = e_r[k]; if (e_g[k] < gmin) gmin = e_g[k]; if (e_g[k] > gmax) gmax = e_g[k]; }
        if (rmax - rmin > 6 || gmax - gmin > 6) continue;                       // у горизонта шаг слишком крупный: проверим только там, где он мал
        k3d_u32 p = at(&r, x, y); int pr = (int)(p >> 16 & 255), pg = (int)(p >> 8 & 255);
        checked++;
        if (pr < rmin - 1.5 || pr > rmax + 1.5 || pg < gmin - 1.5 || pg > gmax + 1.5) wrong++;
    }
    CHECK(checked > 1500 && wrong == 0);
    // цвет: вершины красный/зелёный на разной глубине - в середине экрана значение по перспективно-верной формуле
    memset(v, 0, sizeof v);
    for (int i = 0; i < 4; i++) { v[i].x = (i == 1 || i == 2) ? 1.0f : -1.0f; v[i].y = (i < 2) ? -1.0f : 1.0f; v[i].z = (i == 0 || i == 1) ? 2.0f : 12.0f; v[i].color = (i < 2) ? 0xFF000000u | 0x0000FF : 0xFF000000u | 0xFF0000; }
    k3d_res_reset(r.c); CHECK(k3d_vbuf_create(r.c, v, 4) == 0 && k3d_ibuf_create(r.c, idx, 6) == 0);
    begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
    CHECK(run(&r) == 0);
    // стена x = ±1, y = ±1: низ (z=2) синий, верх (z=12) красный; строка экрана -> y/z = ny/F -> точка на стене; доля красного = (t по глубине)
    int cwrong = 0, cchecked = 0;
    for (int y = 10; y < r.H - 10; y++) {
        double ny = 1 - (y + 0.5) / r.H * 2;               // y_view / z = ny / F; стена y_view = -1 + 2*s, z = 2 + 10 * s  (s - доля вдоль ребра)
        // -1 + 2s = ny/F * (2 + 10 s) -> s (2 - 10 ny/F) = 1 + 2 ny/F
        double k = ny / F, s = (1 + 2 * k) / (2 - 10 * k);
        if (s < 0.05 || s > 0.95) continue;
        int x = r.W / 2; k3d_u32 p = at(&r, x, y);
        // на вертикальной оси x_view = 0: красная доля = s, синяя = 1 - s (вдоль ребра цвет меняется линейно по s в мировом пространстве)
        int er = (int)floor(s * 255 + 0.5), eb = (int)floor((1 - s) * 255 + 0.5);
        cchecked++;
        if (abs((int)(p >> 16 & 255) - er) > 3 || abs((int)(p & 255) - eb) > 3) cwrong++;
    }
    CHECK(cchecked > 30 && cwrong == 0);
    free(tex);
    rig_free(&r);
}

// ---------------- отсечение ----------------
static int count_nonbg(const rig_t* r, k3d_u32 bg) { int n = 0; for (int y = 0; y < r->H; y++) for (int x = 0; x < r->W; x++) if (at(r, x, y) != bg) n++; return n; }
static void test_clipping(void) {
    rig_t r = rig_new(128, 96, 0, 0, 0);
    const float F = 1.7320508f, asp = 128.0f / 96.0f; const float ZN = 0.5f, ZF = 30.0f;
    k3d_mat4 proj = k3d_mat4_perspective(F, asp, ZN, ZF);
    // 1. пол от z = -8 (позади камеры) до z = 50 (за дальней плоскостью): виден только кусок между ближней и дальней плоскостями.
    //    Высота пола -1: проверяется дальняя плоскость; высота -0.1: нижние строки экрана смотрят на пол ближе ближней плоскости - они должны остаться пустыми
    for (int hk = 0; hk < 2; hk++) {
        const float yg = hk ? -0.1f : -1.0f;
        k3d_vertex_t v[4]; memset(v, 0, sizeof v);
        float gx[4] = { -4, 4, 4, -4 }, gz[4] = { -8, -8, 50, 50 };
        for (int i = 0; i < 4; i++) { v[i].x = gx[i]; v[i].y = yg; v[i].z = gz[i]; v[i].color = 0xFFFFFFFF; }
        k3d_u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
        CHECK(mk(&r, v, 4, idx, 6));
        begin(); k3d_cb_clear(&cb, 0x202020, 0x202020, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj);
        k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&r) == 0 && k3d_stats(r.c)->tris_clipped == 2);
        int wrong = 0, inside = 0, near_empty = 0;
        for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) {
            double nx = (x + 0.5) / r.W * 2 - 1, ny = 1 - (y + 0.5) / r.H * 2;
            int drawn = at(&r, x, y) == 0xFFFFFF;
            if (ny > -1e-4) { if (drawn) wrong++; continue; }               // выше горизонта пол не виден
            double z = yg * F / ny, px = nx * asp / F * z;
            // у пологого ребра сдвиг вершины на 1/32 пикселя (сетка 28.4, как у аппаратных растеризаторов) смещает границу на пару пикселей: проверяем с запасом 2.5 пикселя
            double pw = z * 2 * asp / F / r.W;
            int in = (z > ZN * 1.05 && z < ZF * 0.95 && fabs(px) < 4 - 2.5 * pw), out = (z < ZN * 0.95 || z > ZF * 1.05 || fabs(px) > 4 + 2.5 * pw);
            if (in) { inside++; if (!drawn) wrong++; }
            if (out && drawn) wrong++;
            if (z < ZN * 0.95) near_empty++;
        }
        CHECK(wrong == 0 && inside > 800 && (hk == 0 || near_empty > 500));
    }
    // 2. треугольник за камерой и целиком вне пирамиды (слева, справа, сверху, снизу, дальше дальней): ничего, статистика "вне"
    float cases[6][9] = { {-1,0,-3, 1,0,-3, 0,1,-4}, {-30,-1,5, -20,-1,5, -25,2,8}, {30,-1,5, 20,-1,5, 25,2,8}, {-1,40,5, 1,40,5, 0,50,8}, {-1,-40,5, 1,-40,5, 0,-50,8}, {-1,0,60, 1,0,60, 0,1,70} };
    for (int k = 0; k < 6; k++) {
        k3d_vertex_t t[3]; memset(t, 0, sizeof t);
        for (int i = 0; i < 3; i++) { t[i].x = cases[k][i * 3]; t[i].y = cases[k][i * 3 + 1]; t[i].z = cases[k][i * 3 + 2]; t[i].color = 0xFFFFFFFF; }
        k3d_u16 ti[3] = { 0, 1, 2 }; CHECK(mk(&r, t, 3, ti, 3));
        begin(); k3d_cb_clear(&cb, 0x202020, 0x202020, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_state(&cb, K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
        CHECK(run(&r) == 0 && count_nonbg(&r, 0x202020) == 0 && k3d_stats(r.c)->tris_outside == 1 && k3d_stats(r.c)->tris_drawn == 0);
    }
    // 3. огромный треугольник, накрывающий весь экран, пересекающий все боковые плоскости и ближнюю: покрывает каждый пиксель ровно один раз
    k3d_vertex_t big[3]; memset(big, 0, sizeof big);
    float bp[9] = { -500, -300, 3, 500, -300, 3, 0, 700, 20 };
    for (int i = 0; i < 3; i++) { big[i].x = bp[i * 3]; big[i].y = bp[i * 3 + 1]; big[i].z = bp[i * 3 + 2]; big[i].color = 0x80FFFFFF; }
    k3d_u16 bi[3] = { 0, 1, 2 }; CHECK(mk(&r, big, 3, bi, 3));
    begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_BLEND, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
    CHECK(run(&r) == 0);
    int holes = 0, dbl = 0; for (int y = 0; y < r.H; y++) for (int x = 0; x < r.W; x++) { k3d_u32 p = at(&r, x, y) & 255; if (p == 0) holes++; else if (p != 128 && p != 127) dbl++; }
    CHECK(holes == 0 && dbl == 0 && guard_ok(&r));
    // 4. треугольник, перешедший ближнюю плоскость: ломаный на куски, куски стыкуются без щелей (проверка: нет дыр на полу от z = 0.1)
    // (уже проверено в п.1: пол от z = -8; здесь - вершины с w <= 0, NaN и бесконечность: не падает, ничего не пишет за границы)
    float evil[4][3] = { {NAN, 0, 5}, {INFINITY, 1, 5}, {0, -INFINITY, 5}, {1e30f, 1e30f, 1e30f} };
    for (int k = 0; k < 4; k++) {
        k3d_vertex_t t[3]; memset(t, 0, sizeof t);
        for (int i = 0; i < 3; i++) { t[i].x = (float)i - 1; t[i].y = (float)(i & 1); t[i].z = 5; t[i].color = 0xFFFFFFFF; }
        t[1].x = evil[k][0]; t[1].y = evil[k][1]; t[1].z = evil[k][2];
        k3d_u16 ti[3] = { 0, 1, 2 }; CHECK(mk(&r, t, 3, ti, 3));
        begin(); k3d_cb_clear(&cb, 0x202020, 0x202020, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_state(&cb, K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
        CHECK(run(&r) == 0 && guard_ok(&r));
    }
    // 5. обход и отбраковка
    k3d_vertex_t q[3]; memset(q, 0, sizeof q);
    float ccw[9] = { -1, -1, 5, 1, -1, 5, 0, 1, 5 };                      // с камеры (y вверх): против часовой - лицевой
    for (int i = 0; i < 3; i++) { q[i].x = ccw[i * 3]; q[i].y = ccw[i * 3 + 1]; q[i].z = ccw[i * 3 + 2]; q[i].color = 0xFFFFFFFF; }
    k3d_u16 fi[3] = { 0, 1, 2 }, bk[3] = { 0, 2, 1 };
    struct { k3d_u16* i; k3d_u32 cull; int drawn; } cc[] = { {fi, K3D_CULL_BACK, 1}, {bk, K3D_CULL_BACK, 0}, {fi, K3D_CULL_FRONT, 0}, {bk, K3D_CULL_FRONT, 1}, {fi, K3D_CULL_NONE, 1}, {bk, K3D_CULL_NONE, 1} };
    for (int k = 0; k < 6; k++) {
        CHECK(mk(&r, q, 3, cc[k].i, 3));
        begin(); k3d_cb_clear(&cb, 0x202020, 0x202020, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_state(&cb, cc[k].cull, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
        CHECK(run(&r) == 0 && (count_nonbg(&r, 0x202020) > 100) == cc[k].drawn && (k3d_stats(r.c)->tris_culled == (cc[k].drawn ? 0u : 1u)));
    }
    rig_free(&r);
}

// ---------------- свет Ламберта ----------------
static void test_lighting(void) {
    rig_t r = rig_new(64, 48, 0, 0, 0);
    // квадрат на весь экран, нормаль n, белый цвет вершин; свет к (0,0,-1) (свет со стороны зрителя: в модели нормаль (0,0,-1) освещена полностью)
    float L[3] = { 0, 0, -1 }, LC[3] = { 0.6f, 0.5f, 0.4f }, LA[3] = { 0.2f, 0.25f, 0.3f };
    struct { float n[3]; float ndl; } nt[] = { { {0, 0, -1}, 1.0f }, { {0.6f, 0, -0.8f}, 0.8f }, { {0, 0, 1}, 0.0f }, { {0, 1, 0}, 0.0f } };
    for (int mode = 0; mode < 2; mode++) for (int k = 0; k < 4; k++) {
        k3d_vertex_t v[4]; k3d_u16 idx[6] = { 0, 2, 1, 0, 3, 2 };
        for (int i = 0; i < 4; i++) { v[i] = vpx(&r, i == 1 || i == 2 ? 64.0f : 0.0f, i >= 2 ? 48.0f : 0.0f, 0.5f, 0xFFFFFFFF); v[i].nx = nt[k].n[0]; v[i].ny = nt[k].n[1]; v[i].nz = nt[k].n[2]; }
        CHECK(mk(&r, v, 4, idx, 6));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_light(&cb, L, LC, LA); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | (mode ? K3D_LIGHT_PIXEL : K3D_LIGHT_VERTEX), 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&r) == 0);
        k3d_u32 p = at(&r, 30, 20);
        float e[3] = { LA[0] + LC[0] * nt[k].ndl, LA[1] + LC[1] * nt[k].ndl, LA[2] + LC[2] * nt[k].ndl };
        for (int ch = 0; ch < 3; ch++) CHECK(abs((int)(p >> (16 - 8 * ch) & 255) - (int)(fminf(e[ch], 1.0f) * 255 + 0.5f)) <= 1);
    }
    // свет не задан: цвет вершин без изменений (ambient 1). Цвет вершины и tint перемножаются
    { k3d_vertex_t v[3] = { vpx(&r, 0, 0, 0.5f, 0xFF80C040), vpx(&r, 64, 0, 0.5f, 0xFF80C040), vpx(&r, 0, 48, 0.5f, 0xFF80C040) }; k3d_u16 idx[3] = { 0, 2, 1 };
      CHECK(mk(&r, v, 3, idx, 3));
      begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE, 0xFF808080); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
      CHECK(run(&r) == 0);
      k3d_u32 p = at(&r, 5, 5);
      CHECK(abs((int)(p >> 16 & 255) - 64) <= 1 && abs((int)(p >> 8 & 255) - 96) <= 1 && abs((int)(p & 255) - 32) <= 1); }
    // нормали меняются по треугольнику: по пикселям = нормировка интерполированной нормали, по вершинам = интерполяция цвета; режимы различаются
    { k3d_vertex_t v[3] = { vpx(&r, 0, 0, 0.5f, 0xFFFFFFFF), vpx(&r, 64, 0, 0.5f, 0xFFFFFFFF), vpx(&r, 0, 48, 0.5f, 0xFFFFFFFF) };
      float nn[3][3] = { {0, 0, -1}, {0.9f, 0, 0.436f}, {0, 0.9f, 0.436f} };
      for (int i = 0; i < 3; i++) { float l = sqrtf(nn[i][0] * nn[i][0] + nn[i][1] * nn[i][1] + nn[i][2] * nn[i][2]); v[i].nx = nn[i][0] / l; v[i].ny = nn[i][1] / l; v[i].nz = nn[i][2] / l; }
      k3d_u16 idx[3] = { 0, 2, 1 }; CHECK(mk(&r, v, 3, idx, 3));
      float la[3] = { 0.1f, 0.1f, 0.1f }, lc[3] = { 0.9f, 0.9f, 0.9f };
      k3d_u32 img[2][64 * 48]; int maxdiff = 0, wrong[2] = {0, 0};
      for (int mode = 0; mode < 2; mode++) {
          begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_light(&cb, L, lc, la); k3d_cb_state(&cb, K3D_CULL_NONE | (mode ? K3D_LIGHT_PIXEL : K3D_LIGHT_VERTEX), 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
          CHECK(run(&r) == 0);
          for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) img[mode][y * 64 + x] = at(&r, x, y);
      }
      for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) {
          double l1 = (x + 0.5) / 64.0, l2 = (y + 0.5) / 48.0, l0 = 1 - l1 - l2;
          if (l0 < 0.03 || l1 < 0.03 || l2 < 0.03) continue;                // от границ треугольника отступаем
          double ix = l0 * v[0].nx + l1 * v[1].nx + l2 * v[2].nx, iy = l0 * v[0].ny + l1 * v[1].ny + l2 * v[2].ny, iz = l0 * v[0].nz + l1 * v[1].nz + l2 * v[2].nz;
          double len = sqrt(ix * ix + iy * iy + iz * iz), ndl = -iz / len; if (ndl < 0) ndl = 0;
          int ep = (int)((0.1 + 0.9 * ndl) * 255 + 0.5);
          double vl = 0; for (int i = 0; i < 3; i++) { double nd = -v[i].nz; if (nd < 0) nd = 0; vl += (i == 0 ? l0 : (i == 1 ? l1 : l2)) * (0.1 + 0.9 * nd); }
          int ev = (int)(vl * 255 + 0.5);
          int gp = (int)(img[1][y * 64 + x] & 255), gv = (int)(img[0][y * 64 + x] & 255);
          if (abs(gp - ep) > 2) wrong[1]++;
          if (abs(gv - ev) > 2) wrong[0]++;
          if (abs(gp - gv) > maxdiff) maxdiff = abs(gp - gv);
      }
      CHECK(wrong[0] == 0 && wrong[1] == 0 && maxdiff >= 8); }
    // матрица нормалей: поворот модели на 90 градусов вокруг y переводит нормаль (0,0,-1) в (-1,0,0) (или +1): свет по x даёт полный цвет; масштаб не по осям - обратная транспонированная
    { k3d_vertex_t v[3] = { vpx(&r, 0, 0, 0.5f, 0xFFFFFFFF), vpx(&r, 64, 0, 0.5f, 0xFFFFFFFF), vpx(&r, 0, 48, 0.5f, 0xFFFFFFFF) };
      for (int i = 0; i < 3; i++) { v[i].nx = 0; v[i].ny = 0; v[i].nz = -1; }
      k3d_u16 idx[3] = { 0, 2, 1 }; CHECK(mk(&r, v, 3, idx, 3));
      float la[3] = { 0, 0, 0 }, lc[3] = { 1, 1, 1 }, lx[3] = { -1, 0, 0 }, lxp[3] = { 1, 0, 0 };
      k3d_mat4 rot = k3d_mat4_rot_y(1.5707963f), unrot = k3d_mat4_rot_y(-1.5707963f);   // вид отменяет поворот в геометрии: поворачиваются только нормали
      int full = 0;
      for (int s = 0; s < 2; s++) {
          begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_MODEL, &rot); k3d_cb_matrix(&cb, K3D_SLOT_VIEW, &unrot); k3d_cb_light(&cb, s ? lxp : lx, lc, la); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_LIGHT_VERTEX, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
          CHECK(run(&r) == 0);
          // вершины тоже повёрнуты и могли уйти из экрана; сравниваем только свет: ищем любой пиксель
          for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) if ((at(&r, x, y) & 255) > 250) full |= 1 << s;
      }
        CHECK(full == 1);                              // свет с -x даёт полный цвет (нормаль повёрнута в -x), с +x - темно
      // неравномерный масштаб: плоскость x + y = 0 (нормаль (1,1,0)/sqrt2) при масштабе (1,3,1) имеет нормаль (3,1,0)/sqrt10
      for (int i = 0; i < 3; i++) { v[i].nx = 0.70710678f; v[i].ny = 0.70710678f; v[i].nz = 0; }
      CHECK(mk(&r, v, 3, idx, 3));
      k3d_mat4 sc = k3d_mat4_scale(1, 3, 1), usc = k3d_mat4_scale(1, 1.0f / 3.0f, 1);
      float ld[3] = { 0.9486833f, 0.3162278f, 0 };    // (3,1,0)/sqrt10: должен получиться полный свет; если бы нормаль масштабировалась как точка (1,3,0) - свет 0.7 и ниже
      begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_MODEL, &sc); k3d_cb_matrix(&cb, K3D_SLOT_VIEW, &usc); k3d_cb_light(&cb, ld, lc, la); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_LIGHT_VERTEX, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 3, 0);
      CHECK(run(&r) == 0);
      int found = 0; for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) if ((at(&r, x, y) & 255) >= 253) found++;
      CHECK(found > 100); }
    rig_free(&r);
}

// ---------------- текстуры ----------------
static void test_textures(void) {
    rig_t r = rig_new(64, 64, 0, 0, 0);
    // экранный квадрат: uv линейны по экрану (w = 1), эталон считается независимо
    struct { float u0, u1; } uvr[3] = { {0, 1}, {0, 3}, {-1, 2} };
    k3d_u32 t4[16]; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) t4[y * 4 + x] = ((k3d_u32)(x * 60 + 10) << 16) | ((k3d_u32)(y * 60 + 20) << 8) | ((k3d_u32)((x ^ y) & 1) * 200 + 30);
    CHECK(k3d_tex_create(r.c, 4, 4, t4, 0) == 0);
    for (int rep = 0; rep < 3; rep++) for (int bil = 0; bil < 2; bil++) for (int clampw = 0; clampw < 2; clampw++) {
        k3d_vertex_t v[4]; k3d_u16 idx[6] = { 0, 2, 1, 0, 3, 2 };
        for (int i = 0; i < 4; i++) { v[i] = vpx(&r, i == 1 || i == 2 ? 64.0f : 0.0f, i >= 2 ? 64.0f : 0.0f, 0.5f, 0xFFFFFFFF); v[i].u = (i == 1 || i == 2) ? uvr[rep].u1 : uvr[rep].u0; v[i].v = i >= 2 ? uvr[rep].u1 : uvr[rep].u0; }
        k3d_res_reset(r.c); CHECK(k3d_tex_create(r.c, 4, 4, t4, 0) == 0 && k3d_vbuf_create(r.c, v, 4) == 0 && k3d_ibuf_create(r.c, idx, 6) == 0);
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_TEXTURE | (bil ? K3D_FILTER_BILINEAR : 0) | (clampw ? K3D_WRAP_CLAMP : 0), 0xFFFFFFFF);
        bind_all(); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&r) == 0);
        int wrong = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            double u = uvr[rep].u0 + (uvr[rep].u1 - uvr[rep].u0) * (x + 0.5) / 64, vv = uvr[rep].u0 + (uvr[rep].u1 - uvr[rep].u0) * (y + 0.5) / 64;
            double e[3];
            if (!bil) {
                int ix = (int)floor(u * 4), iy = (int)floor(vv * 4);
                if (clampw) { ix = ix < 0 ? 0 : (ix > 3 ? 3 : ix); iy = iy < 0 ? 0 : (iy > 3 ? 3 : iy); } else { ix &= 3; iy &= 3; }
                for (int c = 0; c < 3; c++) e[c] = (double)(t4[iy * 4 + ix] >> (16 - 8 * c) & 255);
            } else {
                double fu = u * 4 - 0.5, fv = vv * 4 - 0.5; int iu = (int)floor(fu), iv = (int)floor(fv); double a = fu - iu, b = fv - iv;
                int xs[2] = { iu, iu + 1 }, ys[2] = { iv, iv + 1 };
                for (int k = 0; k < 2; k++) { if (clampw) { xs[k] = xs[k] < 0 ? 0 : (xs[k] > 3 ? 3 : xs[k]); ys[k] = ys[k] < 0 ? 0 : (ys[k] > 3 ? 3 : ys[k]); } else { xs[k] &= 3; ys[k] &= 3; } }
                for (int c = 0; c < 3; c++) {
                    int sh = 16 - 8 * c; double t00 = t4[ys[0] * 4 + xs[0]] >> sh & 255, t10 = t4[ys[0] * 4 + xs[1]] >> sh & 255, t01 = t4[ys[1] * 4 + xs[0]] >> sh & 255, t11 = t4[ys[1] * 4 + xs[1]] >> sh & 255;
                    e[c] = (t00 * (1 - a) + t10 * a) * (1 - b) + (t01 * (1 - a) + t11 * a) * b;
                }
            }
            k3d_u32 p = at(&r, x, y);
            // у границ текселей float и ближайшая выборка могут разойтись на один тексель: для ближайшей проверяем, что цвет - один из соседних
            double tol = bil ? 2.5 : 0.5;
            for (int c = 0; c < 3; c++) {
                double g = (double)(p >> (16 - 8 * c) & 255);
                if (fabs(g - e[c]) > tol) {
                    if (!bil) { double fx = u * 4 - floor(u * 4), fy = vv * 4 - floor(vv * 4); if (fx < 0.02 || fx > 0.98 || fy < 0.02 || fy > 0.98) continue; }
                    wrong++;
                }
            }
        }
        CHECK(wrong == 0);
    }
    // мип-уровни: шахматка 1x1 на 64x64, показанная в 8x8 пикселей, даёт средний серый (уровень 3); без мипов - чёрные и белые пиксели
    { k3d_u32 chk[64 * 64]; for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) chk[y * 64 + x] = ((x ^ y) & 1) ? 0xFFFFFF : 0x000000;
      for (int mips = 0; mips < 2; mips++) {
          k3d_vertex_t v[4]; k3d_u16 idx[6] = { 0, 2, 1, 0, 3, 2 };
          for (int i = 0; i < 4; i++) { v[i] = vpx(&r, i == 1 || i == 2 ? 40.0f : 32.0f, i >= 2 ? 40.0f : 32.0f, 0.5f, 0xFFFFFFFF); v[i].u = (i == 1 || i == 2) ? 1.0f : 0.0f; v[i].v = i >= 2 ? 1.0f : 0.0f; }
          k3d_res_reset(r.c); CHECK(k3d_tex_create(r.c, 64, 64, chk, mips) == 0 && k3d_vbuf_create(r.c, v, 4) == 0 && k3d_ibuf_create(r.c, idx, 6) == 0);
          begin(); k3d_cb_clear(&cb, 0x404040, 0x404040, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_TEXTURE, 0xFFFFFFFF); bind_all(); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0); k3d_cb_draw(&cb, 0, 6, 0);
          CHECK(run(&r) == 0);
          int gray = 0, ext = 0; for (int y = 32; y < 40; y++) for (int x = 32; x < 40; x++) { int g = (int)(at(&r, x, y) & 255); if (g >= 126 && g <= 129) gray++; if (g <= 20 || g >= 235) ext++; }
          if (mips) CHECK(gray == 64 && ext == 0); else CHECK(ext > 8);
      } }
    // 1x1 и неквадратные текстуры, неверные размеры
    { k3d_u32 one = 0xFF336699; CHECK(k3d_tex_create(r.c, 1, 1, &one, 1) >= 0); k3d_u32 d[3 * 4] = {0};
      CHECK(k3d_tex_create(r.c, 3, 4, d, 0) == K3D_E_ARG && k3d_tex_create(r.c, 2048, 2, d, 0) == K3D_E_ARG && k3d_tex_create(r.c, 4, 4, 0, 0) == K3D_E_ARG); }
    CHECK(guard_ok(&r));
    rig_free(&r);
}

// ---------------- прозрачность ----------------
static void test_alpha(void) {
    rig_t r = rig_new(64, 64, 0, 0, 0);
    // значения: цвет * альфа поверх фона (градиент), формула с округлением /255
    { k3d_vertex_t v[4]; k3d_u16 idx[6] = { 0, 2, 1, 0, 3, 2 };
      for (int i = 0; i < 4; i++) v[i] = vpx(&r, i == 1 || i == 2 ? 64.0f : 0.0f, i >= 2 ? 64.0f : 0.0f, 0.5f, 0xFFFF2010);
      k3d_u32 tints[3] = { 0xFFFFFFFF, 0x80FFFFFF, 0x40FFFFFF };
      for (int k = 0; k < 3; k++) {
          CHECK(mk(&r, v, 4, idx, 6));
          begin(); k3d_cb_clear(&cb, 0x204060, 0xA0B0C0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_BLEND, tints[k]); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
          CHECK(run(&r) == 0);
          int wrong = 0;
          for (int y = 0; y < 64; y++) {
              unsigned f = (unsigned)y * 256 / 63, bg[3]; for (int s = 0; s < 3; s++) { unsigned a = (0x204060 >> (16 - 8 * s)) & 255, b = (0xA0B0C0 >> (16 - 8 * s)) & 255; bg[s] = ((a * (256 - f) + b * f) >> 8) & 255; }
              unsigned al = tints[k] >> 24; unsigned src[3] = { 255, 32, 16 };
              for (int x = 0; x < 64; x++) for (int s = 0; s < 3; s++) {
                  unsigned e = (src[s] * al + bg[s] * (255 - al) + 127) / 255, g = (at(&r, x, y) >> (16 - 8 * s)) & 255;
                  if ((int)g - (int)e > 1 || (int)e - (int)g > 1) wrong++;
              }
          }
          CHECK(wrong == 0);
      } }
    // порядок: непрозрачные треугольники плитки рисуются раньше прозрачных, как бы их ни подали; прозрачное за непрозрачным скрыто глубиной
    { k3d_vertex_t v[8]; k3d_u16 idx[12] = { 0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6 };
      for (int i = 0; i < 4; i++) { v[i] = vpx(&r, i == 1 || i == 2 ? 48.0f : 8.0f, i >= 2 ? 48.0f : 8.0f, 0.3f, 0x80FF0000); v[4 + i] = vpx(&r, i == 1 || i == 2 ? 56.0f : 16.0f, i >= 2 ? 56.0f : 16.0f, 0.6f, 0xFF0000FF); }
      for (int order = 0; order < 2; order++) {
          if (order) { for (int k = 0; k < 6; k++) { k3d_u16 t = idx[k]; idx[k] = idx[6 + k]; idx[6 + k] = t; } }
          CHECK(mk(&r, v, 8, idx, 12));
          begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_BLEND, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
          // два отдельных DRAW разных состояний: прозрачный ближний и непрозрачный дальний
          begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); bind_all();
          k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_BLEND, 0xFFFFFFFF); k3d_cb_draw(&cb, order ? 6 : 0, 6, 0);
          k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); k3d_cb_draw(&cb, order ? 0 : 6, 6, 0);
          CHECK(run(&r) == 0);
          // в области пересечения (20..48) - синий снизу и красный полупрозрачный сверху: (128, 0, 127)
          k3d_u32 p = at(&r, 30, 30); CHECK(abs((int)(p >> 16 & 255) - 128) <= 1 && (int)(p >> 8 & 255) == 0 && abs((int)(p & 255) - 127) <= 1);
          CHECK(at(&r, 10, 10) != 0 && (at(&r, 10, 10) >> 16 & 255) >= 126 && (at(&r, 10, 10) & 255) == 0);     // красный на чёрном
          CHECK(at(&r, 54, 54) == 0x0000FF);
      }
      // прозрачное ДАЛЬШЕ непрозрачного: глубина прячет его
      for (int i = 0; i < 4; i++) { v[i].z = 0.8f; }
      k3d_u16 i2[12] = { 0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6 };
      CHECK(mk(&r, v, 8, i2, 12));
      begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); bind_all();
      k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_BLEND, 0xFFFFFFFF); k3d_cb_draw(&cb, 0, 6, 0);
      k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF); k3d_cb_draw(&cb, 6, 6, 0);
      CHECK(run(&r) == 0 && at(&r, 30, 30) == 0x0000FF); }
    // текстура с альфой: альфа из текселя перемножается
    { k3d_u32 t2[4] = { 0xFFFF0000, 0x00FF0000, 0x80FF0000, 0xFFFF0000 }; k3d_vertex_t v[4]; k3d_u16 idx[6] = { 0, 2, 1, 0, 3, 2 };
      for (int i = 0; i < 4; i++) { v[i] = vpx(&r, i == 1 || i == 2 ? 64.0f : 0.0f, i >= 2 ? 64.0f : 0.0f, 0.5f, 0xFFFFFFFF); v[i].u = (i == 1 || i == 2) ? 1.0f : 0.0f; v[i].v = i >= 2 ? 1.0f : 0.0f; }
      k3d_res_reset(r.c); CHECK(k3d_tex_create(r.c, 2, 2, t2, 0) == 0 && k3d_vbuf_create(r.c, v, 4) == 0 && k3d_ibuf_create(r.c, idx, 6) == 0);
      begin(); k3d_cb_clear(&cb, 0x000020, 0x000020, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE | K3D_TEXTURE | K3D_BLEND, 0xFFFFFFFF); bind_all(); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0); k3d_cb_draw(&cb, 0, 6, 0);
      CHECK(run(&r) == 0 && at(&r, 10, 10) == 0xFF0000 && at(&r, 50, 10) == 0x000020 && abs((int)(at(&r, 10, 50) >> 16 & 255) - 128) <= 1 && (at(&r, 10, 50) & 255) >= 15 && (at(&r, 10, 50) & 255) <= 17); }
    CHECK(guard_ok(&r));
    rig_free(&r);
}

// ---------------- сцена для потоков, ёмкости и ресурсов ----------------
#define NSV 700
static k3d_vertex_t sv[NSV]; static k3d_u16 si[NSV];
static void scene_mesh(rig_t* r) {                               // облако случайных треугольников в пространстве вида (камера в нуле)
    for (int t = 0; t < NSV / 3; t++) {
        float cx = (rndf() - 0.5f) * 14, cy = (rndf() - 0.5f) * 9, cz = 3 + rndf() * 22, sz = 0.3f + rndf() * 2.5f;
        k3d_u32 col = 0xFF000000u | (rnd() & 0xFFFFFF);
        for (int k = 0; k < 3; k++) {
            k3d_vertex_t* v = &sv[t * 3 + k]; memset(v, 0, sizeof *v);
            v->x = cx + (rndf() - 0.5f) * sz * 2; v->y = cy + (rndf() - 0.5f) * sz * 2; v->z = cz + (rndf() - 0.5f) * sz;
            v->nx = rndf() - 0.5f; v->ny = rndf() - 0.5f; v->nz = -0.2f - rndf(); v->u = rndf() * 3 - 1; v->v = rndf() * 3 - 1;
            v->color = (k == 0 || (t & 3)) ? col : (0xFF000000u | (rnd() & 0xFFFFFF)); si[t * 3 + k] = (k3d_u16)(t * 3 + k);
        }
    }
    (void)r;
}
static void scene_cmds(rig_t* r, int with_blend, int time) {
    k3d_mat4 proj = k3d_mat4_perspective(1.7320508f, (float)r->W / (float)r->H, 0.5f, 40.0f), rot = k3d_mat4_rot_y(0.05f * (float)time), tr = k3d_mat4_translate(0, 0, 0), model = k3d_mat4_mul(&tr, &rot);
    (void)tr;
    float L[3] = { -0.4f, 0.7f, -0.5f }, LC[3] = { 0.8f, 0.8f, 0.7f }, LA[3] = { 0.2f, 0.2f, 0.25f };
    begin(); k3d_cb_clear(&cb, 0x101830, 0x405070, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_matrix(&cb, K3D_SLOT_MODEL, &model); k3d_cb_light(&cb, L, LC, LA);
    bind_all(); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0);
    k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_LIGHT_VERTEX, 0xFFFFFFFF); k3d_cb_draw(&cb, 0, 60, 0);
    k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_LIGHT_PIXEL | K3D_TEXTURE | K3D_FILTER_BILINEAR, 0xFFFFFFFF); k3d_cb_draw(&cb, 60, 90, 0);
    k3d_cb_state(&cb, K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_TEXTURE | K3D_WRAP_CLAMP, 0xFFFFFFFF); k3d_cb_draw(&cb, 150, 450, 0);
    if (with_blend) { k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_BLEND | K3D_LIGHT_VERTEX, 0x90FFFFFF); k3d_cb_draw(&cb, 600, 99, 0); }
}
static k3d_u32 chk[32 * 32];
static uint32_t render_scene_hash(int workers, k3d_u32 max_tris, k3d_u32 max_bins, int with_blend, int frames, uint32_t* stat_flushes, int check_each) {
    rig_t r = rig_new(150, 100, workers, max_tris, max_bins);
    if (!r.c) { CHECK(0); return 0; }
    CHECK(k3d_workers(r.c) == workers);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) chk[y * 32 + x] = 0xFF000000u | (((x >> 3) ^ (y >> 3)) & 1 ? 0xE0E0E0 : 0x2060A0);
    CHECK(k3d_tex_create(r.c, 32, 32, chk, 1) == 0 && k3d_vbuf_create(r.c, sv, NSV - 1) == 0 && k3d_ibuf_create(r.c, si, NSV - 1) == 0);
    uint32_t h = 0;
    for (int f = 0; f < frames; f++) {
        scene_cmds(&r, with_blend, f);
        CHECK(run(&r) == 0);
        uint32_t hf = hash_rig(&r); h = h * 31 + hf;
        if (check_each) CHECK(guard_ok(&r));
    }
    if (stat_flushes) *stat_flushes = k3d_stats(r.c)->flushes;
    CHECK(guard_ok(&r) && k3d_stats(r.c)->tris_drawn > 50);
    rig_free(&r);
    return h;
}
static void test_threads_and_capacity(void) {
    scene_mesh(0);
    uint32_t fl = 0;
    uint32_t h0 = render_scene_hash(0, 0, 0, 1, 6, &fl, 1);
    CHECK(fl == 1);
    int ws[] = { 1, 2, 3, 7 };
    for (int i = 0; i < 4; i++) CHECK(render_scene_hash(ws[i], 0, 0, 1, 6, 0, 1) == h0);        // побитно как без потоков (с прозрачностью, светом, текстурами)
    // тяжёлый прогон на многих потоках: гонок нет (под ASAN, выход потоков и уничтожение)
    CHECK(render_scene_hash(7, 0, 0, 1, 120, 0, 0) == render_scene_hash(0, 0, 0, 1, 120, 0, 0));
    // малая ёмкость: движок сам сбрасывает плитки несколько раз; без прозрачности результат тот же, что при большой ёмкости
    uint32_t hb = render_scene_hash(0, 0, 0, 0, 3, &fl, 1); CHECK(fl == 1);
    uint32_t hs = render_scene_hash(3, 7, 0, 0, 3, &fl, 1); CHECK(fl >= 10 && hs == hb);
    hs = render_scene_hash(0, 0, 20, 0, 3, &fl, 1); CHECK(fl > 1 && hs == hb);                     // мало записей в списках плиток
    // помощники не запустились: движок работает с теми, что есть (в том числе ни с одним)
    { k3d_config_t cfg; k3d_config_default(&cfg, 64, 48, 4); host_sys(&cfg.sys); cfg.par_min_cost = 0; void* mem; host_spawn_fail_after = 2;
      k3d_ctx* c = host_create(&cfg, &mem); CHECK(c && k3d_workers(c) == 2); host_spawn_fail_after = -1; k3d_destroy(c); free(mem);
      host_spawn_fail_after = 0; c = host_create(&cfg, &mem); CHECK(c && k3d_workers(c) == 0);
      k3d_u32 px[64 * 48]; k3d_u32 w[4 * 2]; k3d_cmdbuf_t b; k3d_cb_init(&b, w, 8); k3d_cb_clear(&b, 0x123456, 0x123456, 1.0f); CHECK(k3d_submit(c, w, b.n, px, 64) == 0 && px[100] == 0x123456);
      host_spawn_fail_after = -1; k3d_destroy(c); free(mem); }
    // порог: мало работы - в вызывающем потоке (flushes_parallel == 0), порог 0 - всегда с помощниками; картинка одна и та же
    { k3d_config_t cfg; k3d_config_default(&cfg, 64, 48, 2); host_sys(&cfg.sys); void* mem; k3d_u32 pa[64 * 48], pb[64 * 48]; k3d_u32 w[4 * 2]; k3d_cmdbuf_t b;
      k3d_cb_init(&b, w, 8); k3d_cb_clear(&b, 0x123456, 0x654321, 1.0f);
      cfg.par_min_cost = 0xFFFFFFFFu; k3d_ctx* c = host_create(&cfg, &mem); CHECK(c && k3d_workers(c) == 2);
      CHECK(k3d_submit(c, w, b.n, pa, 64) == 0 && k3d_stats(c)->flushes == 1 && k3d_stats(c)->flushes_parallel == 0); k3d_destroy(c); free(mem);
      cfg.par_min_cost = 0; c = host_create(&cfg, &mem);
      CHECK(k3d_submit(c, w, b.n, pb, 64) == 0 && k3d_stats(c)->flushes_parallel == 1 && memcmp(pa, pb, sizeof pa) == 0); k3d_destroy(c); free(mem); }
}

// ---------------- поток команд: переносимость, ошибки, ресурсы, память ----------------
static void test_stream_and_resources(void) {
    scene_mesh(0);
    rig_t a = rig_new(150, 100, 0, 0, 0), b = rig_new(150, 100, 2, 0, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) chk[y * 32 + x] = 0xFF000000u | (((x >> 3) ^ (y >> 3)) & 1 ? 0xE0E0E0 : 0x2060A0);
    rig_t* rs[2] = { &a, &b };
    for (int i = 0; i < 2; i++) CHECK(k3d_tex_create(rs[i]->c, 32, 32, chk, 1) == 0 && k3d_vbuf_create(rs[i]->c, sv, NSV - 1) == 0 && k3d_ibuf_create(rs[i]->c, si, NSV - 1) == 0);
    scene_cmds(&a, 1, 2);
    // тот же поток слов, скопированный в другой буфер и выполненный другим движком (другие потоки), даёт тот же кадр: поток самодостаточен
    k3d_u32* copy = (k3d_u32*)malloc(cb.n * 4); memcpy(copy, words, cb.n * 4);
    CHECK(k3d_submit(a.c, words, cb.n, a.px, a.pitch) == 0 && k3d_submit(b.c, copy, cb.n, b.px, b.pitch) == 0 && hash_rig(&a) == hash_rig(&b));
    // повторное выполнение даёт то же (состояние начинается заново каждый раз)
    uint32_t h = hash_rig(&a); CHECK(k3d_submit(a.c, words, cb.n, a.px, a.pitch) == 0 && hash_rig(&a) == h);
    // неверный поток: ничего не рисуется и не трогается
    rig_fill(&a, 0x5A5A5A); uint32_t before = hash_rig(&a);
    CHECK(k3d_submit(a.c, copy, cb.n - 1, a.px, a.pitch) == K3D_E_BADCMD && hash_rig(&a) == before);
    copy[0] = 0xFFFF0004; CHECK(k3d_submit(a.c, copy, cb.n, a.px, a.pitch) == K3D_E_BADCMD && hash_rig(&a) == before);
    CHECK(k3d_submit(a.c, words, cb.n, 0, a.pitch) == K3D_E_ARG && k3d_submit(a.c, words, cb.n, a.px, a.W - 1) == K3D_E_ARG && k3d_submit(0, words, cb.n, a.px, a.pitch) == K3D_E_ARG);
    // ошибки рисования: неверный номер, индекс за границей, нет привязок, текстура включена без текстуры
    struct { int step; } e[] = { {0}, {1}, {2}, {3}, {4}, {5} };
    for (int k = 0; k < 6; k++) {
        begin(); k3d_cb_clear(&cb, 0x111111, 0x111111, 1.0f);
        switch (e[k].step) {
        case 0: k3d_cb_bind(&cb, K3D_BIND_VERTEX, 5); break;                               // нет такого буфера
        case 1: bind_all(); k3d_cb_draw(&cb, 0, 3, 100000); break;                         // смещение вершин за буфером
        case 2: bind_all(); k3d_cb_draw(&cb, NSV - 3, 6, 0); break;                        // индексы за буфером
        case 3: k3d_cb_draw(&cb, 0, 3, 0); break;                                          // нет привязок
        case 4: bind_all(); k3d_cb_state(&cb, K3D_TEXTURE | K3D_CULL_NONE, 0xFFFFFFFF); k3d_cb_draw(&cb, 0, 3, 0); break;   // текстура включена, не привязана
        case 5: k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 9); break;
        }
        int rc = k3d_submit(a.c, words, cb.n, a.px, a.pitch);
        CHECK(rc < 0 && at(&a, 3, 3) == 0x111111 && guard_ok(&a));                          // очистка до ошибки доходит до цели, остального нет
    }
    // после ошибки движок исправен
    scene_cmds(&a, 1, 2); CHECK(k3d_submit(a.c, words, cb.n, a.px, a.pitch) == 0 && hash_rig(&a) == h);
    free(copy);
    // ресурсы: пределы
    k3d_ctx* c = a.c; k3d_res_reset(c);
    k3d_vertex_t vv[3]; memset(vv, 0, sizeof vv); k3d_u16 ii[3] = { 0, 1, 2 };
    CHECK(k3d_vbuf_create(c, vv, 2) == K3D_E_ARG && k3d_vbuf_create(c, vv, a.cfg.max_verts + 1) == K3D_E_ARG && k3d_vbuf_create(c, 0, 3) == K3D_E_ARG && k3d_ibuf_create(c, ii, 2) == K3D_E_ARG);
    int made = 0; while (k3d_vbuf_create(c, vv, 3) >= 0) made++;
    CHECK(made == K3D_MAX_RES); k3d_res_reset(c);
    k3d_u32* big = (k3d_u32*)calloc(1024 * 1024, 4); int tn = 0; while (k3d_tex_create(c, 1024, 1024, big, 1) >= 0) tn++;
    CHECK(tn == 0);                                                                          // с мип-уровнями 1024x1024 не помещается в пул 1 МБ
    while (k3d_tex_create(c, 256, 256, big, 1) >= 0) tn++;
    CHECK(tn >= 2 && tn <= 3 && k3d_tex_create(c, 256, 256, big, 1) == K3D_E_NOMEM);
    free(big);
    rig_free(&a); rig_free(&b);
    // создание: неверные настройки, мало памяти, невыровненная память
    k3d_config_t cfg; k3d_config_default(&cfg, 64, 64, 0); host_sys(&cfg.sys);
    k3d_u64 need = k3d_mem_size(&cfg); CHECK(need > 64 * 64 * 4);
    void* mem = aligned_alloc(64, (need + 63) & ~63ull);
    CHECK(k3d_create(&cfg, mem, need - 16) == 0 && k3d_create(&cfg, (char*)mem + 8, need) == 0 && k3d_create(&cfg, 0, need) == 0 && k3d_create(&cfg, mem, need) != 0);
    k3d_config_t bad = cfg; bad.width = 0; CHECK(k3d_mem_size(&bad) == 0 && k3d_create(&bad, mem, need) == 0);
    bad = cfg; bad.max_bins = 3; CHECK(k3d_mem_size(&bad) == 0);                               // меньше числа плиток
    bad = cfg; bad.workers = 2; bad.sys.spawn = 0; CHECK(k3d_mem_size(&bad) == 0);
    bad = cfg; bad.workers = 99; CHECK(k3d_mem_size(&bad) == 0);
    bad = cfg; bad.max_tris = 70000; CHECK(k3d_mem_size(&bad) == 0);
    bad = cfg; bad.width = 5000; CHECK(k3d_mem_size(&bad) == 0);
    free(mem);
    // окна не кратных плитке размеров: 1x1, 33x31, 100x7 - вся поверхность очищается и рисуется, за границы не пишется
    int sizes[][2] = { {1, 1}, {33, 31}, {100, 7}, {31, 97} };
    for (int k = 0; k < 4; k++) {
        rig_t t = rig_new(sizes[k][0], sizes[k][1], 2, 0, 0);
        CHECK(t.c != 0);
        k3d_vertex_t q[4]; k3d_u16 qi[6] = { 0, 2, 1, 0, 3, 2 };
        for (int i = 0; i < 4; i++) q[i] = vpx(&t, i == 1 || i == 2 ? (float)t.W : 0.0f, i >= 2 ? (float)t.H : 0.0f, 0.5f, 0xFF80C0FF);
        CHECK(mk(&t, q, 4, qi, 6));
        begin(); k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_state(&cb, K3D_CULL_NONE, 0xFFFFFFFF); bind_all(); k3d_cb_draw(&cb, 0, 6, 0);
        CHECK(run(&t) == 0 && guard_ok(&t));
        int bad_px = 0; for (int y = 0; y < t.H; y++) for (int x = 0; x < t.W; x++) if (at(&t, x, y) != 0x80C0FF) bad_px++;
        CHECK(bad_px == 0);
        rig_free(&t);
    }
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    test_math_and_commands();
    test_coverage(); if (getenv("K3D_TRACE")) printf("ok test_coverage\n");
    test_depth(); if (getenv("K3D_TRACE")) printf("ok test_depth\n");
    test_perspective(); if (getenv("K3D_TRACE")) printf("ok test_perspective\n");
    test_clipping(); if (getenv("K3D_TRACE")) printf("ok test_clipping\n");
    test_lighting(); if (getenv("K3D_TRACE")) printf("ok test_lighting\n");
    test_textures(); if (getenv("K3D_TRACE")) printf("ok test_textures\n");
    test_alpha(); if (getenv("K3D_TRACE")) printf("ok test_alpha\n");
    test_threads_and_capacity(); if (getenv("K3D_TRACE")) printf("ok test_threads_and_capacity\n");
    test_stream_and_resources(); if (getenv("K3D_TRACE")) printf("ok test_stream_and_resources\n");
    if (fails) { printf("k3d: %d FAILED\n", fails); return 1; }
    printf("k3d: coverage vs reference (top-left, no holes/overlap), depth, perspective, clipping, culling, lighting, textures, alpha, threads (bit-identical), command stream, resources passed\n");
    return 0;
}
