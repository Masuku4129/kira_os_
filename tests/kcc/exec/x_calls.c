#include "ktest.h"
long many(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j; }
int mixed(char a, short b, int c, long d, unsigned char e, unsigned short f, unsigned g) { return a + b + c + (int)d + e + f + (int)g; }
static int depth(int n) { return n == 0 ? 0 : 1 + depth(n - 1); }
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int (*pick(int w))(int, int) { return w ? mul : add; }
typedef struct { int a; long b; char c[5]; } S;
static void fill(S *s, int v) { s->a = v; s->b = v * 1000L; for (int i = 0; i < 4; i++) s->c[i] = (char)('a' + v + i); s->c[4] = 0; }
static long sum3(long a, long b, long c) { return a + b + c; }
int main(void) {
    pn(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    pn(many(sum3(1, 2, 3), 2, many(1,1,1,1,1,1,1,1,1,1), 4, 5, 6, 7, 8, 9, sum3(sum3(1,2,3), 4, 5)));
    pn(mixed(-1, -2, -3, -4, 255, 65535, 4000000000u));
    pn(depth(10000));
    pn(pick(0)(3, 4)); pn(pick(1)(3, 4));
    S s; fill(&s, 2); pn(s.a); pn(s.b); ps(s.c); ps("\n");
    S t = s; t.a++; pn(t.a + s.a); ps(t.c); ps("\n");
    int (*fps[2])(int, int) = { add, mul }; pn(fps[1](6, 7) + fps[0](1, 2));
    return 0;
}
