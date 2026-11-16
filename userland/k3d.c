/* k3d: программный 3D-движок (см. k3d.h). Плиточный растеризатор с буфером глубины, перспективно-верной интерполяцией,
   текстурами (билинейно, мип-уровни), отсечением по пирамиде видимости, светом Ламберта, прозрачностью и растеризацией
   плиток несколькими потоками процесса. Только арифметика float/int и атомарные операции: ни libc, ни libm. */
#include "k3d.h"

/* ================= математика ================= */
#define PI_F 3.14159265358979f

float k3d_sqrtf(float x) { return __builtin_sqrtf(x); }

/* sin на [-pi/2, pi/2]: ряд Тейлора до x^11, погрешность < 1e-6 */
static float sin_range(float x) {
    if (x > PI_F * 0.5f) x = PI_F - x;
    else if (x < -PI_F * 0.5f) x = -PI_F - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.6666667e-1f + x2 * (8.3333333e-3f + x2 * (-1.9841270e-4f
             + x2 * (2.7557319e-6f + x2 * -2.5052108e-8f)))));
}
float k3d_sinf(float x) {
    if (!(x < 1e6f && x > -1e6f)) return 0.0f;                     /* NaN, бесконечность и безумные углы */
    while (x > PI_F) x -= 2.0f * PI_F;
    while (x < -PI_F) x += 2.0f * PI_F;
    return sin_range(x);
}
float k3d_cosf(float x) { return k3d_sinf(x + PI_F * 0.5f); }

k3d_mat4 k3d_mat4_identity(void) {
    k3d_mat4 r = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    return r;
}
k3d_mat4 k3d_mat4_mul(const k3d_mat4* a, const k3d_mat4* b) {
    k3d_mat4 r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r.m[i * 4 + j] = a->m[i*4+0] * b->m[0*4+j] + a->m[i*4+1] * b->m[1*4+j] + a->m[i*4+2] * b->m[2*4+j] + a->m[i*4+3] * b->m[3*4+j];
    return r;
}
k3d_mat4 k3d_mat4_translate(float x, float y, float z) { k3d_mat4 r = {{1,0,0,x, 0,1,0,y, 0,0,1,z, 0,0,0,1}}; return r; }
k3d_mat4 k3d_mat4_scale(float x, float y, float z) { k3d_mat4 r = {{x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1}}; return r; }
k3d_mat4 k3d_mat4_rot_x(float a) { float c = k3d_cosf(a), s = k3d_sinf(a); k3d_mat4 r = {{1,0,0,0, 0,c,-s,0, 0,s,c,0, 0,0,0,1}}; return r; }
k3d_mat4 k3d_mat4_rot_y(float a) { float c = k3d_cosf(a), s = k3d_sinf(a); k3d_mat4 r = {{c,0,s,0, 0,1,0,0, -s,0,c,0, 0,0,0,1}}; return r; }
k3d_mat4 k3d_mat4_rot_z(float a) { float c = k3d_cosf(a), s = k3d_sinf(a); k3d_mat4 r = {{c,-s,0,0, s,c,0,0, 0,0,1,0, 0,0,0,1}}; return r; }
k3d_mat4 k3d_mat4_perspective(float f, float aspect, float zn, float zf) {
    float q = zf / (zf - zn);
    k3d_mat4 r = {{f / aspect,0,0,0, 0,f,0,0, 0,0,q,-q * zn, 0,0,1,0}};
    return r;
}

/* ================= внутренние типы ================= */
enum { PL_Z, PL_IW, PL_R, PL_G, PL_B, PL_A, PL_U, PL_V, PL_NX, PL_NY, PL_NZ, NPL };

typedef struct { float x, y, z, w, r, g, b, a, u, v, nx, ny, nz; } cv_t;       /* вершина после света, до деления на w */

typedef struct { k3d_u32 flags, tint; int tex; float ld[3], lc[3], la[3]; } dstate_t;   /* снимок состояния для треугольников */

typedef struct {
    int x0, y0, x1, y1;                  /* охват в пикселях (внутри цели) */
    int ea[3], eb[3]; long long ec[3];   /* рёбра: E = ea*X + eb*Y + ec, X,Y - центр пикселя в 28.4; пиксель внутри, если E >= 0 */
    int a16[3], dq[3], dr[3];            /* шаг E по пикселю (ea*16) и постоянные добавки частного и остатка от строки к строке (раскрой отрезка без делений) */
    float ox, oy;                        /* начало плоскостей: значение в (x - ox, y - oy) */
    k3d_u32 color;                       /* плоский цвет (flat) */
    k3d_u16 st;                          /* снимок состояния */
    k3d_u8 flat, tx0, ty0, tx1, ty1;     /* flat: цвет один на весь треугольник и нет текстуры/света по пикселям */
    float pl[NPL][3];                    /* плоскости: значение в начале, d/dx, d/dy */
} tri_t;

typedef struct { const k3d_u32* mip[K3D_MAX_MIPS]; int w, h, n; } tex_t;
typedef struct { k3d_vertex_t* v; k3d_u32 n; } vbuf_t;
typedef struct { k3d_u16* i; k3d_u32 n; } ibuf_t;

typedef struct { k3d_ctx* c; k3d_u64 px; char pad[48]; } worker_t;               /* pad: не делить кэш-линию */

struct k3d_ctx {
    k3d_config_t cfg;
    int W, H, ntx, nty, ntiles;
    float* zb; tri_t* tris; k3d_u16* bin; k3d_u32* offs; k3d_u32* cursor; cv_t* cvs; k3d_u32* stamp; dstate_t* sts; k3d_u32* rowclr;
    k3d_u8* pool; k3d_u64 pool_cap, pool_used;
    vbuf_t vb[K3D_MAX_RES]; ibuf_t ib[K3D_MAX_RES]; tex_t tx[K3D_MAX_RES]; int nvb, nib, ntx_res;
    /* состояние выполнения потока команд */
    k3d_mat4 mat[3], mvp; float nm[9]; int xf_dirty;
    k3d_u32 flags, tint; float tintf[4]; float ld[3], lc[3], la[3];
    int bvb, bib, btx, cur_st;
    k3d_u32 serial;
    /* пулы треугольников */
    k3d_u32 ntris, nsts, bin_total, has_blend; k3d_u64 work;      /* work: оценка стоимости растеризации накопленного (нс на ядре 3 ГГц) */
    int clear_pending; float clear_depth;
    k3d_u32* target; int pitch;
    k3d_stats_t stats;
    /* потоки */
    int nworkers;
    volatile k3d_u32 gen, done, quit, exited, next_tile;
    worker_t wk[K3D_MAX_WORKERS + 1];
};

/* ================= память ================= */
typedef struct { k3d_u64 ctx, zb, tris, bin, offs, cursor, cvs, stamp, sts, rowclr, pool, stacks, total; } layout_t;
static k3d_u64 al(k3d_u64 x) { return (x + 15) & ~15ULL; }

