/* 3D лаборатория: программный рендерер в программе Ring 3. Раньше жил в ядре (gfx3d.c) и требовал SSE в ядре;
   теперь ядро без SSE, а это обычная программа со своим окном, своим z-буфером в куче и своим состоянием FPU/SSE.
   Геометрия считается на float (SSE2), внутренний цикл растеризации - целочисленный (фиксированная точка
   28.4 для границ и 40.16 для глубины): быстро и без аппаратного ускорения. */
#include "kira.h"
KIRA_ENTRY()

#define W 360
#define H 270

#define PI_F      3.14159265358979f
#define TWO_PI_D  6.283185307179586

typedef struct { u32* px; int w, h, pitch; } surf_t;
typedef struct { float x, y, z; } v3;
typedef struct { float m[3][3]; } m3;

static inline float f_min(float a, float b) { return a < b ? a : b; }
static inline float f_max(float a, float b) { return a > b ? a : b; }
static inline v3 v_sub(v3 a, v3 b) { v3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static inline v3 v_cross(v3 a, v3 b) { v3 r = {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; return r; }
static inline float v_dot(v3 a, v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float v_len(v3 a) { return __builtin_sqrtf(v_dot(a, a)); }

/* ---- sin/cos без libm: ряд Тейлора до x^11 на [-pi/2, pi/2], погрешность < 1e-6 ---- */
static float k_sin_range(float x) {
    if (x > PI_F * 0.5f) x = PI_F - x;
    else if (x < -PI_F * 0.5f) x = -PI_F - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.6666667e-1f + x2 * (8.3333333e-3f + x2 * (-1.9841270e-4f
             + x2 * (2.7557319e-6f + x2 * -2.5052108e-8f)))));
}
static float k_sinf(float x) {
    while (x > PI_F) x -= 2.0f * PI_F;
    while (x < -PI_F) x += 2.0f * PI_F;
    return k_sin_range(x);
}
static float k_cosf(float x) { return k_sinf(x + PI_F * 0.5f); }

/* Угол 0..2pi из времени: периодизация в целых числах, чтобы не терять точность за часы работы */
static float angle_ns(u64 t_ns, u64 period_ns) {
    double ph = (double)(t_ns % period_ns) / (double)period_ns;
    return (float)(ph * TWO_PI_D);
}

static m3 m_rot_x(float a) { float c = k_cosf(a), s = k_sinf(a); m3 r = {{{1,0,0},{0,c,-s},{0,s,c}}}; return r; }
static m3 m_rot_y(float a) { float c = k_cosf(a), s = k_sinf(a); m3 r = {{{c,0,s},{0,1,0},{-s,0,c}}}; return r; }
static m3 m_mul(const m3* a, const m3* b) {
    m3 r;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        r.m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j] + a->m[i][2]*b->m[2][j];
    return r;
}
static v3 m_apply(const m3* m, v3 v, float scale, v3 t) {
    v3 r = { (m->m[0][0]*v.x + m->m[0][1]*v.y + m->m[0][2]*v.z) * scale + t.x,
             (m->m[1][0]*v.x + m->m[1][1]*v.y + m->m[1][2]*v.z) * scale + t.y,
             (m->m[2][0]*v.x + m->m[2][1]*v.y + m->m[2][2]*v.z) * scale + t.z };
    return r;
}

/* ---- модели ---- */
typedef struct { u8 a, b, c; u32 color; } tri_t;

