/* 3D лаборатория: сцена на движке k3d (userland/k3d.c) - плиточный растеризатор с буфером глубины, перспективно-верными цветом
   и текстурами, билинейной выборкой, отсечением, светом Ламберта и прозрачностью; плитки считают несколько потоков процесса.
   Раньше здесь был свой простой растеризатор без текстур; всё, что он умел (вращающийся куб и октаэдр на орбите, свет, глубина,
   самопроверка, выход по q и закрытию окна, отчёт в журнал), осталось. Ядро собирается без SSE, а это обычная программа со своим
   состоянием FPU/SSE: геометрия считается на float (SSE2), внутренний цикл плиток - тоже.
   Аргументы запуска (run cube3d ...): threads=N (потоков-помощников 0..8, по умолчанию 3), bench (замер без пауз: кадры/с и
   время геометрии и растеризации для 0 помощников и для N). */
#include "kira.h"
#include "k3d_kira.h"
KIRA_ENTRY()

#define W 360
#define H 270
#define DEFAULT_WORKERS 3

typedef struct { u32* px; int w, h, pitch; } surf_t;
typedef struct { float x, y, z; } v3;

static inline v3 v_sub(v3 a, v3 b) { v3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static inline v3 v_cross(v3 a, v3 b) { v3 r = {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; return r; }
static inline float v_dot(v3 a, v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

/* Угол 0..2pi из времени: периодизация в целых числах, чтобы не терять точность за часы работы */
static float angle_ns(u64 t_ns, u64 period_ns) {
    double ph = (double)(t_ns % period_ns) / (double)period_ns;
    return (float)(ph * 6.283185307179586);
}

/* ---- модели: вершины и индексы для движка ---- */
#define MAX_V 512
#define MAX_I 2048
typedef struct { k3d_vertex_t v[MAX_V]; k3d_u16 i[MAX_I]; u32 nv, ni; } mesh_t;
static mesh_t m_cube, m_oct, m_sphere;

static v3 mesh_pos(const k3d_vertex_t* v) { v3 r = {v->x, v->y, v->z}; return r; }

/* Добавляет треугольник; обход выбирается так, чтобы грань с внешней нормалью n была ЛИЦЕВОЙ: лицевая - против часовой с точки зрения камеры
   (y вверх, z вперёд), для неё векторное произведение (b-a)x(c-a) смотрит ВНУТРЬ фигуры (против нормали). */
static void add_tri(mesh_t* m, u32 a, u32 b, u32 c, v3 n) {
    v3 cr = v_cross(v_sub(mesh_pos(&m->v[b]), mesh_pos(&m->v[a])), v_sub(mesh_pos(&m->v[c]), mesh_pos(&m->v[a])));
    if (v_dot(cr, n) > 0.0f) { u32 t = b; b = c; c = t; }
    m->i[m->ni++] = (k3d_u16)a; m->i[m->ni++] = (k3d_u16)b; m->i[m->ni++] = (k3d_u16)c;
}
static u32 add_vert(mesh_t* m, v3 p, v3 n, float u, float v, u32 color) {
    k3d_vertex_t* d = &m->v[m->nv];
    d->x = p.x; d->y = p.y; d->z = p.z; d->nx = n.x; d->ny = n.y; d->nz = n.z; d->u = u; d->v = v; d->color = color;
    return m->nv++;
}

static void build_cube(void) {
    static const v3 cv[8] = { {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
    static const struct { u8 q[4]; v3 n; u32 color; } faces[6] = {
        {{0,3,2,1}, { 0, 0,-1}, 0xFFE0453A}, {{5,6,7,4}, { 0, 0, 1}, 0xFF3AB0E0},   /* -z красный, +z голубой */
        {{4,7,3,0}, {-1, 0, 0}, 0xFF4CC76B}, {{1,2,6,5}, { 1, 0, 0}, 0xFFF0C040},   /* -x зелёный, +x жёлтый */
        {{3,7,6,2}, { 0, 1, 0}, 0xFFA070E0}, {{4,0,1,5}, { 0,-1, 0}, 0xFFF08A3A},   /* +y фиолетовый, -y оранжевый */
    };
    m_cube.nv = m_cube.ni = 0;
    for (int f = 0; f < 6; f++) {
        static const float uu[4] = {0, 0, 1, 1}, vv[4] = {0, 1, 1, 0};
        u32 id[4];
        for (int k = 0; k < 4; k++) id[k] = add_vert(&m_cube, cv[faces[f].q[k]], faces[f].n, uu[k], vv[k], faces[f].color);
        add_tri(&m_cube, id[0], id[1], id[2], faces[f].n); add_tri(&m_cube, id[0], id[2], id[3], faces[f].n);
    }
}

static void build_oct(void) {
    m_oct.nv = m_oct.ni = 0;
    for (int s = 0; s < 8; s++) {
        float sx = (s & 1) ? -1.0f : 1.0f, sy = (s & 2) ? -1.0f : 1.0f, sz = (s & 4) ? -1.0f : 1.0f;
        v3 p[3] = { {sx, 0, 0}, {0, sy, 0}, {0, 0, sz} };
        v3 n = { sx * 0.57735027f, sy * 0.57735027f, sz * 0.57735027f };
        u32 col = ((s ^ (s >> 1) ^ (s >> 2)) & 1) ? 0xFFFFFFFF : 0xFFD8E8FF, id[3];    /* грани в шахматном порядке: белая / бледно-голубая */
        for (int k = 0; k < 3; k++) id[k] = add_vert(&m_oct, p[k], n, 0.0f, 0.0f, col);
        add_tri(&m_oct, id[0], id[1], id[2], n);
    }
}

#define SPH_R 12
#define SPH_S 20
static void build_sphere(void) {
    m_sphere.nv = m_sphere.ni = 0;
    for (int i = 0; i <= SPH_R; i++) {
        float th = 3.14159265f * (float)i / (float)SPH_R, st = k3d_sinf(th), ct = k3d_cosf(th);
        for (int j = 0; j <= SPH_S; j++) {
            float ph = 6.2831853f * (float)j / (float)SPH_S;
            v3 p = { st * k3d_cosf(ph), ct, st * k3d_sinf(ph) };
            add_vert(&m_sphere, p, p, (float)j / (float)SPH_S, (float)i / (float)SPH_R, 0xFFFFFFFF);
        }
    }
    for (int i = 0; i < SPH_R; i++) for (int j = 0; j < SPH_S; j++) {
        u32 a = (u32)(i * (SPH_S + 1) + j), b = a + 1, c = a + (SPH_S + 1), d = c + 1;
        v3 pa = mesh_pos(&m_sphere.v[a]), pd = mesh_pos(&m_sphere.v[d]);
        v3 n = { pa.x + pd.x, pa.y + pd.y, pa.z + pd.z };
        if (i > 0) add_tri(&m_sphere, a, c, b, n);                  /* у полюса квад вырождается в один треугольник */
        if (i < SPH_R - 1) add_tri(&m_sphere, b, c, d, n);
    }
}

/* ---- текстуры (процедурные, без файлов) ---- */
static u32 hash32(u32 x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static u32 tex_brick[128 * 128], tex_planet[128 * 64];
static void build_textures(void) {
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {                 /* кирпичи 32x16 со швом 2 пикселя, ряды сдвинуты */
        int row = y / 16, xx = (x + (row & 1) * 16) & 127, bx = xx / 32, ly = y & 15, lx = xx & 31;
        u32 col;
        if (ly < 2 || lx < 2) col = 0x9A968C;
        else {
            u32 h = hash32((u32)(row * 8 + bx) * 2654435761u + 77), n = hash32((u32)(x * 131 + y * 71)) & 15;
            int r = 150 + (int)(h & 31) - 16 + (int)n - 8, g = 70 + (int)((h >> 5) & 15) - 8 + (int)n / 2, b = 50 + (int)((h >> 9) & 15) - 8 + (int)n / 2;
            col = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
        tex_brick[y * 128 + x] = 0xFF000000u | col;
    }
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {                  /* планета: полосы по широте и пятна */
        int band = (y * 7 / 64) & 1;
        u32 n = hash32((u32)((x >> 2) * 97 + (y >> 2) * 1013)) & 63;
        int r = band ? 70 : 205, g = band ? 120 : 160, b = band ? 190 : 90;
        r += (int)n - 32; g += (int)n / 2 - 16; b += (int)n / 3 - 10;
        if (n > 52) { r = 235; g = 235; b = 245; }                                 /* "облака" */
        r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b);
        tex_planet[y * 128 + x] = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    }
}

/* ---- сцена ---- */
typedef struct {
    k3d_ctx* c; int w, h;
    int vb[3], ib[3], tex[2];
    k3d_stats_t last;                         /* статистика последнего кадра */
    u64 ns_geom, ns_raster;                   /* накоплено за все кадры */
    int ready;
} scene_t;
static scene_t sc;
static int models_ready;
static u32 cfg_tris = 4096, cfg_res = 1u << 20;       /* профиль памяти движка (grab_arena подбирает по куче) */

static void prepare_models(void) {
    if (models_ready) return;
    build_cube(); build_oct(); build_sphere(); build_textures();
    models_ready = 1;
}

/* Память: arena от программы (k_heap_grow). Возвращает 0 или -1. */
static int scene_init(void* mem, u64 bytes, int width, int height, int workers) {
    prepare_models();
    sc.ready = 0; sc.c = 0;
    if (!mem || width <= 0 || height <= 0) return -1;
    k3d_config_t cfg; k3d_config_default(&cfg, width, height, workers); k3d_kira_sys(&cfg.sys);
    cfg.max_tris = cfg_tris; cfg.res_bytes = cfg_res;
    sc.c = k3d_create(&cfg, mem, bytes);
    if (!sc.c) return -1;
    mesh_t* ms[3] = { &m_cube, &m_oct, &m_sphere };
    for (int i = 0; i < 3; i++) {
        sc.vb[i] = k3d_vbuf_create(sc.c, ms[i]->v, ms[i]->nv); sc.ib[i] = k3d_ibuf_create(sc.c, ms[i]->i, ms[i]->ni);
        if (sc.vb[i] < 0 || sc.ib[i] < 0) { k3d_destroy(sc.c); sc.c = 0; return -1; }
    }
    sc.tex[0] = k3d_tex_create(sc.c, 128, 128, tex_brick, 1); sc.tex[1] = k3d_tex_create(sc.c, 128, 64, tex_planet, 1);
    if (sc.tex[0] < 0 || sc.tex[1] < 0) { k3d_destroy(sc.c); sc.c = 0; return -1; }
    sc.w = width; sc.h = height; sc.ns_geom = sc.ns_raster = 0; sc.ready = 1;
    return 0;
}
static void scene_close(void) { if (sc.c) k3d_destroy(sc.c); sc.c = 0; sc.ready = 0; }

static k3d_u32 words[1024];

static void draw_model(k3d_cmdbuf_t* cb, int mesh, const k3d_mat4* model, k3d_u32 flags, k3d_u32 tint, int tex) {
    k3d_cb_matrix(cb, K3D_SLOT_MODEL, model);
    k3d_cb_state(cb, flags, tint);
    k3d_cb_bind(cb, K3D_BIND_VERTEX, sc.vb[mesh]); k3d_cb_bind(cb, K3D_BIND_INDEX, sc.ib[mesh]);
    k3d_cb_bind(cb, K3D_BIND_TEXTURE, tex);
    mesh_t* ms[3] = { &m_cube, &m_oct, &m_sphere };
    k3d_cb_draw(cb, 0, ms[mesh]->ni, 0);
}

/* Кадр в момент t_ns. surf должен совпадать с размером сцены; строка (pitch) может быть шире окна. */
static int render_scene(surf_t* t, u64 t_ns) {
    if (!sc.ready || t->w != sc.w || t->h != sc.h) return -1;
    k3d_cmdbuf_t cb; k3d_cb_init(&cb, words, sizeof words / 4);
    float aspect = (float)t->w / (float)t->h;
    k3d_mat4 proj = k3d_mat4_perspective(1.7320508f, aspect, 0.5f, 30.0f);      /* fov 60 градусов по вертикали, камера в начале, смотрит в +z */
    v3 l = {-0.45f, 0.70f, -0.55f};
    float ll = __builtin_sqrtf(v_dot(l, l));
    float ldir[3] = { l.x / ll, l.y / ll, l.z / ll }, lcol[3] = { 0.78f, 0.78f, 0.78f }, lamb[3] = { 0.22f, 0.22f, 0.22f };   /* как у прежнего куба: 0.22 + 0.78 * диффузный */

    k3d_cb_clear(&cb, 0x000A0E22, 0x001F2A55, 1.0f);                           /* фон: вертикальный градиент, глубина на бесконечность */
    k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj);
    k3d_cb_light(&cb, ldir, lcol, lamb);
    const k3d_u32 OPAQUE = K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_DEPTH_WRITE;
    const k3d_u32 TEXLIT = OPAQUE | K3D_TEXTURE | K3D_FILTER_BILINEAR;

    /* центральный куб (кирпичи, цвет граней как раньше, свет по пикселям) */
    float ay = angle_ns(t_ns, 14000000000ULL), ax = angle_ns(t_ns, 23000000000ULL);
    k3d_mat4 ry = k3d_mat4_rot_y(ay), rx = k3d_mat4_rot_x(ax), rot = k3d_mat4_mul(&ry, &rx);
    k3d_mat4 sc125 = k3d_mat4_scale(1.25f, 1.25f, 1.25f), pos = k3d_mat4_translate(0.0f, 0.0f, 6.2f);
    k3d_mat4 m = k3d_mat4_mul(&rot, &sc125); m = k3d_mat4_mul(&pos, &m);
    draw_model(&cb, 0, &m, TEXLIT | K3D_LIGHT_PIXEL, 0xFFFFFFFF, sc.tex[0]);

    /* октаэдр на наклонной орбите: проходит и перед кубом, и за ним (проверка буфера глубины); грани плоские, свет по вершинам */
    float ao = angle_ns(t_ns, 6000000000ULL);
    k3d_mat4 ory = k3d_mat4_rot_y(-ao * 3.0f), orx = k3d_mat4_rot_x(ao * 2.0f), orot = k3d_mat4_mul(&ory, &orx);
    k3d_mat4 os = k3d_mat4_scale(0.75f, 0.75f, 0.75f), opos = k3d_mat4_translate(3.0f * k3d_cosf(ao), 1.1f * k3d_sinf(ao * 2.0f), 6.2f + 2.6f * k3d_sinf(ao));
    m = k3d_mat4_mul(&orot, &os); m = k3d_mat4_mul(&opos, &m);
    draw_model(&cb, 1, &m, OPAQUE | K3D_LIGHT_VERTEX, 0xFFFFFFFF, -1);

    /* планета на встречной орбите: текстура с мип-уровнями, билинейно, свет по вершинам */
    float ap = angle_ns(t_ns, 9000000000ULL) + 3.14159265f, as = angle_ns(t_ns, 11000000000ULL);
    k3d_mat4 pr = k3d_mat4_rot_y(as), ps = k3d_mat4_scale(0.95f, 0.95f, 0.95f), pp = k3d_mat4_translate(-3.3f * k3d_cosf(ap), -1.2f * k3d_sinf(ap), 6.6f + 2.2f * k3d_sinf(ap));
    m = k3d_mat4_mul(&pr, &ps); m = k3d_mat4_mul(&pp, &m);
    draw_model(&cb, 2, &m, TEXLIT | K3D_LIGHT_VERTEX, 0xFFFFFFFF, sc.tex[1]);

    /* стеклянный куб вокруг центрального: прозрачность, глубина проверяется, но не пишется */
    float ag = angle_ns(t_ns, 31000000000ULL), ag2 = angle_ns(t_ns, 17000000000ULL);
    k3d_mat4 gy = k3d_mat4_rot_y(ag), gz = k3d_mat4_rot_z(ag2), gr = k3d_mat4_mul(&gy, &gz), gs = k3d_mat4_scale(2.3f, 2.3f, 2.3f);
    m = k3d_mat4_mul(&gr, &gs); m = k3d_mat4_mul(&pos, &m);
    draw_model(&cb, 0, &m, K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_BLEND | K3D_LIGHT_VERTEX, 0x60A8D8FF, -1);

    if (cb.err) return cb.err;
    int rc = k3d_submit(sc.c, words, cb.n, (k3d_u32*)t->px, t->pitch);
    sc.last = *k3d_stats(sc.c);
    sc.ns_geom += sc.last.ns_geometry; sc.ns_raster += sc.last.ns_raster;
    return rc;
}

/* ---- самопроверка ---- */
static int absf_lt(float a, float b, float tol) { float d = a - b; if (d < 0) d = -d; return d < tol; }

#define ST_W 40
#define ST_H 40
static u64 st_arena[16384] __attribute__((aligned(16)));       /* 128 КБ: движок самопроверки (40x40, 2 помощника) без кучи */

static u32 st_hash(const u32* p, int n) { u32 h = 2166136261u; for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }

/* Прямоугольный треугольник (0,0)-(w,0)-(0,w) в пикселях окна ST_W x ST_H, глубина z; цвет - flat */
static void st_tri(k3d_vertex_t* v, float sz, float z, u32 color, int flip) {
    float px[3] = {0, sz, 0}, py[3] = {0, 0, sz};
    for (int i = 0; i < 3; i++) {
        v[i].x = px[i] / (float)ST_W * 2.0f - 1.0f; v[i].y = 1.0f - py[i] / (float)ST_H * 2.0f; v[i].z = z;
        v[i].nx = v[i].ny = 0.0f; v[i].nz = -1.0f; v[i].u = v[i].v = 0.0f; v[i].color = color;
    }
    if (flip) { k3d_vertex_t t = v[1]; v[1] = v[2]; v[2] = t; }
}

static int selftest(void) {
    prepare_models();
    /* 1. тригонометрия */
    if (!absf_lt(k3d_sinf(0.0f), 0.0f, 1e-6f)) return 1;
    if (!absf_lt(k3d_sinf(3.14159265f / 6.0f), 0.5f, 2e-6f)) return 2;
    if (!absf_lt(k3d_cosf(3.14159265f / 3.0f), 0.5f, 2e-6f)) return 3;
    if (!absf_lt(k3d_cosf(3.14159265f), -1.0f, 2e-6f)) return 4;
    if (!absf_lt(k3d_sinf(4.0f), -0.7568025f, 3e-6f)) return 5;
    if (!absf_lt(k3d_cosf(-2.5f), -0.8011436f, 3e-6f)) return 6;
    /* 2. обход всех граней: лицевая сторона (против часовой для камеры) смотрит наружу, т.е. (b-a)x(c-a) направлено внутрь фигуры */
    mesh_t* ms[3] = { &m_cube, &m_oct, &m_sphere };
    for (int k = 0; k < 3; k++) {
        int degenerate = 0;
        for (u32 i = 0; i < ms[k]->ni; i += 3) {
            v3 a = mesh_pos(&ms[k]->v[ms[k]->i[i]]), b = mesh_pos(&ms[k]->v[ms[k]->i[i + 1]]), c = mesh_pos(&ms[k]->v[ms[k]->i[i + 2]]);
            v3 cr = v_cross(v_sub(b, a), v_sub(c, a)), cen = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
            if (v_dot(cr, cr) < 1e-12f) { degenerate++; continue; }
            if (v_dot(cr, cen) >= 0) return 7 + k;
        }
        if (degenerate > 2 * SPH_S) return 10;
    }
    /* 3. растеризация и буфер глубины на движке 40x40 (память - статический массив): половина квадрата 16x16 = 128 пикселей */
    k3d_config_t cfg; k3d_config_default(&cfg, ST_W, ST_H, 2); k3d_kira_sys(&cfg.sys); cfg.par_min_cost = 0;
    cfg.max_tris = 32; cfg.max_bins = 64; cfg.max_states = 8; cfg.max_verts = 64; cfg.res_bytes = 4096; cfg.stack_bytes = 8192;
    u64 need = k3d_mem_size(&cfg);
    if (!need || need > sizeof st_arena) return 11;
    static u32 pix[ST_W * ST_H], pix2[ST_W * ST_H];
    int rc = 0, cnt = 0;
    for (int pass = 0; pass < 2 && !rc; pass++) {                    /* pass 0: без помощников, pass 1: два помощника - кадр обязан совпасть */
        cfg.workers = pass ? 2 : 0;
        k3d_ctx* c = k3d_create(&cfg, st_arena, sizeof st_arena);
        if (!c) { rc = 12; break; }
        if (pass && k3d_workers(c) != 2) rc = 13;
        k3d_vertex_t v[12]; k3d_u16 idx[12] = {0,1,2, 3,4,5, 6,7,8, 9,10,11};
        st_tri(v, 16.0f, 0.5f, 0x111111, 0); st_tri(v + 3, 16.0f, 0.8f, 0x222222, 0); st_tri(v + 6, 16.0f, 0.2f, 0x333333, 0); st_tri(v + 9, 16.0f, 0.5f, 0x444444, 1);
        if (k3d_vbuf_create(c, v, 12) < 0 || k3d_ibuf_create(c, idx, 12) < 0) rc = 14;
        u32* out = pass ? pix2 : pix;
        k3d_u32 w[64]; k3d_cmdbuf_t cb;
        for (int step = 0; step < 4 && !rc; step++) {
            /* шаг 0: треугольник; шаг 1: дальше - не перекрывает; шаг 2: ближе - перекрывает; шаг 3: другой обход без отбраковки - то же покрытие */
            k3d_cb_init(&cb, w, 64);
            if (step == 0 || step == 3) k3d_cb_clear(&cb, 0, 0, 1.0f);
            k3d_cb_state(&cb, K3D_CULL_NONE | K3D_DEPTH_TEST | K3D_DEPTH_WRITE, 0xFFFFFFFF);
            k3d_cb_bind(&cb, K3D_BIND_VERTEX, 0); k3d_cb_bind(&cb, K3D_BIND_INDEX, 0);
            k3d_cb_draw(&cb, (u32)step * 3, 3, 0);
            if (k3d_submit(c, w, cb.n, out, ST_W)) { rc = 15; break; }
            int n0 = 0, n1 = 0, n2 = 0, n3 = 0;
            for (int i = 0; i < ST_W * ST_H; i++) { u32 p = out[i]; n0 += p == 0x111111; n1 += p == 0x222222; n2 += p == 0x333333; n3 += p == 0x444444; }
            if (step == 0) { cnt = n0; if (cnt < 120 || cnt > 136) rc = 16; }
            if (step == 1 && n1 != 0) rc = 17;                                 /* дальше (глубина больше) - не перекрыл */
            if (step == 2 && n2 != cnt) rc = 18;                               /* ближе - перекрыл всё */
            if (step == 3 && n3 != cnt) rc = 19;                               /* обратный обход - то же покрытие */
        }
        if (!rc) {
            /* 4. текстура и свет: 2x2 текстура на экранном квадрате; билинейно в центре - средний цвет четырёх текселей */
            k3d_u32 t2[4] = { 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000 };
            k3d_vertex_t q[4]; k3d_u16 qi[6] = { 0, 2, 1, 0, 3, 2 };
            for (int i = 0; i < 4; i++) {
                q[i].x = (i == 1 || i == 2) ? 1.0f : -1.0f; q[i].y = i >= 2 ? -1.0f : 1.0f; q[i].z = 0.5f; q[i].nx = q[i].ny = 0.0f; q[i].nz = -1.0f;
                q[i].u = (i == 1 || i == 2) ? 1.0f : 0.0f; q[i].v = i >= 2 ? 1.0f : 0.0f; q[i].color = 0xFFFFFFFF;
            }
            k3d_res_reset(c);
            if (k3d_vbuf_create(c, q, 4) < 0 || k3d_ibuf_create(c, qi, 6) < 0 || k3d_tex_create(c, 2, 2, t2, 0) < 0) { rc = 20; }
            else {
                k3d_cb_init(&cb, w, 64);
                float ld[3] = {0, 0, -1}, lc[3] = {0.5f, 0.5f, 0.5f}, la[3] = {0.5f, 0.5f, 0.5f};
                k3d_cb_clear(&cb, 0, 0, 1.0f); k3d_cb_light(&cb, ld, lc, la);
                k3d_cb_state(&cb, K3D_CULL_NONE | K3D_TEXTURE | K3D_FILTER_BILINEAR | K3D_LIGHT_PIXEL, 0xFFFFFFFF);
                k3d_cb_bind(&cb, K3D_BIND_VERTEX, 0); k3d_cb_bind(&cb, K3D_BIND_INDEX, 0); k3d_cb_bind(&cb, K3D_BIND_TEXTURE, 0); k3d_cb_draw(&cb, 0, 6, 0);
                if (k3d_submit(c, w, cb.n, out, ST_W)) rc = 21;
                else {
                    u32 mid = out[(ST_H / 2) * ST_W + ST_W / 2] & 255;         /* (0 + 255 + 255 + 0) / 4 = 127.5, свет 0.5 + 0.5 * 1 = 1 */
                    if (mid < 124 || mid > 131) rc = 22;
                }
            }
        }
        k3d_destroy(c);
    }
    if (!rc && st_hash(pix, ST_W * ST_H) != st_hash(pix2, ST_W * ST_H)) rc = 23;   /* помощники дают тот же кадр, что и без них */
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

/* Память движка из кучи программы; пробует профили от большого к малому (на машине с малой ОЗУ квота кучи - 1 МБ). */
static void* grab_arena(int workers, u64* bytes_out) {
    static const u32 tris[3] = { 4096, 2048, 1024 }, res[3] = { 1u << 20, 768u << 10, 512u << 10 };
    for (int p = 0; p < 3; p++) {
        k3d_config_t cfg; k3d_config_default(&cfg, W, H, workers); k3d_kira_sys(&cfg.sys);
        cfg.max_tris = tris[p]; cfg.res_bytes = res[p];
        u64 need = k3d_mem_size(&cfg);
        if (!need) return 0;
        u64 pages = (need + 4095) / 4096;
        void* m = k_heap_grow(pages);
        if (m) { *bytes_out = pages * 4096; cfg_tris = tris[p]; cfg_res = res[p]; return m; }
    }
    return 0;
}

static int parse_workers(const char* a, int def) {
    for (; *a; a++) if (a[0] == 't' && a[1] == 'h' && a[2] == 'r' && a[3] == 'e' && a[4] == 'a' && a[5] == 'd' && a[6] == 's' && a[7] == '=' ) {
        int v = 0; const char* p = a + 8;
        if (*p < '0' || *p > '9') return def;
        while (*p >= '0' && *p <= '9' && v < 100) v = v * 10 + (*p++ - '0');
        return v > K3D_MAX_WORKERS ? K3D_MAX_WORKERS : v;
    }
    return def;
}
static int has_word(const char* a, const char* w) {
    u64 n = k_strlen(w);
    for (; *a; a++) if (a[0] == w[0]) { u64 i = 0; while (i < n && a[i] == w[i]) i++; if (i == n && (a[n] == 0 || a[n] == ' ')) return 1; }
    return 0;
}

/* Замер без пауз: фиксированная анимация, N кадров на каждое число помощников. Печатает кадры/с и время фаз. */
static int run_bench(u32* px, void* arena, u64 bytes, int workers) {
    int configs[2] = { 0, workers }, ncfg = workers > 0 ? 2 : 1;
    u64 per[2] = { 0, 0 };
    for (int k = 0; k < ncfg; k++) {
        if (scene_init(arena, bytes, W, H, configs[k]) != 0) { say("cube3d: bench: cannot start the engine\n"); return 1; }
        surf_t s = { px, W, H, W };
        for (int i = 0; i < 4; i++) render_scene(&s, (u64)i * 16666667ULL);            /* прогрев */
        sc.ns_geom = sc.ns_raster = 0;
        enum { N = 60 };
        u64 t0 = k_clock_ns(), tris = 0, pxw = 0;
        for (int i = 0; i < N; i++) { render_scene(&s, (u64)(i + 4) * 16666667ULL); tris += sc.last.tris_drawn; pxw += sc.last.px_written; }
        u64 dt = k_clock_ns() - t0;
        per[k] = dt / N;
        say("cube3d: bench helpers="); say_u((u64)k3d_workers(sc.c)); say(": "); say_u(N); say(" frames, ");
        say_u(dt ? (u64)N * 1000000000ULL / dt : 0); say(" fps, "); say_u(per[k] / 1000); say(" us/frame (geometry "); say_u(sc.ns_geom / N / 1000);
        say(" us, tiles "); say_u(sc.ns_raster / N / 1000); say(" us), "); say_u(tris / N); say(" triangles, "); say_u(pxw / N); say(" px written per frame, ");
        say_u(pxw ? dt / pxw : 0); say(" ns/px, "); say_u(tris ? dt / tris : 0); say(" ns/triangle\n");
        scene_close();
    }
    if (ncfg == 2 && per[1]) { say("cube3d: bench speedup x"); say_u(per[0] * 100 / per[1] / 100); say("."); say_u(per[0] * 100 / per[1] % 100 / 10); say_u(per[0] * 100 / per[1] % 10); say("\n"); }
    return 0;
}

int umain(void) {
    char args[96]; args[0] = 0;
    i64 al = k_args(args, sizeof args - 1);
    if (al > 0) args[al] = 0; else args[0] = 0;
    int workers = parse_workers(args, DEFAULT_WORKERS), bench = has_word(args, "bench");

    int rc = selftest();
    if (rc) { say("cube3d: selftest FAILED, check #"); say_u((u64)rc); say("\n"); return 1; }
    say("cube3d: selftest PASS (trig, face winding, rasterizer, z-test, texture, light, helper threads)\n");

    u32* px = k_gfx_create(W, H);
    if (!px) { say("cube3d: cannot create window\n"); return 1; }
    u64 bytes = 0;
    void* arena = grab_arena(workers, &bytes);                                         /* буфер глубины, треугольники, ресурсы, стеки помощников */
    if (!arena || scene_init(arena, bytes, W, H, workers) != 0) { say("cube3d: cannot allocate the z-buffer and engine memory\n"); return 1; }
    say("cube3d: k3d engine running, "); say_u((u64)k3d_workers(sc.c)); say(" helper threads + main; close the window or press q to exit\n");
    if (bench) { scene_close(); return run_bench(px, arena, bytes, workers); }

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
    say_u(dt ? frames * 100 / dt : 0); say(" fps average, last frame "); say_u(sc.last.tris_drawn);
    say(" triangles, "); say_u(sc.last.px_written); say(" px shaded");
    if (frames) { say(" (average geometry "); say_u(sc.ns_geom / frames / 1000); say(" us, tiles "); say_u(sc.ns_raster / frames / 1000); say(" us per frame)"); }
    say("\n");
    scene_close();
    return 0;
}
