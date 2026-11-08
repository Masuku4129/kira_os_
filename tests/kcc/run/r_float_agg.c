/* Массивы, структуры и объединения с float/double: инициализаторы, поля, указатели, вложенные структуры. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
typedef struct { double x, y; } V2;
typedef struct { float x, y, z; } V3;
typedef struct { int id; double w; float f; char c; } Mixed;
typedef struct { double m[3][3]; } M3;
typedef struct { V2 a, b; } Seg;
static double gd[4] = { 1.5, -2.5, 1e100, 0.1 };
static float gf[] = { 1, 2.5f, -3, 0.1f };
static V2 gv = { 3, 4 };
static Mixed gm[2] = { { 1, 2.5, 3.5f, 'a' }, { 2, -1e-3, 0.25f, 'b' } };
static double zero_init[5];
static void vadd(V2 *a, V2 *b, V2 *o) { V2 r = { a->x + b->x, a->y + b->y }; *o = r; }
static void vscale(V2 *a, double k) { a->x *= k; a->y *= k; }
static double vdot(V2 *a, V2 *b) { return a->x * b->x + a->y * b->y; }
static void v3cross(V3 *a, V3 *b, V3 *r) { V3 t = { a->y * b->z - a->z * b->y, a->z * b->x - a->x * b->z, a->x * b->y - a->y * b->x }; *r = t; }
static void mkmixed(Mixed *m, int i, double d) { m->id = i; m->w = d; m->f = (float)d; m->c = 'z'; }
static double mlen2(Mixed *m) { return m->id + m->w + m->f + m->c; }
static void mtrans(M3 *m, M3 *r) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r->m[i][j] = m->m[j][i]; }
static double seglen2(Seg *s) { double dx = s->b.x - s->a.x, dy = s->b.y - s->a.y; return dx * dx + dy * dy; }
static void bump(double *p, float *q, V2 *v) { *p += 1; *q *= 2; v->x -= 0.5; v->y++; }
int main(void) {
    for (int i = 0; i < 4; i++) { md(gd[i]); mf(gf[i]); }
    md(gv.x); md(gv.y); mix(sizeof(V2)); mix(sizeof(V3)); mix(sizeof(Mixed)); mix(sizeof gd); mix(sizeof gf);
    for (int i = 0; i < 2; i++) { mix(gm[i].id); md(gm[i].w); mf(gm[i].f); mix(gm[i].c); }
    for (int i = 0; i < 5; i++) md(zero_init[i]);
    V2 a = { 1.5, 2.5 }, b = { -0.5, 0.25 };
    V2 c; vadd(&a, &b, &c); md(c.x); md(c.y);
    vscale(&c, 0.1); md(c.x); md(c.y); md(vdot(&a, &b));
    V3 p = { 1, 2, 3 }, q = { 4, 5.5f, 6 }, r; v3cross(&p, &q, &r); mf(r.x); mf(r.y); mf(r.z);
    Mixed m; mkmixed(&m, 7, 0.3); mix(m.id); md(m.w); mf(m.f); md(mlen2(&m)); md(mlen2(&gm[1]));
    M3 mm, mt; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mm.m[i][j] = i * 3 + j + 0.5;
    mtrans(&mm, &mt); for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) md(mt.m[i][j]);
    Seg s = { { 0, 0 }, { 3, 4 } }; md(seglen2(&s)); vadd(&s.b, &a, &s.b); md(seglen2(&s));
    double d = 1; float f = 2; V2 v = { 5, 6 };
    bump(&d, &f, &v); bump(&d, &f, &v); md(d); mf(f); md(v.x); md(v.y);
    double *pd = gd; pd++; md(*pd); md(pd[1]); md(*(pd + 2)); mix(pd - gd);
    float *pf = &gf[3]; mf(*pf); mf(pf[-1]);
    union { double d; unsigned long long u; float f[2]; } u; u.d = 1.0; mix(u.u); u.u = 0x400921fb54442d18ULL; md(u.d); u.f[0] = 1.0f; u.f[1] = 2.0f; mix(u.u);
    double loc[6] = { 1, 2, 3 }; loc[3] = loc[0] + loc[1] * loc[2]; loc[4] = -loc[3]; for (int i = 0; i < 6; i++) md(loc[i]);
    V2 arr[3] = { { 1, 2 }, { 3, 4 } }; double sumv = 0; for (int i = 0; i < 3; i++) sumv += arr[i].x * arr[i].y; md(sumv);
    V2 cp = arr[1]; cp.x += 0.5; md(cp.x); md(arr[1].x);
    V2 *pv = &arr[2]; pv->x = 9.5; md(arr[2].x); V2 mv[2]; mv[0] = a; vadd(&mv[0], &mv[0], &mv[1]); md(mv[1].y);
    struct { float a; double b; float c; } packed = { 1, 2, 3 }; md(packed.a + packed.b + packed.c); mix(sizeof packed);
    return (int)(H & 0x7fffffff);
}