static v3 cube_v[8] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}
};
static tri_t cube_t[12] = {
    {0,1,2,0xE0453A}, {0,2,3,0xE0453A},   /* -z  красный */
    {5,4,7,0x3AB0E0}, {5,7,6,0x3AB0E0},   /* +z  голубой */
    {4,0,3,0x4CC76B}, {4,3,7,0x4CC76B},   /* -x  зелёный */
    {1,5,6,0xF0C040}, {1,6,2,0xF0C040},   /* +x  жёлтый */
    {3,2,6,0xA070E0}, {3,6,7,0xA070E0},   /* +y  фиолетовый */
    {4,5,1,0xF08A3A}, {4,1,0,0xF08A3A},   /* -y  оранжевый */
};
static v3 oct_v[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static tri_t oct_t[8] = {
    {0,2,4,0xFFFFFF}, {2,1,4,0xD8E8FF}, {1,3,4,0xFFFFFF}, {3,0,4,0xD8E8FF},
    {2,0,5,0xD8E8FF}, {1,2,5,0xFFFFFF}, {3,1,5,0xD8E8FF}, {0,3,5,0xFFFFFF},
};

/* Приводит обход каждой грани к "нормаль смотрит наружу" (фигуры выпуклые и центрированы в 0). */
static int models_ready;
static void fix_winding(const v3* v, tri_t* t, int n) {
    for (int i = 0; i < n; i++) {
        v3 a = v[t[i].a], b = v[t[i].b], c = v[t[i].c];
        v3 nrm = v_cross(v_sub(b, a), v_sub(c, a));
        v3 cen = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
        if (v_dot(nrm, cen) < 0) { u8 tmp = t[i].b; t[i].b = t[i].c; t[i].c = tmp; }
    }
}
static void prepare_models(void) {
    if (models_ready) return;
    fix_winding(cube_v, cube_t, 12);
    fix_winding(oct_v, oct_t, 8);
    models_ready = 1;
}

/* ---- растеризатор ---- */
typedef struct { float x, y, d; } sv_t;       /* экранные координаты и глубина 0..1 */

static u32* zbuf;
static int zw, zh;
static u32 stat_px, stat_tris;
typedef struct { u64 frames; u32 tris_drawn, pixels_written; } stats_t;
static stats_t stats;

#define ZMAX 16777215.0

/* Треугольник в экранных координатах: целочисленные граничные функции (28.4, центры пикселей),
   плоскость глубины в фиксированной точке. Порядок обхода любой. */
static void raster_tri(surf_t* t, sv_t a, sv_t b, sv_t c, u32 color) {
    i64 ax = (i64)(a.x * 16.0f + (a.x >= 0 ? 0.5f : -0.5f)), ay = (i64)(a.y * 16.0f + (a.y >= 0 ? 0.5f : -0.5f));
    i64 bx = (i64)(b.x * 16.0f + (b.x >= 0 ? 0.5f : -0.5f)), by = (i64)(b.y * 16.0f + (b.y >= 0 ? 0.5f : -0.5f));
    i64 cx = (i64)(c.x * 16.0f + (c.x >= 0 ? 0.5f : -0.5f)), cy = (i64)(c.y * 16.0f + (c.y >= 0 ? 0.5f : -0.5f));

    i64 area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area2 == 0) return;
    if (area2 < 0) { /* единая ориентация */
        i64 tx = bx, ty = by; bx = cx; by = cy; cx = tx; cy = ty;
        sv_t ts = b; b = c; c = ts;
        area2 = -area2;
    }

    i64 minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    i64 maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    i64 miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    i64 maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    int x0 = (int)(minx >> 4), x1 = (int)(maxx >> 4), y0 = (int)(miny >> 4), y1 = (int)(maxy >> 4);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w - 1) x1 = t->w - 1;
    if (y1 > t->h - 1) y1 = t->h - 1;
    if (x0 > x1 || y0 > y1) return;

    /* E(p) = (q.x-p.x)*(y-p.y) - (q.y-p.y)*(x-p.x); внутри треугольника все три >= 0 */
    i64 px0 = (i64)x0 * 16 + 8, py0 = (i64)y0 * 16 + 8;
    i64 e0 = (bx - ax) * (py0 - ay) - (by - ay) * (px0 - ax);
    i64 e1 = (cx - bx) * (py0 - by) - (cy - by) * (px0 - bx);
    i64 e2 = (ax - cx) * (py0 - cy) - (ay - cy) * (px0 - cx);
    i64 a0 = -(by - ay) * 16, b0 = (bx - ax) * 16;
    i64 a1 = -(cy - by) * 16, b1 = (cx - bx) * 16;
    i64 a2 = -(ay - cy) * 16, b2 = (ax - cx) * 16;

    /* плоскость глубины d(x,y) по вершинам (double: настройка раз на треугольник) */
    double fax = (double)ax / 16.0, fay = (double)ay / 16.0;
    double fbx = (double)bx / 16.0, fby = (double)by / 16.0;
    double fcx = (double)cx / 16.0, fcy = (double)cy / 16.0;
    double det = (fbx - fax) * (fcy - fay) - (fcx - fax) * (fby - fay);
    double dzdx = ((double)(b.d - a.d) * (fcy - fay) - (double)(c.d - a.d) * (fby - fay)) / det;
    double dzdy = ((double)(c.d - a.d) * (fbx - fax) - (double)(b.d - a.d) * (fcx - fax)) / det;
    double zc = (double)a.d + dzdx * ((double)x0 + 0.5 - fax) + dzdy * ((double)y0 + 0.5 - fay);
    const double S = ZMAX * 65536.0;
    i64 z_row = (i64)(zc * S);
    i64 z_dx = (i64)(dzdx * S);
    i64 z_dy = (i64)(dzdy * S);

    for (int y = y0; y <= y1; y++) {
        i64 E0 = e0, E1 = e1, E2 = e2, Z = z_row;
        u32* row = t->px + (long)y * t->pitch;
        u32* zr = zbuf + (long)y * zw;
        for (int x = x0; x <= x1; x++) {
            if ((E0 | E1 | E2) >= 0) {
                i64 zz = Z >> 16;
                if (zz < 0) zz = 0;
                u32 zi = (u32)zz;
                if (zi < zr[x]) { zr[x] = zi; row[x] = color; stat_px++; }
            }
            E0 += a0; E1 += a1; E2 += a2; Z += z_dx;
        }
        e0 += b0; e1 += b1; e2 += b2; z_row += z_dy;
    }
    stat_tris++;
}

