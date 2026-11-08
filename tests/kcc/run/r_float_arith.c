/* float/double: + - * /, унарный минус, составные присваивания, ++/--, знак нуля, бесконечности. Хеш по битам результата (NaN не хешируется). */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
static double zero = 0.0, one = 1.0, three = 3.0, tenth = 0.1, big = 1e308, tiny = 5e-324;
static float fzero = 0.0f, fone = 1.0f, fthree = 3.0f, ftenth = 0.1f, fbig = 3e38f;
int main(void) {
    md(one / three); md(tenth + 0.2); md(tenth * three); md(one - tenth); md(-tenth);
    md(big * 10); md(-big * 10); md(big + big); md(tiny / 2); md(tiny * 0.5 + tiny); md(tiny - tiny);
    md(one / zero); md(-one / zero); md(-zero); md(zero * -one);
    md(1.0 / 3 * 3); md(100.0 / 7); md(1e16 + 1); md(1e16 + 2); md(9007199254740993.0);
    mf(fone / fthree); mf(ftenth + 0.2f); mf(ftenth * fthree); mf(-ftenth); mf(fbig * 10); mf(fbig + fbig);
    mf(fone / fzero); mf(-fzero); mf(16777216.0f + 1.0f); mf(16777216.0f + 3.0f);
    md(ftenth); md(ftenth + tenth); md((double)(ftenth * 3.0f)); md(ftenth * 3.0);
    /* знак нуля через деление */
    mix(one / -zero < 0); mix(one / (zero * -one) < 0); mix(one / (-zero) < 0); mix(1.0 / -0.0 < 0);
    mix(fone / -fzero < 0);
    double a = 1.5; float b = 2.5f; int i = 7;
    a += i; mf(b *= 2); a -= 0.25; a *= b; a /= 4; md(a); b /= 3; mf(b); b -= 1; b += a; mf(b);
    a++; ++a; a--; md(a++); md(++a); md(a--); md(--a); mf(b++); mf(--b);
    i += 2.9; mix((unsigned long long)i); i *= 1.5; mix((unsigned long long)i); i -= 0.5; mix((unsigned long long)i);
    unsigned char uc = 250; uc += 10.7; mix(uc);
    long long ll = 5; ll *= 1e10; mix((unsigned long long)ll);
    double sum = 0, comp = 0;
    for (int k = 1; k <= 1000; k++) { double t = 1.0 / k; double y = t - comp; double s2 = sum + y; comp = (s2 - sum) - y; sum = s2; }
    md(sum);
    float fs = 0; for (int k = 1; k <= 1000; k++) fs += 1.0f / k;
    mf(fs);
    double p = 1; for (int k = 0; k < 100; k++) p = p * 1.1 - p * 0.0999;
    md(p);
    double mixed = i * 0.5 + (char)-3 * 1.5f + (unsigned)4000000000u * 2.0 + 10000000000LL / 3.0;
    md(mixed);
    return (int)(H & 0x7fffffff);
}