static int layout(const k3d_config_t* c, layout_t* L) {
    if (!c || c->width < 1 || c->height < 1 || c->width > 4096 || c->height > 4096) return 0;
    if (c->workers < 0 || c->workers > K3D_MAX_WORKERS) return 0;
    if (c->max_tris < 1 || c->max_tris > 65535 || c->max_states < 1 || c->max_states > 65535) return 0;
    if (c->max_verts < 3 || c->max_verts > 65536) return 0;
    k3d_u64 ntiles = (k3d_u64)((c->width + K3D_TILE - 1) / K3D_TILE) * (k3d_u64)((c->height + K3D_TILE - 1) / K3D_TILE);
    if (c->max_bins < ntiles || c->max_bins > (1u << 24)) return 0;
    if (c->workers > 0 && (!c->sys.spawn || !c->sys.futex_wait || !c->sys.futex_wake || c->stack_bytes < 8192)) return 0;
    k3d_u64 o = 0;
    L->ctx = o;     o = al(o + sizeof(struct k3d_ctx));
    L->zb = o;      o = al(o + (k3d_u64)c->width * c->height * 4);
    L->tris = o;    o = al(o + (k3d_u64)c->max_tris * sizeof(tri_t));
    L->bin = o;     o = al(o + (k3d_u64)c->max_bins * 2);
    L->offs = o;    o = al(o + (ntiles + 1) * 4);
    L->cursor = o;  o = al(o + ntiles * 4);
    L->cvs = o;     o = al(o + (k3d_u64)c->max_verts * sizeof(cv_t));
    L->stamp = o;   o = al(o + (k3d_u64)c->max_verts * 4);
    L->sts = o;     o = al(o + (k3d_u64)c->max_states * sizeof(dstate_t));
    L->rowclr = o;  o = al(o + (k3d_u64)c->height * 4);
    L->pool = o;    o = al(o + c->res_bytes);
    L->stacks = o;  o = al(o + (k3d_u64)c->workers * c->stack_bytes);
    L->total = o;
    return 1;
}

void k3d_config_default(k3d_config_t* cfg, int w, int h, int workers) {
    char* p = (char*)cfg; for (unsigned i = 0; i < sizeof *cfg; i++) p[i] = 0;
    cfg->width = w; cfg->height = h; cfg->workers = workers;
    k3d_u32 ntiles = (k3d_u32)((w + K3D_TILE - 1) / K3D_TILE) * (k3d_u32)((h + K3D_TILE - 1) / K3D_TILE);
    cfg->max_tris = 4096; cfg->max_bins = ntiles * 32 > 8192 ? ntiles * 32 : 8192; cfg->max_states = 128;
    cfg->max_verts = 2048; cfg->res_bytes = 1u << 20; cfg->stack_bytes = 16384; cfg->par_min_cost = 200000;
}
k3d_u64 k3d_mem_size(const k3d_config_t* cfg) { layout_t L; return layout(cfg, &L) ? L.total : 0; }

/* ================= команды ================= */
void k3d_cb_init(k3d_cmdbuf_t* cb, k3d_u32* mem, k3d_u32 cap_words) { cb->w = mem; cb->n = 0; cb->cap = cap_words; cb->err = 0; }
void k3d_cb_reset(k3d_cmdbuf_t* cb) { cb->n = 0; cb->err = 0; }
static k3d_u32 fbits(float f) { union { float f; k3d_u32 u; } x; x.f = f; return x.u; }
static float bitsf(k3d_u32 u) { union { float f; k3d_u32 u; } x; x.u = u; return x.f; }
static k3d_u32* cb_alloc(k3d_cmdbuf_t* cb, k3d_u32 op, k3d_u32 len) {
    if (cb->err) return 0;
    if (cb->n + len > cb->cap) { cb->err = K3D_E_FULL; return 0; }
    k3d_u32* p = cb->w + cb->n; cb->n += len; p[0] = (op << 16) | len;
    return p;
}
int k3d_cb_clear(k3d_cmdbuf_t* cb, k3d_u32 top, k3d_u32 bottom, float depth) {
    k3d_u32* p = cb_alloc(cb, K3D_CMD_CLEAR, 4); if (!p) return cb->err;
    p[1] = top; p[2] = bottom; p[3] = fbits(depth); return 0;
}
int k3d_cb_matrix(k3d_cmdbuf_t* cb, int slot, const k3d_mat4* m) {
    if (slot < 0 || slot > 2) return K3D_E_ARG;
    k3d_u32* p = cb_alloc(cb, K3D_CMD_MATRIX, 18); if (!p) return cb->err;
    p[1] = (k3d_u32)slot; for (int i = 0; i < 16; i++) p[2 + i] = fbits(m->m[i]);
    return 0;
}
int k3d_cb_state(k3d_cmdbuf_t* cb, k3d_u32 flags, k3d_u32 tint) {
    k3d_u32* p = cb_alloc(cb, K3D_CMD_STATE, 3); if (!p) return cb->err;
    p[1] = flags; p[2] = tint; return 0;
}
int k3d_cb_light(k3d_cmdbuf_t* cb, const float dir[3], const float color[3], const float ambient[3]) {
    k3d_u32* p = cb_alloc(cb, K3D_CMD_LIGHT, 10); if (!p) return cb->err;
    for (int i = 0; i < 3; i++) { p[1 + i] = fbits(dir[i]); p[4 + i] = fbits(color[i]); p[7 + i] = fbits(ambient[i]); }
    return 0;
}
int k3d_cb_bind(k3d_cmdbuf_t* cb, int kind, int handle) {
    if (kind < 0 || kind > 2) return K3D_E_ARG;
    k3d_u32* p = cb_alloc(cb, K3D_CMD_BIND, 3); if (!p) return cb->err;
    p[1] = (k3d_u32)kind; p[2] = handle < 0 ? K3D_NONE : (k3d_u32)handle; return 0;
}
int k3d_cb_draw(k3d_cmdbuf_t* cb, k3d_u32 first_index, k3d_u32 index_count, k3d_u32 base_vertex) {
    k3d_u32* p = cb_alloc(cb, K3D_CMD_DRAW, 4); if (!p) return cb->err;
    p[1] = first_index; p[2] = index_count; p[3] = base_vertex; return 0;
}
int k3d_cb_validate(const k3d_u32* w, k3d_u32 n) {
    if (!w && n) return K3D_E_BADCMD;
    k3d_u32 i = 0;
    while (i < n) {
        k3d_u32 op = w[i] >> 16, len = w[i] & 0xFFFF, want;
        switch (op) {
        case K3D_CMD_CLEAR: want = 4; break;
        case K3D_CMD_MATRIX: want = 18; break;
        case K3D_CMD_STATE: want = 3; break;
        case K3D_CMD_LIGHT: want = 10; break;
        case K3D_CMD_BIND: want = 3; break;
        case K3D_CMD_DRAW: want = 4; break;
        default: return K3D_E_BADCMD;
        }
        if (len != want || n - i < len) return K3D_E_BADCMD;
        if (op == K3D_CMD_MATRIX && w[i + 1] > 2) return K3D_E_BADCMD;
        if (op == K3D_CMD_BIND && w[i + 1] > 2) return K3D_E_BADCMD;
        if (op == K3D_CMD_DRAW && w[i + 2] % 3) return K3D_E_BADCMD;
        i += len;
    }
    return 0;
}