/* ---- сцена ---- */
typedef struct { float zn, zf, f_over_aspect, f; float hw, hh; } cam_t;

static int project(const cam_t* c, v3 p, sv_t* out) {
    if (p.z < c->zn) return 0;                    /* отсечения нет: треугольники у камеры пропускаются */
    float inv = 1.0f / p.z;
    out->x = c->hw * (1.0f + c->f_over_aspect * p.x * inv);
    out->y = c->hh * (1.0f - c->f * p.y * inv);
    out->d = (c->zf / (c->zf - c->zn)) * (1.0f - c->zn * inv);
    return 1;
}

static void draw_model(surf_t* t, const cam_t* cam, const v3* mv, int nv, const tri_t* tr, int nt,
                       const m3* rot, float scale, v3 pos, v3 light) {
    v3 w[8]; sv_t s[8]; int ok[8];
    for (int i = 0; i < nv; i++) {
        w[i] = m_apply(rot, mv[i], scale, pos);
        ok[i] = project(cam, w[i], &s[i]);
    }
    for (int i = 0; i < nt; i++) {
        int a = tr[i].a, b = tr[i].b, c = tr[i].c;
        if (!ok[a] || !ok[b] || !ok[c]) continue;
        v3 n = v_cross(v_sub(w[b], w[a]), v_sub(w[c], w[a]));
        v3 cen = { (w[a].x + w[b].x + w[c].x) / 3.0f, (w[a].y + w[b].y + w[c].y) / 3.0f, (w[a].z + w[b].z + w[c].z) / 3.0f };
        float len = v_len(n);
        if (len < 1e-9f) continue;
        float inv = 1.0f / len;
        n.x *= inv; n.y *= inv; n.z *= inv;
        if (v_dot(n, cen) >= 0) continue;         /* грань смотрит от камеры (камера в начале координат) */
        float diff = f_max(0.0f, v_dot(n, light));
        float k = 0.22f + 0.78f * diff;
        float r = (float)((tr[i].color >> 16) & 255) * k, g = (float)((tr[i].color >> 8) & 255) * k, bl = (float)(tr[i].color & 255) * k;
        u32 col = ((u32)f_min(r, 255.0f) << 16) | ((u32)f_min(g, 255.0f) << 8) | (u32)f_min(bl, 255.0f);
        raster_tri(t, s[a], s[b], s[c], col);
    }
}

