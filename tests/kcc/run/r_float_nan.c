/* NaN и бесконечности: все шесть сравнений, !x, ?:, &&, ||, if/while как у clang. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static double zero = 0.0, inf, nan_;
static float fzero = 0.0f, finf, fnan;
#define CMPS(a, b) (((a) < (b)) | ((a) <= (b)) << 1 | ((a) > (b)) << 2 | ((a) >= (b)) << 3 | ((a) == (b)) << 4 | ((a) != (b)) << 5)
static int cmpd(double a, double b) { return CMPS(a, b); }
static int cmpf(float a, float b) { return CMPS(a, b); }
static int cmpmix(double a, float b) { return CMPS(a, b); }
static int branches(double a, double b) {
    int r = 0;
    if (a < b) r |= 1;
    if (a <= b) r |= 2;
    if (a > b) r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    if (!(a < b)) r |= 64;
    if (!(a == b)) r |= 128;
    if (!(a != b)) r |= 256;
    if (a < b || a > b) r |= 512;
    if (a == a && b == b) r |= 1024;
    r |= (a < b ? 2048 : 0) | (a >= b ? 4096 : 0) | (a != a ? 8192 : 0);
    return r;
}
static int loops(double x) {
    int n = 0;
    while (x < 10) { x += 3; n++; if (n > 50) break; }
    for (; x > 0; x -= 7) { n += 100; if (n > 1000) break; }
    do { n += 1000; } while (x >= 0 && x < 1);
    return n;
}
int main(void) {
    inf = 1.0 / zero; nan_ = zero / zero; finf = 1.0f / fzero; fnan = fzero / fzero;
    double ds[] = { 0.0, -0.0, 1.0, -1.0, 1e300, -1e300, 5e-324 };
    float fs[] = { 0.0f, -0.0f, 1.0f, -1.0f, 3e38f, -3e38f, 1e-45f };
    mix(cmpd(nan_, nan_)); mix(cmpd(nan_, 1.0)); mix(cmpd(1.0, nan_)); mix(cmpd(inf, inf)); mix(cmpd(-inf, inf)); mix(cmpd(inf, nan_));
    mix(cmpd(-inf, -inf)); mix(cmpd(inf, 1e308)); mix(cmpd(zero, -zero)); mix(cmpd(nan_, inf));
    mix(cmpf(fnan, fnan)); mix(cmpf(fnan, 1.0f)); mix(cmpf(1.0f, fnan)); mix(cmpf(finf, finf)); mix(cmpf(-finf, finf)); mix(cmpf(fzero, -fzero));
    mix(cmpmix(nan_, 1.0f)); mix(cmpmix(1.0, fnan)); mix(cmpmix(inf, finf)); mix(cmpmix(0.1, 0.1f)); mix(cmpmix(0.5, 0.5f));
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 7; j++) { mix(cmpd(ds[i], ds[j])); mix(cmpf(fs[i], fs[j])); mix(cmpmix(ds[i], fs[j])); mix(branches(ds[i], ds[j])); }
        mix(cmpd(ds[i], nan_)); mix(cmpd(nan_, ds[i])); mix(branches(ds[i], nan_)); mix(branches(nan_, ds[i])); mix(branches(ds[i], inf)); mix(branches(-inf, ds[i]));
        mix(loops(ds[i]));
    }
    mix(branches(nan_, nan_)); mix(branches(inf, inf)); mix(branches(inf, -inf));
    mix(!nan_); mix(!inf); mix(!zero); mix(!-zero); mix(!fnan); mix(!fzero); mix(!1.5);
    mix(nan_ ? 1 : 2); mix(zero ? 1 : 2); mix(-zero ? 1 : 2); mix(fnan ? 1 : 2); mix(inf && nan_); mix(zero || nan_); mix(zero || zero); mix(1.0 && fnan);
    if (nan_) mix(11); else mix(12);
    if (!nan_) mix(13); else mix(14);
    while (nan_) { mix(15); break; }
    for (int k = 0; fnan && k < 3; k++) mix(16 + k);
    /* константные сравнения с NaN, сворачиваются при разборе */
    mix(0.0 / 0.0 == 0.0 / 0.0); mix(0.0 / 0.0 != 0.0 / 0.0); mix(0.0 / 0.0 < 1.0); mix(1.0 / 0.0 > 1e308); mix(!(0.0 / 0.0)); mix(-0.0 == 0.0);
    mix((0.0 / 0.0) ? 5 : 6); mix(1.0f / 0.0f >= 1.0f / 0.0f);
    return (int)(H & 0x7fffffff);
}