/* ================= ресурсы ================= */
static void* pool_alloc(k3d_ctx* c, k3d_u64 bytes) {
    k3d_u64 b = al(bytes);
    if (c->pool_used + b > c->pool_cap) return 0;
    void* p = c->pool + c->pool_used; c->pool_used += b; return p;
}
int k3d_vbuf_create(k3d_ctx* c, const k3d_vertex_t* v, k3d_u32 n) {
    if (!c || !v || n < 3 || n > c->cfg.max_verts) return K3D_E_ARG;
    if (c->nvb >= K3D_MAX_RES) return K3D_E_FULL;
    k3d_vertex_t* d = (k3d_vertex_t*)pool_alloc(c, (k3d_u64)n * sizeof *d); if (!d) return K3D_E_NOMEM;
    for (k3d_u32 i = 0; i < n; i++) d[i] = v[i];
    c->vb[c->nvb].v = d; c->vb[c->nvb].n = n; return c->nvb++;
}
int k3d_ibuf_create(k3d_ctx* c, const k3d_u16* idx, k3d_u32 n) {
    if (!c || !idx || n < 3 || n > (1u << 24)) return K3D_E_ARG;
    if (c->nib >= K3D_MAX_RES) return K3D_E_FULL;
    k3d_u16* d = (k3d_u16*)pool_alloc(c, (k3d_u64)n * 2); if (!d) return K3D_E_NOMEM;
    for (k3d_u32 i = 0; i < n; i++) d[i] = idx[i];
    c->ib[c->nib].i = d; c->ib[c->nib].n = n; return c->nib++;
}
static int is_pow2(int x) { return x > 0 && !(x & (x - 1)); }
int k3d_tex_create(k3d_ctx* c, int w, int h, const k3d_u32* px, int mips) {
    if (!c || !px || !is_pow2(w) || !is_pow2(h) || w > 1024 || h > 1024) return K3D_E_ARG;
    if (c->ntx_res >= K3D_MAX_RES) return K3D_E_FULL;
    int n = 1;
    if (mips) { int mw = w, mh = h; while ((mw > 1 || mh > 1) && n < K3D_MAX_MIPS) { mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; n++; } }
    k3d_u64 total = 0; { int mw = w, mh = h; for (int l = 0; l < n; l++) { total += (k3d_u64)mw * mh * 4; mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; } }
    k3d_u32* d = (k3d_u32*)pool_alloc(c, total); if (!d) return K3D_E_NOMEM;
    tex_t* t = &c->tx[c->ntx_res]; t->w = w; t->h = h; t->n = n;
    int mw = w, mh = h; k3d_u32* cur = d;
    for (int i = 0; i < (int)((k3d_u64)mw * mh); i++) cur[i] = px[i];
    t->mip[0] = cur;
    for (int l = 1; l < n; l++) {
        int nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        k3d_u32* nx = cur + (k3d_u64)mw * mh;
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
            int x0 = mw > 1 ? 2 * x : 0, x1 = mw > 1 ? 2 * x + 1 : 0, y0 = mh > 1 ? 2 * y : 0, y1 = mh > 1 ? 2 * y + 1 : 0;
            k3d_u32 p0 = cur[y0 * mw + x0], p1 = cur[y0 * mw + x1], p2 = cur[y1 * mw + x0], p3 = cur[y1 * mw + x1], o = 0;
            for (int s = 0; s < 32; s += 8) o |= (((p0 >> s & 255) + (p1 >> s & 255) + (p2 >> s & 255) + (p3 >> s & 255) + 2) >> 2) << s;
            nx[y * nw + x] = o;
        }
        t->mip[l] = nx; cur = nx; mw = nw; mh = nh;
    }
    return c->ntx_res++;
}
void k3d_res_reset(k3d_ctx* c) { if (!c) return; c->nvb = c->nib = c->ntx_res = 0; c->pool_used = 0; c->bvb = c->bib = c->btx = -1; c->cur_st = -1; }