/* z-буфер даёт вызывающий: в программе - кусок кучи, в хост-тесте - malloc. */
static int scene_init(u32* z, int width, int height) {
    prepare_models();
    if (!z || width <= 0 || height <= 0) return -1;
    zbuf = z; zw = width; zh = height;
    stats.frames = 0;
    return 0;
}

static void render_scene(surf_t* t, u64 t_ns) {
    if (!zbuf || t->w != zw || t->h != zh) return;
    stat_px = 0; stat_tris = 0;

    /* фон: вертикальный градиент, z-буфер на бесконечность */
    for (int y = 0; y < t->h; y++) {
        unsigned k = (unsigned)y * 255u / (unsigned)t->h;
        u32 c = ((10 + k / 12) << 16) | ((14 + k / 9) << 8) | (34 + k / 5);
        u32* row = t->px + (long)y * t->pitch;
        for (int x = 0; x < t->w; x++) row[x] = c;
    }
    for (long i = 0; i < (long)zw * zh; i++) zbuf[i] = 0xFFFFFFFFu;

    cam_t cam;
    float aspect = (float)t->w / (float)t->h;
    cam.zn = 0.5f; cam.zf = 30.0f;
    cam.f = 1.7320508f;                            /* fov 60 градусов по вертикали */
    cam.f_over_aspect = cam.f / aspect;
    cam.hw = (float)t->w * 0.5f; cam.hh = (float)t->h * 0.5f;

    v3 light = {-0.45f, 0.70f, -0.55f};
    float ll = v_len(light); light.x /= ll; light.y /= ll; light.z /= ll; /* "к свету" (камера смотрит в +z) */

    float ay = angle_ns(t_ns, 14000000000ULL), ax = angle_ns(t_ns, 23000000000ULL);
    m3 ry = m_rot_y(ay), rx = m_rot_x(ax);
    m3 cube_rot = m_mul(&ry, &rx);
    v3 cube_pos = {0.0f, 0.0f, 6.2f};
    draw_model(t, &cam, cube_v, 8, cube_t, 12, &cube_rot, 1.25f, cube_pos, light);

    /* октаэдр на наклонной орбите: проходит и перед кубом, и за ним (проверка z-буфера) */
    float ao = angle_ns(t_ns, 6000000000ULL);
    v3 opos = { 3.0f * k_cosf(ao), 1.1f * k_sinf(ao * 2.0f), 6.2f + 2.6f * k_sinf(ao) };
    m3 ory = m_rot_y(-ao * 3.0f);
    m3 orx = m_rot_x(ao * 2.0f);
    m3 orot = m_mul(&ory, &orx);
    draw_model(t, &cam, oct_v, 6, oct_t, 8, &orot, 0.75f, opos, light);

    stats.frames++;
    stats.tris_drawn = stat_tris;
    stats.pixels_written = stat_px;
}

/* ---- самопроверка ---- */
static int absf_lt(float a, float b, float tol) { float d = a - b; if (d < 0) d = -d; return d < tol; }

