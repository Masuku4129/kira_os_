/* Передача и возврат float/double: регистры xmm0-7 / v0-v7, стек после восьми, смешанно с целыми, косвенные вызовы, рекурсия. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
static double add2(double a, double b) { return a + b; }
static float fadd2(float a, float b) { return a + b; }
static double many(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j, double k) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k;
}
static float fmany(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j) {
    return a - b + c - d + e - f + g - h + i * j;
}
static double mixed(int a, double b, long c, float d, char e, double f, unsigned g, float h, long long i, int j, int k, int l, double m, int n, double o, double p, double q, double r, int s) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13 + n * 14 + o * 15 + p * 16 + q * 17 + r * 18 + s * 19;
}
static double fact(int n) { return n <= 1 ? 1.0 : n * fact(n - 1); }
static double fibd(double n) { return n < 2 ? n : fibd(n - 1) + fibd(n - 2); }
static float ident(float x) { return x; }
static double widen(float x) { return x; }
static float narrow(double x) { return (float)x; }
static int toint(double x) { return (int)x; }
static double apply(double (*f)(double, double), double a, double b) { return f(a, b); }
static double table_f(double x) { return x * x - 1; }
static double mul2(double a, double b) { return a * b; }
static double (*tab[3])(double, double) = { add2 };
static double compose(double x) { return table_f(table_f(x)); }
int main(void) {
    md(add2(1.5, 2.25)); mf(fadd2(1.5f, 0.1f)); md(add2(1, 2)); md(add2(1.5f, 2));
    md(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)); md(many(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5));
    mf(fmany(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)); mf(fmany(0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.1f));
    md(mixed(1, 2.5, 3, 4.5f, 5, 6.5, 7, 8.5f, 9, 10, 11, 12, 13.5, 14, 15.5, 16.5, 17.5, 18.5, 19));
    md(fact(20)); md(fibd(15)); mf(ident(0.1f)); md(widen(0.1f)); mf(narrow(0.1)); mix((unsigned)toint(-7.9)); mix((unsigned)toint(7.9f));
    md(apply(add2, 3, 4)); tab[1] = mul2; md(apply(tab[0], 1, 2)); md(apply(tab[1], 5, 0));
    double (*fp)(double) = table_f; md(fp(3)); md(compose(1.5));
    return (int)(H & 0x7fffffff);
}
