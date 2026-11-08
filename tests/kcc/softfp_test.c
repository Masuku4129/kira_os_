/* Проверка kcc/softfp.c против аппаратной арифметики хоста: арифметика, сравнения, преобразования и разбор литералов (случайные и граничные значения). */
#include "../../kcc/kcc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int fails;
static uint64_t db(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint64_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double bd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static float bf(uint64_t u) { uint32_t v = (uint32_t)u; float f; memcpy(&f, &v, 4); return f; }
static int same(uint64_t a, uint64_t b, int sz) {      /* NaN совпадает с любым NaN */
    int isn = sz == 8 ? isnan(bd(a)) && isnan(bd(b)) : isnan(bf(a)) && isnan(bf(b));
    return isn || a == b;
}
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 30) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t pick(int sz) {
    uint64_t r = rnd();
    int k = (int)(rnd() % 8);
    uint64_t v;
    if (sz == 8) {
        v = r;
        if (k == 0) v &= 0x800FFFFFFFFFFFFFULL;                      /* субнормальные */
        else if (k == 1) v = (v & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1020 + rnd() % 8) << 52);
        else if (k == 2) v = (v & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1 + rnd() % 3) << 52);
        else if (k == 3) v = (v & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(2040 + rnd() % 8) << 52);
        else if (k == 4) v = (v & 0x8000000000000000ULL) | ((uint64_t)(1023 - 30 + rnd() % 60) << 52) | (rnd() & 0xff);
    } else {
        v = r & 0xffffffffULL;
        if (k == 0) v &= 0x807FFFFFULL;
        else if (k == 1) v = (v & 0x807FFFFFULL) | ((uint64_t)(124 + rnd() % 8) << 23);
        else if (k == 2) v = (v & 0x807FFFFFULL) | ((uint64_t)(1 + rnd() % 3) << 23);
        else if (k == 3) v = (v & 0x807FFFFFULL) | ((uint64_t)(250 + rnd() % 5) << 23);
        else if (k == 4) v = (v & 0x80000000ULL) | ((uint64_t)(127 - 20 + rnd() % 40) << 23) | (rnd() & 0xff);
    }
    return v;
}

static void arith(int sz, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t a = pick(sz), b = pick(sz);
        if (i % 7 == 0) b = a ^ (1ULL << (sz * 8 - 1));
        for (int o = 0; o < 4; o++) {
            int op = "+-*/"[o];
            uint64_t got = sf_binop(op, a, b, sz), want;
            if (sz == 8) { double x = bd(a), y = bd(b); volatile double r = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y; want = db(r); }
            else { float x = bf(a), y = bf(b); volatile float r = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y; want = fb(r); }
            CHECK(same(got, want, sz), "sz=%d %llx %c %llx = %llx, want %llx", sz, (unsigned long long)a, op, (unsigned long long)b, (unsigned long long)got, (unsigned long long)want);
        }
        int c = sf_cmp(a, b, sz), w;
        if (sz == 8) { double x = bd(a), y = bd(b); w = isnan(x) || isnan(y) ? 2 : x < y ? -1 : x > y ? 1 : 0; }
        else { float x = bf(a), y = bf(b); w = isnan(x) || isnan(y) ? 2 : x < y ? -1 : x > y ? 1 : 0; }
        CHECK(c == w, "cmp sz=%d %llx %llx: %d want %d", sz, (unsigned long long)a, (unsigned long long)b, c, w);
    }
}

static void conv(int n) {
    for (int i = 0; i < n; i++) {
        int64_t v = (int64_t)rnd();
        int k = (int)(rnd() % 6);
        if (k == 0) v >>= rnd() % 64;
        else if (k == 1) v = (int64_t)((uint64_t)v & ~(((uint64_t)1 << (rnd() % 40)) - 1));
        else if (k == 2) v = (int64_t)((1ULL << 53) + (int64_t)(rnd() % 40) - 20);
        else if (k == 3) v = (int64_t)(((uint64_t)1 << (rnd() % 64)) + (rnd() % 5) - 2);
        uint64_t u = (uint64_t)v;
        CHECK(sf_from_int(v, 0, 8) == db((double)v), "i64->f64 %lld", (long long)v);
        CHECK(sf_from_int(v, 1, 8) == db((double)u), "u64->f64 %llu", (unsigned long long)u);
        CHECK(sf_from_int(v, 0, 4) == fb((float)v), "i64->f32 %lld", (long long)v);
        CHECK(sf_from_int(v, 1, 4) == fb((float)u), "u64->f32 %llu", (unsigned long long)u);
        uint64_t a = pick(8);
        double d = bd(a);
        int ok;
        if (!isnan(d) && fabs(d) < 9.2e18) { int64_t r = sf_to_int(a, 8, 0, &ok); CHECK(ok && r == (int64_t)d, "f64->i64 %llx: %lld want %lld", (unsigned long long)a, (long long)r, (long long)d); }
        if (!isnan(d) && d > -1 && d < 1.8e19) { uint64_t r = (uint64_t)sf_to_int(a, 8, 1, &ok); CHECK(ok && r == (uint64_t)d, "f64->u64 %llx", (unsigned long long)a); }
        uint64_t af = pick(4);
        float f = bf(af);
        if (!isnan(f) && fabsf(f) < 9.2e18f) { int64_t r = sf_to_int(af, 4, 0, &ok); CHECK(ok && r == (int64_t)f, "f32->i64 %llx", (unsigned long long)af); }
        CHECK(same(sf_conv(a, 8, 4), fb((float)d), 4), "f64->f32 %llx: %llx want %llx", (unsigned long long)a, (unsigned long long)sf_conv(a, 8, 4), (unsigned long long)fb((float)d));
        CHECK(same(sf_conv(af, 4, 8), db((double)f), 8), "f32->f64 %llx", (unsigned long long)af);
    }
}