static int selftest(void) {
    prepare_models();
    /* 1. тригонометрия */
    if (!absf_lt(k_sinf(0.0f), 0.0f, 1e-6f)) return 1;
    if (!absf_lt(k_sinf(PI_F / 6.0f), 0.5f, 2e-6f)) return 2;
    if (!absf_lt(k_cosf(PI_F / 3.0f), 0.5f, 2e-6f)) return 3;
    if (!absf_lt(k_cosf(PI_F), -1.0f, 2e-6f)) return 4;
    if (!absf_lt(k_sinf(4.0f), -0.7568025f, 3e-6f)) return 5;
    if (!absf_lt(k_cosf(-2.5f), -0.8011436f, 3e-6f)) return 6;
    /* 2. все нормали смотрят наружу */
    for (int i = 0; i < 12; i++) {
        v3 a = cube_v[cube_t[i].a], b = cube_v[cube_t[i].b], c = cube_v[cube_t[i].c];
        v3 n = v_cross(v_sub(b, a), v_sub(c, a));
        v3 cen = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
        if (v_dot(n, cen) <= 0) return 7;
    }
    for (int i = 0; i < 8; i++) {
        v3 a = oct_v[oct_t[i].a], b = oct_v[oct_t[i].b], c = oct_v[oct_t[i].c];
        v3 n = v_cross(v_sub(b, a), v_sub(c, a));
        v3 cen = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
        if (v_dot(n, cen) <= 0) return 8;
    }
    /* 3. растеризация и z-тест на буфере 16x16 (на стеке) */
    u32 pix[16 * 16], zb[16 * 16];
    surf_t s = { pix, 16, 16, 16 };
    u32* saved_z = zbuf; int saved_w = zw, saved_h = zh;
    u32 sp = stat_px, st = stat_tris;
    zbuf = zb; zw = 16; zh = 16;
    for (int i = 0; i < 256; i++) { pix[i] = 0; zb[i] = 0xFFFFFFFFu; }
    sv_t a = {0, 0, 0.5f}, b = {16, 0, 0.5f}, c = {0, 16, 0.5f};
    raster_tri(&s, a, b, c, 0x111111);
    int cnt = 0; for (int i = 0; i < 256; i++) if (pix[i]) cnt++;
    int rc = 0;
    if (cnt < 120 || cnt > 136) rc = 9;                         /* половина квадрата 16x16 = 128 */
    sv_t a2 = {0, 0, 0.8f}, b2 = {16, 0, 0.8f}, c2 = {0, 16, 0.8f}; /* дальше (больше d) - не должен перекрыть */
    raster_tri(&s, a2, b2, c2, 0x222222);
    for (int i = 0; i < 256; i++) if (pix[i] == 0x222222) rc = rc ? rc : 10;
    sv_t a3 = {0, 0, 0.2f}, b3 = {16, 0, 0.2f}, c3 = {0, 16, 0.2f}; /* ближе - перекрывает */
    raster_tri(&s, a3, b3, c3, 0x333333);
    int near_cnt = 0; for (int i = 0; i < 256; i++) if (pix[i] == 0x333333) near_cnt++;
    if (near_cnt != cnt) rc = rc ? rc : 11;
    /* обратный обход даёт то же самое */
    for (int i = 0; i < 256; i++) { pix[i] = 0; zb[i] = 0xFFFFFFFFu; }
    raster_tri(&s, a, c, b, 0x444444);
    int cnt2 = 0; for (int i = 0; i < 256; i++) if (pix[i] == 0x444444) cnt2++;
    if (cnt2 != cnt) rc = rc ? rc : 12;
    zbuf = saved_z; zw = saved_w; zh = saved_h; stat_px = sp; stat_tris = st;
    return rc;
}

/* ---- программа ---- */
static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    char o[21]; int i = 0; while (n) o[i++] = t[--n];
    k_write(o, (u64)i);
}

int umain(void) {
    int rc = selftest();
    if (rc) { say("cube3d: selftest FAILED, check #"); say_u((u64)rc); say("\n"); return 1; }
    say("cube3d: selftest PASS (trig, face winding, rasterizer, z-test)\n");

    u32* px = k_gfx_create(W, H);
    if (!px) { say("cube3d: cannot create window\n"); return 1; }
    u32* z = (u32*)k_heap_grow((W * H * 4 + 4095) / 4096);       /* z-буфер: 95 страниц кучи */
    if (!z || scene_init(z, W, H) != 0) { say("cube3d: cannot allocate the z-buffer\n"); return 1; }
    say("cube3d: software renderer running; close the window or press q to exit\n");

    surf_t s = { px, W, H, W };
    u64 t0 = k_ticks(), frames = 0;
    for (;;) {
        kevent_t ev;
        int quit = 0;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) quit = 1;
            if (ev.type == KEV_KEY_DOWN && ev.b == 'q') quit = 1;
        }
        if (quit) break;
        u64 now = k_ticks();
        render_scene(&s, (now - t0) * 10000000ULL);              /* тик = 10 мс */
        k_gfx_present();
        frames++;
        k_sleep(1);
    }
    u64 dt = k_ticks() - t0;
    say("cube3d: "); say_u(frames); say(" frames, ");
    say_u(dt ? frames * 100 / dt : 0); say(" fps average, last frame "); say_u(stats.tris_drawn);
    say(" triangles, "); say_u(stats.pixels_written); say(" px shaded\n");
    return 0;
}
