/* Итерации Ньютона, ряды и циклы с накоплением погрешности: строгий IEEE (без переупорядочения) даёт те же биты, что clang -O0. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
static double dsqrt(double a) {
    double x = a > 1 ? a : 1, y;
    int it = 0;
    do { y = x; x = 0.5 * (y + a / y); it++; } while (x < y && it < 200);
    mix((unsigned)it);
    return y;
}
static float fsqrt_(float a) {
    float x = a, y;
    for (int i = 0; i < 40; i++) { y = x; x = 0.5f * (y + a / y); if (x == y) break; }
    return x;
}
static double cbrt_(double a) {
    double x = a;
    for (int i = 0; i < 100; i++) { double n = (2 * x + a / (x * x)) / 3; if (n == x) break; x = n; }
    return x;
}
static double exp_(double x) {
    double term = 1, sum = 1;
    for (int n = 1; n < 60; n++) { term *= x / n; sum += term; }
    return sum;
}
static double pi_leibniz(int n) { double s = 0, sg = 1; for (int k = 0; k < n; k++) { s += sg / (2 * k + 1); sg = -sg; } return 4 * s; }
static double horner(const double *c, int n, double x) { double r = 0; for (int i = n - 1; i >= 0; i--) r = r * x + c[i]; return r; }
int main(void) {
    static const double vals[] = { 2, 3, 10, 0.5, 1e-6, 12345.678, 1e10, 7 };
    for (int i = 0; i < 8; i++) {
        double s = dsqrt(vals[i]);
        md(s); md(s * s - vals[i]); mf(fsqrt_((float)vals[i])); md(cbrt_(vals[i])); md(exp_(vals[i] > 20 ? 0.5 : vals[i] / 4));
        md(__builtin_sqrt(vals[i])); mf(__builtin_sqrtf((float)vals[i])); md(__builtin_fabs(-vals[i]));
    }
    md(pi_leibniz(10000)); mf((float)pi_leibniz(1000));
    static const double c[] = { 1, -2, 3, -4, 5, 0.5 };
    for (int i = 0; i < 10; i++) md(horner(c, 6, i * 0.37 - 1));
    /* логистическое отображение: хаос усиливает любое расхождение округлений */
    double x = 0.3; for (int i = 0; i < 1000; i++) x = 3.9 * x * (1 - x);
    md(x);
    float fx = 0.3f; for (int i = 0; i < 1000; i++) fx = 3.9f * fx * (1 - fx);
    mf(fx);
    /* ассоциативность не используется: (a + b) + c != a + (b + c) */
    double a = 1e16, b = -1e16, d = 1.0;
    md((a + b) + d); md(a + (b + d)); md(a + d + b);
    double u = 0.1 + 0.2 + 0.3, v = 0.1 + (0.2 + 0.3);
    mix(u == v); md(u); md(v);
    return (int)(H & 0x7fffffff);
}
