/* Пул регистров и кадр (x86-64): параметры прямо из регистров аргументов (в том числе когда приёмник совпадает с ещё не прочитанным
 * аргументом), листовые функции без кадра, большое давление на регистры, значения через вызовы, параметры по стеку (вариадики и агрегаты - в tests/kcc/exec/x_vararg.c, x_structval.c). */
static long f6(long a, long b, long c, long d, long e, long f) { return a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6; }
static long f6r(long a, long b, long c, long d, long e, long f) { return f * 1 + e * 2 + d * 3 + c * 4 + b * 5 + a * 6; }
static int f4i(int a, int b, int c, int d) { int t = a - b; int u = c - d; return t * u + (a ^ d) - (b | c); }
static unsigned f3u(unsigned a, unsigned b, unsigned c) { return (a << 3) + (b >> 2) - c * 7u; }
static long unusedp(long a, long b, long c) { return b + 1; }
static long lastonly(long a, long b, long c, long d, long e, long f) { return f; }
static long firstonly(long a, long b, long c, long d, long e, long f) { return a; }
static long swapd(long a, long b, long c, long d, long e, long f) { long t = a; a = f; f = t; t = b; b = e; e = t; return a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6; }
static long sevenargs(long a, long b, long c, long d, long e, long f, long g, long h) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h; }
static long pressure(long a, long b, long c, long d, long e, long f) {
    long x1 = a + b, x2 = b + c, x3 = c + d, x4 = d + e, x5 = e + f, x6 = f + a, x7 = a * b, x8 = c * d, x9 = e * f, x10 = a - f, x11 = b - e, x12 = c - d;
    long s = 0;
    for (int i = 0; i < 7; i++) {
        s += x1 * x2 + x3 * x4 + x5 * x6 + x7 - x8 + x9 + x10 * x11 - x12 + i;
        x1 += x12; x2 ^= x11; x3 += x10; x4 -= x9; x5 += x8; x6 ^= x7; x7 += i; x8 += 3; x9 -= i; x10 += x1; x11 += x2; x12 ^= x3;
    }
    return s + x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10 + x11 + x12;
}
static long viacall(long a, long b, long c) { long r = f6(a, b, c, a + 1, b + 1, c + 1); return r + f6r(c, b, a, 1, 2, 3) + a; }
static unsigned char leafc(unsigned char a, unsigned char b) { return (unsigned char)(a + b * 3); }
static short leafs(short a, short b) { return (short)(a * b - 7); }
static long leafmem(long *p, long i) { return p[i] + p[i + 1]; }
static long leafdiv(long a, long b) { return a / b + a % b; }
static int cmpsel(int a, int b, int c, int d) { return a < b ? c : d; }
static long (*volatile pf6)(long, long, long, long, long, long) = f6;
static long (*volatile pf6r)(long, long, long, long, long, long) = f6r;
static int (*volatile pf4)(int, int, int, int) = f4i;
static unsigned (*volatile pf3)(unsigned, unsigned, unsigned) = f3u;
static long (*volatile pun)(long, long, long) = unusedp;
static long (*volatile plast)(long, long, long, long, long, long) = lastonly;
static long (*volatile pfirst)(long, long, long, long, long, long) = firstonly;
static long (*volatile pswap)(long, long, long, long, long, long) = swapd;
static long (*volatile pseven)(long, long, long, long, long, long, long, long) = sevenargs;
static long (*volatile ppress)(long, long, long, long, long, long) = pressure;
static long (*volatile pvia)(long, long, long) = viacall;
static unsigned char (*volatile plc)(unsigned char, unsigned char) = leafc;
static short (*volatile pls)(short, short) = leafs;
static long (*volatile plm)(long *, long) = leafmem;
static long (*volatile pld)(long, long) = leafdiv;
static int (*volatile pcs)(int, int, int, int) = cmpsel;
int main(void) {
    long acc = 0;
    long arr[8] = { 3, 1, 4, 1, 5, 9, 2, 6 };
    for (long i = 1; i < 40; i++) {
        acc += pf6(i, i + 1, i * 2, i - 3, i ^ 5, i * 7) + pf6r(i, 2 * i, 3 * i, 4, 5, 6 - i);
        acc += pf4((int)i, (int)(i * 3), (int)(i + 9), (int)(i ^ 2)) + (long)pf3((unsigned)i, (unsigned)(i * 100), (unsigned)(i + 1));
        acc += pun(i, i * 2, i * 3) + plast(1, 2, 3, 4, 5, i) + pfirst(i, 2, 3, 4, 5, 6) + pswap(i, i + 1, i + 2, i + 3, i + 4, i + 5);
        acc += pseven(i, 1, 2, 3, 4, 5, 6, i) + ppress(i, i + 2, i * 3, 4, 5 - i, 6) + pvia(i, 2, 3);
        acc += plc((unsigned char)(i * 9), (unsigned char)(i * 13)) + pls((short)(i * 300), (short)(i + 99)) + plm(arr, i & 3) + pld(i * 1000, i + 1) + pcs((int)i, 20, 1, 2);
    }
    return (int)(acc & 0x7fffff);
}
