#include "ktest.h"
/* struct по значению: параметры и результаты разных размеров (по ABI обеих архитектур), смешение с целыми, указатели на функции */
struct S1 { char c; };
struct S3 { char a, b, c; };
struct S4 { short a, b; };
struct S8 { int a, b; };
struct S12 { int a, b, c; };
struct S16 { long a, b; };
struct S24 { long a, b, c; };
struct S40 { long v[5]; };
struct Mix { char tag; short s; int i; long l; };
struct Nest { struct S8 in; char z[5]; };
union U { long l; char c[8]; };

static struct S1 mk1(char c) { struct S1 s = { c }; return s; }
static struct S3 mk3(int x) { struct S3 s = { (char)x, (char)(x + 1), (char)(x + 2) }; return s; }
static struct S4 mk4(int x) { struct S4 s = { (short)x, (short)-x }; return s; }
static struct S8 mk8(int x) { struct S8 s = { x, x * 2 }; return s; }
static struct S12 mk12(int x) { struct S12 s = { x, x + 1, x + 2 }; return s; }
static struct S16 mk16(long x) { struct S16 s = { x, -x }; return s; }
static struct S24 mk24(long x) { struct S24 s = { x, x * 2, x * 3 }; return s; }
static struct S40 mk40(long x) { struct S40 s; for (int i = 0; i < 5; i++) s.v[i] = x + i; return s; }
static struct Mix mkmix(int t) { struct Mix m = { (char)t, (short)(t * 3), t * 5, (long)t * 7 }; return m; }
static struct Nest mknest(int x) { struct Nest n = { { x, x + 1 }, { 1, 2, 3, 4, 5 } }; return n; }

static int sum1(struct S1 s) { return s.c; }
static int sum3(struct S3 s) { return s.a * 100 + s.b * 10 + s.c; }
static int sum4(struct S4 s) { return s.a + s.b * 2; }
static int sum8(struct S8 s) { s.a += 1000; return s.a + s.b; }       /* изменение копии не видно вызывающему */
static int sum12(struct S12 s) { return s.a + s.b * 10 + s.c * 100; }
static long sum16(struct S16 s) { return s.a * 3 + s.b; }
static long sum24(struct S24 s) { return s.a + s.b + s.c; }
static long sum40(struct S40 s, long k) { long t = 0; for (int i = 0; i < 5; i++) t += s.v[i] * k; s.v[0] = 999; return t; }
static long summix(struct Mix m) { return m.tag + m.s + m.i + m.l; }
static int sumnest(struct Nest n) { return n.in.a + n.in.b + n.z[0] + n.z[4]; }

/* регистры закончились: структуры уходят в стек целиком, скаляры после них снова могут лечь в регистры */
static long many(int a1, int a2, int a3, int a4, int a5, struct S16 s, int a6, struct S16 t, long a7, struct S12 u, int a8) {
    return a1 + a2 * 2 + a3 * 3 + a4 * 4 + a5 * 5 + s.a * 6 + s.b * 7 + a6 * 8 + t.a * 9 + t.b * 10 + a7 * 11 + u.a * 12 + u.b * 13 + u.c * 14 + a8 * 15;
}
static long mixed(struct S8 a, long b, struct S16 c, int d, struct S24 e, struct S4 f, char g) {
    return a.a + a.b * 2 + b * 3 + c.a * 4 + c.b * 5 + d * 6 + e.a * 7 + e.b * 8 + e.c * 9 + f.a * 10 + f.b * 11 + g * 12;
}
static struct S24 pass24(struct S24 s) { s.b += 1; return s; }
static struct S12 ident12(struct S12 s) { return s; }
static struct S8 swap8(struct S8 s) { struct S8 r = { s.b, s.a }; return r; }
static struct S16 sel(int w, struct S16 a, struct S16 b) { if (w) return a; return b; }
static struct S24 chain(int n, struct S24 acc) { if (n == 0) return acc; acc.a += n; acc.c -= n; return chain(n - 1, acc); }

struct Ops { struct S8 (*mk)(int); int (*sum)(struct S8); };
static int sumfp(struct Ops *o, int x) { return o->sum(o->mk(x)); }

struct S12 g12 = { 7, 8, 9 };
struct S24 garr[3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 } };

int main(void) {
    pn(sum1(mk1('A'))); pn(sum3(mk3(1))); pn(sum4(mk4(5))); pn(sum8(mk8(3))); pn(sum12(mk12(2))); pn(sum16(mk16(10)));
    pn(sum24(mk24(4))); pn(sum40(mk40(1), 2)); pn(summix(mkmix(3))); pn(sumnest(mknest(6)));
    struct S8 x8 = mk8(9); int r = sum8(x8); pn(r); pn(x8.a);
    struct S40 x40 = mk40(5); pn(sum40(x40, 1)); pn(x40.v[0]);
    struct S12 a12 = { 1, 2, 3 }; struct S16 b16 = { 5, 6 }, c16 = { -7, 8 };
    pn(many(1, 2, 3, 4, 5, b16, 6, c16, 7, a12, 8));
    struct S24 e24 = { 10, 20, 30 }; struct S4 f4 = { 3, 4 };
    pn(mixed(mk8(1), 2, b16, 3, e24, f4, 'x'));
    struct S24 p = pass24(e24); pn(p.b); pn(e24.b);
    pn(ident12(a12).c); pn(swap8(mk8(5)).a); pn(sel(1, b16, c16).a); pn(sel(0, b16, c16).b);
    struct S24 ch = chain(5, e24); pn(ch.a); pn(ch.c);
    struct S12 s12; s12 = mk12(20); pn(s12.b); s12 = ident12(g12); pn(s12.a + s12.c);
    pn(mk24(2).c); pn(mk3(7).b); pn(mk40(3).v[4]);
    struct Ops ops = { mk8, sum8 }; pn(sumfp(&ops, 4));
    struct S8 (*fp)(int) = mk8; pn(fp(6).b);
    pn(sum24(garr[1])); pn(sum24(pass24(garr[2])));
    long t = 0; for (int i = 0; i < 3; i++) t += sum24(garr[i]) * (i + 1); pn(t);
    pn(sum12(ident12(mk12(1)))); pn(sum16(mk16(sum12(mk12(3)))));
    return 0;
}