static void parse_case(const char *s, int hex) {
    Token t; memset(&t, 0, sizeof t);
    char buf[2000];
    t.s = s; t.len = (int)strlen(s);
    int sz;
    if (setjmp(K.jb)) { printf("FAIL: parse error for '%s': %s\n", s, K.err); fails++; return; }
    uint64_t got = sf_parse(&t, &sz);
    size_t n = strlen(s);
    memcpy(buf, s, n + 1);
    int isf = n && (buf[n - 1] == 'f' || buf[n - 1] == 'F') && !hex;
    if (hex && n && (buf[n - 1] == 'f' || buf[n - 1] == 'F') && strchr(buf, 'p')) isf = 1;
    if (isf) buf[n - 1] = 0;
    uint64_t want = isf ? fb(strtof(buf, NULL)) : db(strtod(buf, NULL));
    CHECK(sz == (isf ? 4 : 8), "sz of '%s'", s);
    CHECK(got == want, "parse '%s': %llx want %llx", s, (unsigned long long)got, (unsigned long long)want);
}

static void parse_all(int n) {
    static const char *fixed[] = { "0.0", "1.0", "0.1", "0.1f", "3.14159265358979323846", "1e308", "1.7976931348623157e308", "1.7976931348623159e308", "1e309",
        "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "1e-400", "2.2250738585072014e-308", "123456789012345678901234567890.5",
        "9007199254740993.0", "9007199254740992.5", "9007199254740993.0000000000000000000000000000001", "1.17549435e-38f", "1.4e-45f", "0.7e-45f", "0.70064923216240853546e-45f",
        "3.4028235e38f", "3.4028236e38f", "3.40282357e38f", "16777217.0f", "1e10f", ".5", "5.", "0x1p0", "0x1.8p3", "0x.8p1", "0x1.fffffffffffffp1023", "0x1p-1074",
        "0x1p-1075", "0x1.0000000000001p0", "0x123456789abcdef01p0", "1e+5", "1E-5", "0e10", "00.5", "100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.0", 0 };
    for (int i = 0; fixed[i]; i++) parse_case(fixed[i], strncmp(fixed[i], "0x", 2) == 0);
    for (int i = 0; i < n; i++) {
        char b[200];
        int nd = 1 + (int)(rnd() % 25), p = 0;
        int dot = (int)(rnd() % (nd + 1));
        for (int k = 0; k < nd; k++) { if (k == dot) b[p++] = '.'; b[p++] = (char)('0' + rnd() % 10); }
        if (dot == nd) b[p++] = '.';
        if (rnd() % 3) p += snprintf(b + p, 20, "e%d", (int)(rnd() % 700) - 350);
        if (rnd() % 4 == 0) { b[p++] = 'f'; }
        b[p] = 0;
        if (b[0] == '.' && p == 1) continue;
        if (!strchr(b, '.') && !strchr(b, 'e')) continue;
        if (b[0] == '.' && !(b[1] >= '0' && b[1] <= '9')) continue;
        parse_case(b, 0);
    }
    for (int i = 0; i < n / 4; i++) {                                   /* точные середины: строка, ближайшая к границе округления */
        uint64_t a = pick(8);
        double d = bd(a);
        if (isnan(d) || isinf(d) || d < 0) continue;
        double e = nextafter(d, INFINITY);
        char b[400];
        long double mid = ((long double)d + (long double)e) / 2;
        snprintf(b, sizeof b, "%.40Le", mid);
        parse_case(b, 0);
        snprintf(b, sizeof b, "%.17e", d);
        parse_case(b, 0);
    }
}

int main(void) {
    static char arena[1 << 20];
    memset(&K, 0, sizeof K);
    K.abase = arena; K.acap = sizeof arena;
    arith(8, 400000);
    arith(4, 400000);
    conv(200000);
    parse_all(200000);
    printf(fails ? "softfp: ПРОВАЛОВ %d\n" : "softfp: OK\n", fails);
    return fails != 0;
}