/* ================= растеризация плитки ================= */
static inline int ifloor(float f) {
    if (f > 1e9f) f = 1e9f; else if (f < -1e9f) f = -1e9f;
    int i = (int)f; return (float)i > f ? i - 1 : i;
}
static inline k3d_u32 lerp8(k3d_u32 c0, k3d_u32 c1, k3d_u32 t) {          /* t: 0..256 */
    k3d_u32 rb = (((c0 & 0x00FF00FFu) * (256 - t) + (c1 & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
    k3d_u32 ag = (((c0 >> 8) & 0x00FF00FFu) * (256 - t) + ((c1 >> 8) & 0x00FF00FFu) * t) & 0xFF00FF00u;
    return rb | ag;
}
static inline int wrap_i(int i, int n, int clamp) { if (clamp) return i < 0 ? 0 : (i >= n ? n - 1 : i); return i & (n - 1); }
static inline k3d_u32 tex_fetch(const k3d_u32* m, int mw, int mh, float u, float v, int bilinear, int clamp) {
    if (!bilinear) {
        int x = wrap_i(ifloor(u * (float)mw), mw, clamp), y = wrap_i(ifloor(v * (float)mh), mh, clamp);
        return m[y * mw + x];
    }
    float fu = u * (float)mw - 0.5f, fv = v * (float)mh - 0.5f;
    int iu = ifloor(fu), iv = ifloor(fv);
    k3d_u32 wu = (k3d_u32)((fu - (float)iu) * 256.0f), wv = (k3d_u32)((fv - (float)iv) * 256.0f);
    if (wu > 255) wu = 255;
    if (wv > 255) wv = 255;
    int x0 = wrap_i(iu, mw, clamp), x1 = wrap_i(iu + 1, mw, clamp), y0 = wrap_i(iv, mh, clamp), y1 = wrap_i(iv + 1, mh, clamp);
    return lerp8(lerp8(m[y0 * mw + x0], m[y0 * mw + x1], wu), lerp8(m[y1 * mw + x0], m[y1 * mw + x1], wu), wv);
}

/* Пол-деление для положительного делителя (округление вниз и для отрицательных чисел). */
static inline long long fdiv(long long n, long long d) { long long q = n / d; return (n - q * d) < 0 ? q - 1 : q; }

static inline int clamp255(float f) { return f > 255.0f ? 255 : (f < 0.0f ? 0 : (int)f); }

/* Закраска одного отрезка строки x0..x1 (строка y) для общего пути. TEX, LIT, BLEND - константы при вызове: компилятор вырезает лишнее
   (always_inline), поэтому в цикле нет проверок режима и лишних шагов интерполяции. */
static inline __attribute__((always_inline)) k3d_u32 shade_span(k3d_ctx* c, const tri_t* t, const dstate_t* s, const tex_t* tx, int y, int x0, int x1,
                                                               const int TEX, const int LIT, const int BLEND) {
    const k3d_u32 fl = s->flags;
    const int depth_test = (fl & K3D_DEPTH_TEST) != 0, depth_write = (fl & K3D_DEPTH_WRITE) != 0, bil = (fl & K3D_FILTER_BILINEAR) != 0, clamp = (fl & K3D_WRAP_CLAMP) != 0;
    k3d_u32* row = c->target + (long)y * c->pitch;
    float* zr = c->zb + (long)y * c->W;
    const float dxs = (float)x0 - t->ox, dys = (float)y - t->oy;
#define PV(I) (t->pl[I][0] + t->pl[I][1] * dxs + t->pl[I][2] * dys)
    float z = PV(PL_Z), iw = PV(PL_IW), cr = PV(PL_R), cg = PV(PL_G), cb = PV(PL_B), ca = BLEND ? PV(PL_A) : 0.0f;
    float u = TEX ? PV(PL_U) : 0.0f, v = TEX ? PV(PL_V) : 0.0f;
    float n0 = LIT ? PV(PL_NX) : 0.0f, n1 = LIT ? PV(PL_NY) : 0.0f, n2 = LIT ? PV(PL_NZ) : 0.0f;
#undef PV
    const float zx = t->pl[PL_Z][1], iwx = t->pl[PL_IW][1], crx = t->pl[PL_R][1], cgx = t->pl[PL_G][1], cbx = t->pl[PL_B][1], cax = t->pl[PL_A][1];
    const float ux = t->pl[PL_U][1], vx = t->pl[PL_V][1], n0x = t->pl[PL_NX][1], n1x = t->pl[PL_NY][1], n2x = t->pl[PL_NZ][1];
    const float luy = t->pl[PL_U][2], lvy = t->pl[PL_V][2], liwy = t->pl[PL_IW][2];       /* всё, что читается в цикле, - в локальные: запись zr[x] (float) не даёт компилятору держать значения из памяти */
    const float ld0 = s->ld[0], ld1 = s->ld[1], ld2 = s->ld[2], la0 = s->la[0], la1 = s->la[1], la2 = s->la[2], lc0 = s->lc[0], lc1 = s->lc[1], lc2 = s->lc[2];
    int lvl = 0; float fw = 0.0f, fh = 0.0f;
    if (TEX) { fw = (float)tx->w; fh = (float)tx->h; }
    k3d_u32 written = 0;
    for (int x = x0; x <= x1; x++) {
        const float zc = z, iwc = iw;
        const float cr0 = cr, cg0 = cg, cb0 = cb, ca0 = ca, u0 = u, v0 = v, m0 = n0, m1 = n1, m2 = n2;
        z += zx; iw += iwx; cr += crx; cg += cgx; cb += cbx;
        if (BLEND) ca += cax;
        if (TEX) { u += ux; v += vx; }
        if (LIT) { n0 += n0x; n1 += n1x; n2 += n2x; }
        if (depth_test && !(zc < zr[x])) continue;
        const float rw = 1.0f / iwc;
        float r = cr0 * rw, g = cg0 * rw, b = cb0 * rw, a = BLEND ? ca0 * rw : 1.0f;
        float tr = 255.0f, tg = 255.0f, tb = 255.0f;
        if (TEX) {
            const float uu = u0 * rw, vv = v0 * rw;
            if (tx->n > 1 && ((x & 3) == 0 || x == x0)) {       /* мип-уровень по производным (du/dx = rw * (dU/dx - u * d(1/w)/dx)); обновляется раз в 4 пикселя */
                const float dudx = rw * (ux - uu * iwx) * fw, dvdx = rw * (vx - vv * iwx) * fh;
                const float dudy = rw * (luy - uu * liwy) * fw, dvdy = rw * (lvy - vv * liwy) * fh;
                float m2 = dudx * dudx + dvdx * dvdx; const float m2y = dudy * dudy + dvdy * dvdy;
                if (m2y > m2) m2 = m2y;
                lvl = 0;
                if (m2 > 1.0f) {
                    union { float f; k3d_u32 i; } q; q.f = m2;
                    int e = (int)((q.i >> 23) & 255) - 127;       /* floor(log2 m2) */
                    lvl = (e + 1) >> 1;                           /* round(log2(m2) / 2) */
                    if (lvl >= tx->n) lvl = tx->n - 1;
                }
            }
            int mw = tx->w >> lvl, mh = tx->h >> lvl; if (mw < 1) mw = 1; if (mh < 1) mh = 1;
            const k3d_u32 tc = tex_fetch(tx->mip[lvl], mw, mh, uu, vv, bil, clamp);
            tr = (float)((tc >> 16) & 255); tg = (float)((tc >> 8) & 255); tb = (float)(tc & 255);
            if (BLEND) a *= (float)(tc >> 24) * (1.0f / 255.0f);
        }
        float lr = 1.0f, lg = 1.0f, lb = 1.0f;
        if (LIT) {
            const float p0 = m0 * rw, p1 = m1 * rw, p2 = m2 * rw;
            const float l2 = p0 * p0 + p1 * p1 + p2 * p2;
            float ndl = 0.0f;
            if (l2 > 1e-20f) { ndl = (p0 * ld0 + p1 * ld1 + p2 * ld2) / __builtin_sqrtf(l2); if (ndl < 0.0f) ndl = 0.0f; }
            lr = la0 + lc0 * ndl; lg = la1 + lc1 * ndl; lb = la2 + lc2 * ndl;
        }
        int ir = clamp255(r * tr * lr + 0.5f), ig = clamp255(g * tg * lg + 0.5f), ib = clamp255(b * tb * lb + 0.5f);
        if (BLEND) {
            const int ia = a >= 1.0f ? 255 : (a <= 0.0f ? 0 : (int)(a * 255.0f + 0.5f));
            if (ia == 0) continue;
            if (ia < 255) {
                const k3d_u32 d = row[x];
                const k3d_u32 mr = (k3d_u32)(ir * ia + (int)((d >> 16) & 255) * (255 - ia) + 128), mg = (k3d_u32)(ig * ia + (int)((d >> 8) & 255) * (255 - ia) + 128),
                              mb = (k3d_u32)(ib * ia + (int)(d & 255) * (255 - ia) + 128);
                ir = (int)((mr + (mr >> 8)) >> 8); ig = (int)((mg + (mg >> 8)) >> 8); ib = (int)((mb + (mb >> 8)) >> 8);
            }
        }
        if (depth_write) zr[x] = zc;
        row[x] = ((k3d_u32)ir << 16) | ((k3d_u32)ig << 8) | (k3d_u32)ib; written++;
    }
    return written;
}

/* Треугольник в пределах плитки. Границы отрезка строки (x0..x1) решаются точно в целых: E + a*k >= 0 для трёх рёбер; от строки к строке
   частное и остаток меняются на постоянные величины (dq, dr считает настройка треугольника), поэтому делений на строку нет. */
static void raster_tri_in_tile(k3d_ctx* c, worker_t* wk, const tri_t* t, int px0, int py0, int px1, int py1) {
    const dstate_t* s = &c->sts[t->st];
    const k3d_u32 fl = s->flags;
    int xa = t->x0 > px0 ? t->x0 : px0, xb = t->x1 < px1 - 1 ? t->x1 : px1 - 1;
    int ya = t->y0 > py0 ? t->y0 : py0, yb = t->y1 < py1 - 1 ? t->y1 : py1 - 1;
    if (xa > xb || ya > yb) return;
    const int depth_test = (fl & K3D_DEPTH_TEST) != 0, depth_write = (fl & K3D_DEPTH_WRITE) != 0;
    const tex_t* tx = (fl & K3D_TEXTURE) && s->tex >= 0 ? &c->tx[s->tex] : 0;
    const int variant = (tx ? 1 : 0) | ((fl & K3D_LIGHT_PIXEL) ? 2 : 0) | ((fl & K3D_BLEND) ? 4 : 0);
    long long q[3] = {0, 0, 0}, r[3] = {0, 0, 0}, e[3] = {0, 0, 0}, bb[3] = {0, 0, 0};
    const long long X = (long long)xa * 16 + 8, Y = (long long)ya * 16 + 8;
    for (int i = 0; i < 3; i++) {
        const long long a = t->a16[i], e0 = (long long)t->ea[i] * X + (long long)t->eb[i] * Y + t->ec[i];
        if (a > 0) { bb[i] = a; q[i] = fdiv(-e0 + a - 1, a); r[i] = (-e0 + a - 1) - q[i] * a; }     /* k >= ceil(-e/a) */
        else if (a < 0) { bb[i] = -a; q[i] = fdiv(e0, -a); r[i] = e0 - q[i] * (-a); }               /* k <= floor(e/(-a)) */
        else e[i] = e0;
    }
    k3d_u64 written = 0;
    const float zx = t->pl[PL_Z][1];
    for (int y = ya; y <= yb; y++) {
        int lo = 0, hi = xb - xa, ok = 1;
        for (int i = 0; i < 3; i++) {
            const long long a = t->a16[i];
            if (a > 0) { if (q[i] > lo) lo = q[i] > hi ? hi + 1 : (int)q[i]; }
            else if (a < 0) { if (q[i] < hi) hi = q[i] < lo ? lo - 1 : (int)q[i]; }
            else if (e[i] < 0) ok = 0;
        }
        if (ok && lo <= hi) {
            const int x0 = xa + lo, x1 = xa + hi;
            if (t->flat) {                                      /* один цвет: только глубина и запись */
                const k3d_u32 col = t->color;
                k3d_u32* row = c->target + (long)y * c->pitch;
                float* zr = c->zb + (long)y * c->W;
                float z = t->pl[PL_Z][0] + zx * ((float)x0 - t->ox) + t->pl[PL_Z][2] * ((float)y - t->oy);
                if (depth_test) {
                    if (depth_write) { for (int x = x0; x <= x1; x++, z += zx) if (z < zr[x]) { zr[x] = z; row[x] = col; written++; } }
                    else { for (int x = x0; x <= x1; x++, z += zx) if (z < zr[x]) { row[x] = col; written++; } }
                } else {
                    for (int x = x0; x <= x1; x++, z += zx) { if (depth_write) zr[x] = z; row[x] = col; written++; }
                }
            } else {
                switch (variant) {
                case 0: written += shade_span(c, t, s, tx, y, x0, x1, 0, 0, 0); break;
                case 1: written += shade_span(c, t, s, tx, y, x0, x1, 1, 0, 0); break;
                case 2: written += shade_span(c, t, s, tx, y, x0, x1, 0, 1, 0); break;
                case 3: written += shade_span(c, t, s, tx, y, x0, x1, 1, 1, 0); break;
                case 4: written += shade_span(c, t, s, tx, y, x0, x1, 0, 0, 1); break;
                case 5: written += shade_span(c, t, s, tx, y, x0, x1, 1, 0, 1); break;
                case 6: written += shade_span(c, t, s, tx, y, x0, x1, 0, 1, 1); break;
                default: written += shade_span(c, t, s, tx, y, x0, x1, 1, 1, 1); break;
                }
            }
        }
        for (int i = 0; i < 3; i++) {                           /* к следующей строке */
            const long long a = t->a16[i];
            if (a == 0) { e[i] += (long long)t->eb[i] * 16; continue; }
            q[i] += t->dq[i]; r[i] += t->dr[i];
            if (r[i] >= bb[i]) { r[i] -= bb[i]; q[i]++; }
        }
    }
    wk->px += written;
}

static void run_tile(k3d_ctx* c, worker_t* wk, int tile) {
    const int tx = tile % c->ntx, ty = tile / c->ntx;
    const int px0 = tx * K3D_TILE, py0 = ty * K3D_TILE;
    const int px1 = px0 + K3D_TILE < c->W ? px0 + K3D_TILE : c->W, py1 = py0 + K3D_TILE < c->H ? py0 + K3D_TILE : c->H;
    const k3d_u32 b0 = c->offs[tile], b1 = c->offs[tile + 1];
    if (c->clear_pending) {
        const float cd = c->clear_depth; k3d_u32 cdb; __builtin_memcpy(&cdb, &cd, 4);                                    /* в локальную: запись в zr (float) могла бы изменить поле c - компилятор перечитывал бы его на каждом пикселе */
        for (int y = py0; y < py1; y++) {
            k3d_u32* row = c->target + (long)y * c->pitch; float* zr = c->zb + (long)y * c->W; const k3d_u32 col = c->rowclr[y];
            if (px1 - px0 == K3D_TILE) {                                   /* целая строка плитки: записи по 8 байт (два пикселя за раз) */
                k3d_u32* rp = row + px0; float* zp = zr + px0;
                const k3d_u64 c2 = ((k3d_u64)col << 32) | col, z2 = ((k3d_u64)cdb << 32) | cdb;
                for (int x = 0; x < K3D_TILE; x += 2) { __builtin_memcpy(rp + x, &c2, 8); __builtin_memcpy(zp + x, &z2, 8); }
            } else for (int x = px0; x < px1; x++) { row[x] = col; zr[x] = cd; }
        }
    }
    if (b0 == b1) return;
    for (int pass = 0; pass < (c->has_blend ? 2 : 1); pass++) {          /* сначала непрозрачные, затем прозрачные, каждые - в порядке подачи */
        for (k3d_u32 i = b0; i < b1; i++) {
            const tri_t* t = &c->tris[c->bin[i]];
            if (c->has_blend && ((c->sts[t->st].flags & K3D_BLEND) != 0) != (pass == 1)) continue;
            raster_tri_in_tile(c, wk, t, px0, py0, px1, py1);
        }
    }
}

static void run_job(k3d_ctx* c, worker_t* wk) {
    for (;;) {
        k3d_u32 t = __atomic_fetch_add(&c->next_tile, 1, __ATOMIC_ACQ_REL);
        if (t >= (k3d_u32)c->ntiles) break;
        run_tile(c, wk, (int)t);
    }
}

static void worker_main(k3d_u64 arg) {
    worker_t* wk = (worker_t*)(unsigned long)arg;
    k3d_ctx* c = wk->c;
    k3d_u32 seen = 0;
    for (;;) {
        k3d_u32 g;
        while ((g = __atomic_load_n(&c->gen, __ATOMIC_ACQUIRE)) == seen) c->cfg.sys.futex_wait(&c->gen, seen);
        seen = g;
        if (__atomic_load_n(&c->quit, __ATOMIC_ACQUIRE)) break;
        run_job(c, wk);
        if (__atomic_add_fetch(&c->done, 1, __ATOMIC_ACQ_REL) == (k3d_u32)c->nworkers) c->cfg.sys.futex_wake(&c->done, 1);
    }
    void (*wake)(volatile k3d_u32*, k3d_u32) = c->cfg.sys.futex_wake;   /* после увеличения exited движок могут освободить: c больше не трогаем */
    volatile k3d_u32* ex = &c->exited;
    __atomic_add_fetch(ex, 1, __ATOMIC_ACQ_REL);
    wake(ex, 1);
}

static k3d_u64 now_ns(k3d_ctx* c) { return c->cfg.sys.clock_ns ? c->cfg.sys.clock_ns() : 0; }

/* ================= создание ================= */
k3d_ctx* k3d_create(const k3d_config_t* cfg, void* mem, k3d_u64 bytes) {
    layout_t L;
    if (!layout(cfg, &L) || !mem || ((unsigned long)mem & 15) || bytes < L.total) return 0;
    char* base = (char*)mem;
    k3d_ctx* c = (k3d_ctx*)(base + L.ctx);
    { char* p = (char*)c; for (unsigned i = 0; i < sizeof *c; i++) p[i] = 0; }
    c->cfg = *cfg; c->W = cfg->width; c->H = cfg->height;
    c->ntx = (c->W + K3D_TILE - 1) / K3D_TILE; c->nty = (c->H + K3D_TILE - 1) / K3D_TILE; c->ntiles = c->ntx * c->nty;
    c->zb = (float*)(base + L.zb); c->tris = (tri_t*)(base + L.tris); c->bin = (k3d_u16*)(base + L.bin);
    c->offs = (k3d_u32*)(base + L.offs); c->cursor = (k3d_u32*)(base + L.cursor); c->cvs = (cv_t*)(base + L.cvs);
    c->stamp = (k3d_u32*)(base + L.stamp); c->sts = (dstate_t*)(base + L.sts); c->rowclr = (k3d_u32*)(base + L.rowclr);
    c->pool = (k3d_u8*)(base + L.pool); c->pool_cap = cfg->res_bytes;
    for (k3d_u32 i = 0; i < cfg->max_verts; i++) c->stamp[i] = 0;
    for (long i = 0; i < (long)c->W * c->H; i++) c->zb[i] = 1.0f;
    for (int i = 0; i < c->H; i++) c->rowclr[i] = 0;
    c->bvb = c->bib = c->btx = -1; c->cur_st = -1;
    for (int i = 0; i < K3D_MAX_WORKERS + 1; i++) c->wk[i].c = c;
    /* потоки-помощники */
    int started = 0;
    for (int i = 0; i < cfg->workers; i++) {
        if (cfg->sys.spawn(worker_main, base + L.stacks + (k3d_u64)i * cfg->stack_bytes, cfg->stack_bytes, (k3d_u64)(unsigned long)&c->wk[i]) != 0) break;
        started++;
    }
    c->nworkers = started;
    return c;
}

int k3d_workers(const k3d_ctx* c) { return c ? c->nworkers : 0; }

void k3d_destroy(k3d_ctx* c) {
    if (!c || !c->nworkers) return;
    __atomic_store_n(&c->quit, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&c->gen, 1, __ATOMIC_ACQ_REL);
    c->cfg.sys.futex_wake(&c->gen, 0xFFFFFFFFu);
    for (;;) {
        k3d_u32 e = __atomic_load_n(&c->exited, __ATOMIC_ACQUIRE);
        if (e == (k3d_u32)c->nworkers) break;
        c->cfg.sys.futex_wait(&c->exited, e);
    }
    c->nworkers = 0;
}

/* ================= сброс плиток: раскладка по плиткам и параллельная растеризация ================= */
static void flush(k3d_ctx* c) {
    if (!c->ntris && !c->clear_pending) { c->cur_st = -1; c->nsts = 0; return; }
    const int nt = c->ntiles;
    k3d_u64 t1;
    for (int i = 0; i <= nt; i++) c->offs[i] = 0;
    for (k3d_u32 i = 0; i < c->ntris; i++) {
        const tri_t* t = &c->tris[i];
        for (int ty = t->ty0; ty <= t->ty1; ty++) for (int tx = t->tx0; tx <= t->tx1; tx++) c->offs[ty * c->ntx + tx + 1]++;
    }
    for (int i = 0; i < nt; i++) { c->offs[i + 1] += c->offs[i]; c->cursor[i] = c->offs[i]; }
    for (k3d_u32 i = 0; i < c->ntris; i++) {                            /* в порядке подачи: порядок внутри плитки сохраняется */
        const tri_t* t = &c->tris[i];
        for (int ty = t->ty0; ty <= t->ty1; ty++) for (int tx = t->tx0; tx <= t->tx1; tx++) c->bin[c->cursor[ty * c->ntx + tx]++] = (k3d_u16)i;
    }
    c->stats.bin_entries += c->offs[nt];
    t1 = now_ns(c);

    worker_t* me = &c->wk[c->nworkers];
    me->px = 0; for (int i = 0; i < c->nworkers; i++) c->wk[i].px = 0;
    c->next_tile = 0; c->done = 0;
    const int par = c->nworkers && (c->work + (c->clear_pending ? (k3d_u64)c->W * c->H * 2 / 5 : 0) >= c->cfg.par_min_cost);
    if (par) {
        __atomic_add_fetch(&c->gen, 1, __ATOMIC_ACQ_REL);
        c->cfg.sys.futex_wake(&c->gen, 0xFFFFFFFFu);
    }
    run_job(c, me);
    if (par) {
        for (;;) {
            k3d_u32 d = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
            if (d == (k3d_u32)c->nworkers) break;
            c->cfg.sys.futex_wait(&c->done, d);
        }
    }
    k3d_u32 px = 0; for (int i = 0; i <= c->nworkers; i++) px += (k3d_u32)c->wk[i].px;
    c->stats.px_written += px; c->stats.flushes++; c->stats.flushes_parallel += par;
    c->stats.ns_raster += now_ns(c) - t1;
    c->ntris = 0; c->nsts = 0; c->bin_total = 0; c->has_blend = 0; c->clear_pending = 0; c->cur_st = -1; c->work = 0;
}

/* ================= геометрия ================= */
static void prepare_transform(k3d_ctx* c) {
    if (!c->xf_dirty) return;
    k3d_mat4 vm = k3d_mat4_mul(&c->mat[K3D_SLOT_VIEW], &c->mat[K3D_SLOT_MODEL]);
    c->mvp = k3d_mat4_mul(&c->mat[K3D_SLOT_PROJ], &vm);
    /* матрица нормалей: алгебраические дополнения верхней 3x3 модели (= det * обратная транспонированная), со знаком определителя */
    const float* m = c->mat[K3D_SLOT_MODEL].m;
    float a = m[0], b = m[1], cc = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float cof[9] = { e*i - f*h, f*g - d*i, d*h - e*g,
                     cc*h - b*i, a*i - cc*g, b*g - a*h,
                     b*f - cc*e, cc*d - a*f, a*e - b*d };
    float det = a * cof[0] + b * cof[1] + cc * cof[2];
    float sg = det < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 9; k++) c->nm[k] = cof[k] * sg;
    c->xf_dirty = 0;
}

static void shade_vertex(const k3d_ctx* c, const k3d_vertex_t* v, cv_t* o) {
    const float* m = c->mvp.m;
    o->x = m[0] * v->x + m[1] * v->y + m[2] * v->z + m[3];
    o->y = m[4] * v->x + m[5] * v->y + m[6] * v->z + m[7];
    o->z = m[8] * v->x + m[9] * v->y + m[10] * v->z + m[11];
    o->w = m[12] * v->x + m[13] * v->y + m[14] * v->z + m[15];
    const float k = 1.0f / 255.0f;
    o->r = (float)((v->color >> 16) & 255) * k * c->tintf[0]; o->g = (float)((v->color >> 8) & 255) * k * c->tintf[1];
    o->b = (float)(v->color & 255) * k * c->tintf[2]; o->a = (float)(v->color >> 24) * k * c->tintf[3];
    o->u = v->u; o->v = v->v; o->nx = o->ny = o->nz = 0.0f;
    if (c->flags & (K3D_LIGHT_VERTEX | K3D_LIGHT_PIXEL)) {
        const float* n = c->nm;
        float nx = n[0] * v->nx + n[1] * v->ny + n[2] * v->nz, ny = n[3] * v->nx + n[4] * v->ny + n[5] * v->nz, nz = n[6] * v->nx + n[7] * v->ny + n[8] * v->nz;
        float l2 = nx * nx + ny * ny + nz * nz;
        if (l2 > 1e-20f) { float inv = 1.0f / __builtin_sqrtf(l2); nx *= inv; ny *= inv; nz *= inv; } else { nx = ny = nz = 0.0f; }
        if (c->flags & K3D_LIGHT_VERTEX) {
            float ndl = nx * c->ld[0] + ny * c->ld[1] + nz * c->ld[2]; if (ndl < 0.0f) ndl = 0.0f;
            o->r *= c->la[0] + c->lc[0] * ndl; o->g *= c->la[1] + c->lc[1] * ndl; o->b *= c->la[2] + c->lc[2] * ndl;
        }
        if (c->flags & K3D_LIGHT_PIXEL) { o->nx = nx; o->ny = ny; o->nz = nz; }
    }
}

/* Снимок состояния для треугольников (общий у подряд идущих DRAW с одним состоянием). */
static void ensure_state(k3d_ctx* c) {
    if (c->cur_st >= 0) return;
    if (c->nsts >= c->cfg.max_states) flush(c);
    dstate_t* s = &c->sts[c->nsts];
    s->flags = c->flags; s->tint = c->tint; s->tex = c->btx;
    for (int i = 0; i < 3; i++) { s->ld[i] = c->ld[i]; s->lc[i] = c->lc[i]; s->la[i] = c->la[i]; }
    c->cur_st = (int)c->nsts++;
}

static void setup_tri(k3d_ctx* c, const cv_t* v0, const cv_t* v1, const cv_t* v2) {
    const cv_t* v[3] = { v0, v1, v2 };
    float sx[3], sy[3], sz[3], iw[3];
    int ix[3], iy[3];
    const int maxx = c->W * 16, maxy = c->H * 16;
    for (int i = 0; i < 3; i++) {
        if (!(v[i]->w > 1e-9f)) { c->stats.tris_outside++; return; }
        iw[i] = 1.0f / v[i]->w;
        sx[i] = (v[i]->x * iw[i] * 0.5f + 0.5f) * (float)c->W;
        sy[i] = (0.5f - v[i]->y * iw[i] * 0.5f) * (float)c->H;
        sz[i] = v[i]->z * iw[i];
        if (!(sx[i] > -16384.0f && sx[i] < 16384.0f && sy[i] > -16384.0f && sy[i] < 16384.0f)) { c->stats.tris_outside++; return; }   /* NaN, w = 0 */
        int qx = (int)(sx[i] * 16.0f + 0.5f), qy = (int)(sy[i] * 16.0f + 0.5f);       /* 28.4; после отсечения координаты >= -eps */
        ix[i] = qx < 0 ? 0 : (qx > maxx ? maxx : qx); iy[i] = qy < 0 ? 0 : (qy > maxy ? maxy : qy);
    }
    long long area2 = (long long)(ix[1] - ix[0]) * (iy[2] - iy[0]) - (long long)(iy[1] - iy[0]) * (ix[2] - ix[0]);
    if (area2 == 0) { c->stats.tris_outside++; return; }
    int front = area2 < 0;                       /* y на экране вниз: лицевая (против часовой, y вверх) даёт отрицательную площадь */
    int cull = c->flags & 3;
    if ((cull == K3D_CULL_BACK && !front) || (cull == K3D_CULL_FRONT && front)) { c->stats.tris_culled++; return; }
    if (area2 < 0) {                             /* единая ориентация: площадь > 0, внутри E >= 0 */
        const cv_t* tv = v[1]; v[1] = v[2]; v[2] = tv;
        float tf = sx[1]; sx[1] = sx[2]; sx[2] = tf; tf = sy[1]; sy[1] = sy[2]; sy[2] = tf; tf = sz[1]; sz[1] = sz[2]; sz[2] = tf; tf = iw[1]; iw[1] = iw[2]; iw[2] = tf;
        int ti = ix[1]; ix[1] = ix[2]; ix[2] = ti; ti = iy[1]; iy[1] = iy[2]; iy[2] = ti;
        area2 = -area2;
    }
    int minx = ix[0] < ix[1] ? (ix[0] < ix[2] ? ix[0] : ix[2]) : (ix[1] < ix[2] ? ix[1] : ix[2]);
    int maxx2 = ix[0] > ix[1] ? (ix[0] > ix[2] ? ix[0] : ix[2]) : (ix[1] > ix[2] ? ix[1] : ix[2]);
    int miny = iy[0] < iy[1] ? (iy[0] < iy[2] ? iy[0] : iy[2]) : (iy[1] < iy[2] ? iy[1] : iy[2]);
    int maxy2 = iy[0] > iy[1] ? (iy[0] > iy[2] ? iy[0] : iy[2]) : (iy[1] > iy[2] ? iy[1] : iy[2]);
    int x0 = minx >> 4, x1 = maxx2 >> 4, y0 = miny >> 4, y1 = maxy2 >> 4;
    if (x1 > c->W - 1) x1 = c->W - 1;
    if (y1 > c->H - 1) y1 = c->H - 1;
    if (x0 > x1 || y0 > y1) { c->stats.tris_outside++; return; }
    int tx0 = x0 / K3D_TILE, tx1 = x1 / K3D_TILE, ty0 = y0 / K3D_TILE, ty1 = y1 / K3D_TILE;
    k3d_u32 entries = (k3d_u32)((tx1 - tx0 + 1) * (ty1 - ty0 + 1));
    if (c->ntris >= c->cfg.max_tris || c->bin_total + entries > c->cfg.max_bins) flush(c);
    ensure_state(c);                                                     /* может сбросить сам (мало снимков): пулы после сброса пусты */

    tri_t* t = &c->tris[c->ntris];
    t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1;
    t->tx0 = (k3d_u8)tx0; t->tx1 = (k3d_u8)tx1; t->ty0 = (k3d_u8)ty0; t->ty1 = (k3d_u8)ty1;
    t->st = (k3d_u16)c->cur_st;
    for (int i = 0; i < 3; i++) {
        int j = i == 2 ? 0 : i + 1;
        long long dx = ix[j] - ix[i], dy = iy[j] - iy[i];
        t->ea[i] = (int)-dy; t->eb[i] = (int)dx;
        int topleft = (-dy > 0) || (-dy == 0 && dx > 0);                 /* на ребре "верх" или "лево" пиксель принадлежит треугольнику, иначе нет */
        t->ec[i] = dy * ix[i] - dx * iy[i] + (topleft ? 0 : -1);
        long long a = (long long)t->ea[i] * 16, eb16 = (long long)t->eb[i] * 16;
        t->a16[i] = (int)a; t->dq[i] = 0; t->dr[i] = 0;
        if (a > 0) { long long dm = -eb16, q = fdiv(dm, a); t->dq[i] = (int)q; t->dr[i] = (int)(dm - q * a); }
        else if (a < 0) { long long q = fdiv(eb16, -a); t->dq[i] = (int)q; t->dr[i] = (int)(eb16 - q * (-a)); }
    }
    const float fx0 = (float)ix[0] * (1.0f / 16.0f), fy0 = (float)iy[0] * (1.0f / 16.0f);
    t->ox = fx0 - 0.5f; t->oy = fy0 - 0.5f;
    const float dx1 = (float)(ix[1] - ix[0]) * (1.0f / 16.0f), dy1 = (float)(iy[1] - iy[0]) * (1.0f / 16.0f);
    const float dx2 = (float)(ix[2] - ix[0]) * (1.0f / 16.0f), dy2 = (float)(iy[2] - iy[0]) * (1.0f / 16.0f);
    const float idet = 1.0f / (dx1 * dy2 - dx2 * dy1);
#define PLANE(I, Q0, Q1, Q2) do { float q0_ = (Q0), d1_ = (Q1) - q0_, d2_ = (Q2) - q0_; \
        t->pl[I][0] = q0_; t->pl[I][1] = (d1_ * dy2 - d2_ * dy1) * idet; t->pl[I][2] = (d2_ * dx1 - d1_ * dx2) * idet; } while (0)
    PLANE(PL_Z, sz[0], sz[1], sz[2]);
    const k3d_u32 fl = c->flags;
    const int textured = (fl & K3D_TEXTURE) != 0, lit_px = (fl & K3D_LIGHT_PIXEL) != 0, blend = (fl & K3D_BLEND) != 0;
    t->flat = 0;
    if (!textured && !lit_px && !blend && v[0]->r == v[1]->r && v[0]->r == v[2]->r && v[0]->g == v[1]->g && v[0]->g == v[2]->g && v[0]->b == v[1]->b && v[0]->b == v[2]->b) {
        float fr = v[0]->r * 255.0f + 0.5f, fg = v[0]->g * 255.0f + 0.5f, fb = v[0]->b * 255.0f + 0.5f;
        int ir = fr > 255.0f ? 255 : (int)fr, ig = fg > 255.0f ? 255 : (int)fg, ib = fb > 255.0f ? 255 : (int)fb;
        t->color = ((k3d_u32)ir << 16) | ((k3d_u32)ig << 8) | (k3d_u32)ib; t->flat = 1;
    } else {
        PLANE(PL_IW, iw[0], iw[1], iw[2]);
        PLANE(PL_R, v[0]->r * iw[0], v[1]->r * iw[1], v[2]->r * iw[2]);
        PLANE(PL_G, v[0]->g * iw[0], v[1]->g * iw[1], v[2]->g * iw[2]);
        PLANE(PL_B, v[0]->b * iw[0], v[1]->b * iw[1], v[2]->b * iw[2]);
        PLANE(PL_A, v[0]->a * iw[0], v[1]->a * iw[1], v[2]->a * iw[2]);
        if (textured) {
            PLANE(PL_U, v[0]->u * iw[0], v[1]->u * iw[1], v[2]->u * iw[2]);
            PLANE(PL_V, v[0]->v * iw[0], v[1]->v * iw[1], v[2]->v * iw[2]);
        }
        if (lit_px) {
            PLANE(PL_NX, v[0]->nx * iw[0], v[1]->nx * iw[1], v[2]->nx * iw[2]);
            PLANE(PL_NY, v[0]->ny * iw[0], v[1]->ny * iw[1], v[2]->ny * iw[2]);
            PLANE(PL_NZ, v[0]->nz * iw[0], v[1]->nz * iw[1], v[2]->nz * iw[2]);
        }
    }
#undef PLANE
    if (blend) c->has_blend = 1;
    c->work += (k3d_u64)(x1 - x0 + 1) * (k3d_u64)(y1 - y0 + 1) / 2 * (t->flat ? 2u : (textured || lit_px ? 18u : 8u));   /* половина охвата закрашена; цена пикселя по режиму */
    c->ntris++; c->bin_total += entries; c->stats.tris_drawn++;
}

/* Плоскости пирамиды видимости в однородных координатах: расстояние >= 0 - внутри. */
static inline float plane_dist(const cv_t* v, int p) {
    switch (p) {
    case 0: return v->w + v->x;
    case 1: return v->w - v->x;
    case 2: return v->w + v->y;
    case 3: return v->w - v->y;
    case 4: return v->z;
    default: return v->w - v->z;
    }
}
static inline unsigned outcode(const cv_t* v) {
    unsigned o = 0;
    for (int p = 0; p < 6; p++) if (plane_dist(v, p) < 0.0f) o |= 1u << p;
    return o;
}
static void lerp_cv(cv_t* o, const cv_t* a, const cv_t* b, float t) {
    const float* pa = &a->x; const float* pb = &b->x; float* po = &o->x;
    for (int i = 0; i < 13; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static void draw_tri(k3d_ctx* c, const cv_t* a, const cv_t* b, const cv_t* d) {
    unsigned oa = outcode(a), ob = outcode(b), od = outcode(d);
    c->stats.tris_in++;
    if (oa & ob & od) { c->stats.tris_outside++; return; }
    if (!(oa | ob | od)) { setup_tri(c, a, b, d); return; }
    c->stats.tris_clipped++;
    cv_t buf[2][10];
    int n = 3, cur = 0;
    buf[0][0] = *a; buf[0][1] = *b; buf[0][2] = *d;
    for (int p = 0; p < 6 && n >= 3; p++) {
        int m = 0;
        cv_t* in = buf[cur]; cv_t* out = buf[cur ^ 1];
        for (int i = 0; i < n; i++) {
            const cv_t* s = &in[i]; const cv_t* e = &in[i + 1 == n ? 0 : i + 1];
            float ds = plane_dist(s, p), de = plane_dist(e, p);
            if (ds >= 0.0f) out[m++] = *s;
            if ((ds >= 0.0f) != (de >= 0.0f)) lerp_cv(&out[m++], s, e, ds / (ds - de));
        }
        n = m; cur ^= 1;
    }
    if (n < 3) { c->stats.tris_outside++; return; }
    for (int i = 1; i + 1 < n; i++) setup_tri(c, &buf[cur][0], &buf[cur][i], &buf[cur][i + 1]);
}

static int exec_draw(k3d_ctx* c, k3d_u32 first, k3d_u32 count, k3d_u32 base) {
    if (c->bvb < 0 || c->bib < 0) return K3D_E_HANDLE;
    const vbuf_t* vb = &c->vb[c->bvb]; const ibuf_t* ib = &c->ib[c->bib];
    if ((k3d_u64)first + count > ib->n) return K3D_E_RANGE;
    if ((c->flags & K3D_TEXTURE) && c->btx < 0) return K3D_E_HANDLE;
    for (k3d_u32 i = 0; i < count; i++) if ((k3d_u64)ib->i[first + i] + base >= vb->n) return K3D_E_RANGE;   /* проверка до рисования: всё или ничего */
    prepare_transform(c);
    c->serial++;
    for (k3d_u32 k = 0; k < count; k += 3) {
        const cv_t* tv[3];
        for (int j = 0; j < 3; j++) {
            k3d_u32 vi = ib->i[first + k + j] + base;
            if (c->stamp[vi] != c->serial) { shade_vertex(c, &vb->v[vi], &c->cvs[vi]); c->stamp[vi] = c->serial; }
            tv[j] = &c->cvs[vi];
        }
        draw_tri(c, tv[0], tv[1], tv[2]);
    }
    return 0;
}

static void reset_exec(k3d_ctx* c) {
    for (int i = 0; i < 3; i++) c->mat[i] = k3d_mat4_identity();
    c->xf_dirty = 1; c->flags = K3D_STATE_DEFAULT; c->tint = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) c->tintf[i] = 1.0f;
    for (int i = 0; i < 3; i++) { c->ld[i] = 0.0f; c->lc[i] = 0.0f; c->la[i] = 1.0f; }
    c->ld[2] = 1.0f;
    c->bvb = c->bib = c->btx = -1; c->cur_st = -1;
    c->stats.tris_in = c->stats.tris_culled = c->stats.tris_outside = c->stats.tris_clipped = c->stats.tris_drawn = 0;
    c->stats.flushes = c->stats.flushes_parallel = c->stats.bin_entries = c->stats.px_written = 0; c->stats.ns_geometry = c->stats.ns_raster = 0;
}

int k3d_submit(k3d_ctx* c, const k3d_u32* w, k3d_u32 n, k3d_u32* target, int pitch) {
    if (!c || !target || pitch < c->W) return K3D_E_ARG;
    int rc = k3d_cb_validate(w, n);
    if (rc) return rc;
    c->target = target; c->pitch = pitch;
    reset_exec(c);
    k3d_u64 t0 = now_ns(c);
    k3d_u32 i = 0;
    while (i < n && !rc) {
        k3d_u32 len = w[i] & 0xFFFF; const k3d_u32* p = w + i;
        switch (p[0] >> 16) {
        case K3D_CMD_CLEAR: {
            flush(c);                                                   /* то, что нарисовано раньше, должно дойти до цели до очистки */
            k3d_u32 top = p[1], bot = p[2];
            for (int y = 0; y < c->H; y++) {
                k3d_u32 f = c->H > 1 ? (k3d_u32)y * 256u / (k3d_u32)(c->H - 1) : 0, col = 0;
                for (int s = 0; s < 24; s += 8) {
                    k3d_u32 a = top >> s & 255, b = bot >> s & 255;
                    col |= (((a * (256 - f) + b * f) >> 8) & 255) << s;
                }
                c->rowclr[y] = col & 0x00FFFFFFu;
            }
            c->clear_depth = bitsf(p[3]); c->clear_pending = 1;
            break;
        }
        case K3D_CMD_MATRIX:
            for (int k = 0; k < 16; k++) c->mat[p[1]].m[k] = bitsf(p[2 + k]);
            c->xf_dirty = 1; break;
        case K3D_CMD_STATE:
            c->flags = p[1]; c->tint = p[2];
            for (int k = 0; k < 4; k++) c->tintf[k] = (float)((p[2] >> (k == 3 ? 24 : 16 - 8 * k)) & 255) * (1.0f / 255.0f);
            c->cur_st = -1; break;
        case K3D_CMD_LIGHT: {
            float d[3] = { bitsf(p[1]), bitsf(p[2]), bitsf(p[3]) };
            float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (l2 > 1e-20f) { float inv = 1.0f / __builtin_sqrtf(l2); for (int k = 0; k < 3; k++) c->ld[k] = d[k] * inv; }
            for (int k = 0; k < 3; k++) { c->lc[k] = bitsf(p[4 + k]); c->la[k] = bitsf(p[7 + k]); }
            c->cur_st = -1; break;
        }
        case K3D_CMD_BIND: {
            int h = p[2] == K3D_NONE ? -1 : (p[2] > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)p[2]);
            if (p[1] == K3D_BIND_VERTEX) { if (h >= c->nvb) rc = K3D_E_HANDLE; else c->bvb = h; }
            else if (p[1] == K3D_BIND_INDEX) { if (h >= c->nib) rc = K3D_E_HANDLE; else c->bib = h; }
            else { if (h >= c->ntx_res) rc = K3D_E_HANDLE; else { c->btx = h; c->cur_st = -1; } }
            break;
        }
        case K3D_CMD_DRAW: rc = exec_draw(c, p[1], p[2], p[3]); break;
        }
        i += len;
    }
    flush(c);
    c->stats.ns_geometry = now_ns(c) - t0 - c->stats.ns_raster;           /* вершины, отсечение, настройка, раскладка по плиткам */
    c->stats.frames++;
    return rc;
}

const k3d_stats_t* k3d_stats(const k3d_ctx* c) { return &c->stats; }
const float* k3d_depth(const k3d_ctx* c) { return c->zb; }
