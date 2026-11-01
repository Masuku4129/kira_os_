/* Смесь возможностей WASM для проверки предкомпилятора интерпретатора: рекурсия с параметрами, взаимная рекурсия,
   косвенные вызовы, i64 (деление, остаток, сдвиги, счёт битов), switch (br_table), вложенные циклы с break/continue/goto,
   select, 8/16-битная память со знаком и без, копирование структур (memory.copy), f64, глобальные переменные.
   Печатает один хеш по каждой группе; эталон - тот же код, собранный обычным компилятором. Собирается и с -O0, и с -O2
   (разные формы кода: много local.tee/local.set или плотные выражения). */
#include "kw.h"

static unsigned long long h64 = 1469598103934665603ull;
static void mix(unsigned long long v) { h64 = (h64 ^ v) * 1099511628211ull; h64 ^= h64 >> 29; }
static unsigned lcg = 12345;
static unsigned rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static int ack(int m, int n) { return m == 0 ? n + 1 : n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }
static int is_even(int n);
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int many(int a, int b, int c, int d, int e, int f, int g, int h) { return a - 2 * b + 3 * c - 4 * d + 5 * e - 6 * f + 7 * g - 8 * h; }

typedef int (*fn_t)(int, int);
static int f_add(int a, int b) { return a + b; }
static int f_sub(int a, int b) { return a - b; }
static int f_mul(int a, int b) { return a * b; }
static int f_xor(int a, int b) { return a ^ (b << 3); }
static fn_t tab[4] = { f_add, f_sub, f_mul, f_xor };

static unsigned long long clz64(unsigned long long x) { return (unsigned long long)__builtin_clzll(x | 1); }
static unsigned long long i64ops(unsigned long long a, unsigned long long b) {
    long long sa = (long long)a, sb = (long long)(b | 1);
    unsigned long long r = a / (b | 1) + a % (b | 1) + (unsigned long long)(sa / sb) + (unsigned long long)(sa % sb);
    r += a << (b & 63); r ^= a >> (b & 31); r += (unsigned long long)(sa >> (b & 63));
    r += (a << 13 | a >> 51) ^ (b >> 7 | b << 57);
    r += clz64(a) + (unsigned long long)__builtin_ctzll(b | 1ull << 40) + (unsigned long long)__builtin_popcountll(a ^ b);
    r += (unsigned long long)(sa < sb) + (unsigned long long)(a > b) * 3 + (unsigned long long)(sa >= sb) * 5;
    return r;
}
static int sw(int x) {
    switch (x) {
    case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55;
    case 5: return 66; case 6: return 77; case 7: return 88; case 8: return 99; case 9: return 110;
    case 100: return 5; case 1000: return 6;
    default: return -1;
    }
}
static int loops(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (i % 3 == 0) continue;
        for (int j = 0; j < i; j++) {
            if (j > 7) break;
            s += i * j;
            if (s > 100000) goto out;
        }
    }
out:
    return s;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static signed char sc[64]; static unsigned char uc[64]; static short ss[64]; static unsigned short us[64];
static int g1 = 5; static long long g2 = -77;
struct S { int a[20]; double d; };
static struct S s1, s2;
static double dmix(double x, int n) {
    double r = 0;
    for (int i = 1; i <= n; i++) { r += x / (double)i; x = x * 0.75 + (double)i; if (r > 1e6) r -= 1e6; }
    return r;
}
static int early(int a, int b) {
    if (a < 0) return -1;
    if (b == 0) return 0;
    for (int i = 0; i < 100; i++) { if ((a + i) % b == 7) return i; }
    return 1000;
}

int kmain(void) {
    for (int i = 0; i < 6; i++) for (int j = 0; j < 4; j++) mix((unsigned long long)ack(i % 3, j + 1));
    mix((unsigned long long)is_even(300) + 7 * (unsigned long long)is_odd(177));
    mix((unsigned long long)many(1, 2, 3, 4, 5, 6, 7, 8));
    kw_say("wmix: rec="); kw_say_x(h64);
    for (int i = 0; i < 2000; i++) { unsigned a = rnd(), b = rnd(); mix((unsigned long long)(unsigned)tab[a & 3]((int)a, (int)b)); }
    kw_say(" ind="); kw_say_x(h64);
    for (int i = 0; i < 400; i++) { unsigned long long a = (unsigned long long)rnd() << 32 | rnd(), b = (unsigned long long)rnd() << (i & 31) | (unsigned)i; mix(i64ops(a, b)); }
    kw_say(" i64="); kw_say_x(h64);
    for (int i = -3; i < 1200; i++) mix((unsigned long long)(unsigned)sw(i));
    kw_say(" sw="); kw_say_x(h64);
    for (int n = 0; n < 60; n++) mix((unsigned long long)loops(n * 7));
    for (int i = 0; i < 300; i++) { int a = (int)rnd() - 8000000, b = (int)(rnd() & 255) - 128; mix((unsigned long long)(unsigned)clampi(a, b, b + 5000000)); mix((unsigned long long)(unsigned)early(a & 1023, b)); }
    kw_say(" loop="); kw_say_x(h64);
    for (int i = 0; i < 64; i++) { sc[i] = (signed char)(rnd() & 255); uc[i] = (unsigned char)(rnd() & 255); ss[i] = (short)(rnd() & 65535); us[i] = (unsigned short)(rnd() & 65535); }
    for (int i = 0; i < 64; i++) mix((unsigned long long)(unsigned)((int)sc[i] * 3 + (int)uc[i] * 5 + (int)ss[i] * 7 + (int)us[i] * 11));
    for (int i = 0; i < 20; i++) s1.a[i] = (int)(rnd() & 0xFFFF) - 30000;
    s1.d = 2.5; s2 = s1; s2.a[3] += 99; s1 = s2;
    for (int i = 0; i < 20; i++) mix((unsigned long long)(unsigned)(s1.a[i] - s2.a[19 - i]));
    kw_say(" mem="); kw_say_x(h64);
    g1 += 3; g2 *= 5; mix((unsigned long long)g1); mix((unsigned long long)g2);
    double d = dmix(3.25, 400); mix((unsigned long long)(long long)(d * 1000.0));
    kw_say(" f64="); kw_say_x(h64); kw_say("\n");
    return 0;
}
#ifdef KW_NATIVE
int main(void) { return kmain(); }
#endif
