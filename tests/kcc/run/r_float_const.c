/* Константные выражения с вещественными: статические инициализаторы, перечисления, размеры массивов, case, шестнадцатеричные и граничные литералы. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
enum { E1 = (int)2.9, E2 = (int)-2.9, E3 = (int)(1e3 * 1.5), E4 = (long long)1e10 > 0 };
static char buf[(int)3.7 + 1];
static double a1 = 1.0 / 3, a2 = 1e308 * 10, a3 = -1e308 * 10, a4 = 0x1.8p1, a5 = 0x.8p0, a6 = 1e-320, a7 = 2.2250738585072014e-308, a8 = 1.7976931348623157e308;
static double a9 = 123456789012345678901234567890.0, a10 = 0.30000000000000004, a11 = 4.35, a12 = 0.1 + 0.2, a13 = 100 * 1.1, a14 = -0.0, a15 = 5e-324, a16 = 2.4703282292062327e-324;
static float b1 = 16777217, b2 = 0.1f, b3 = 1e-45f, b4 = 3.4028235e38f, b5 = 3.4028236e38f, b6 = 1.0f / 3, b7 = 0.1, b8 = 16777217.0, b9 = 7e-46f, b10 = 0x1p-149f;
static int ci = (int)3.99, cj = (int)-3.99, ck = (unsigned char)200.5, cl = 1.5 > 1, cm = 2.5 == 2.5f, cn = !0.0, co = 1.0 ? 5 : 6;
static long long cll = (long long)9e18, cu = (long long)(unsigned long long)1.8e19;
static unsigned long long cull = 18446744073709549568.0;
static double sel = 1 ? 2.5 : 3, sel2 = 0 ? 2 : 3.5f, ex = 2 + 3.5 * 2 - 1 / 4.0, neg = -(2.5), un = +3.5, conv = 7 / 2 * 1.0, conv2 = 7 / 2.0, cc = (float)0.1, cd = (double)0.1f;
static double arr[] = { 1, 2.5, 'a', 3u, 4LL, 1e2 };
static float farr[] = { 1, 2.5, 'a', 3u, 4LL, 1e2 };
static struct { double d; float f; int i; } st = { 1.5, 2, 3.9 };
static const double *pc = &arr[2];
static double lit(int k) {
    switch (k) { case (int)1.5: return 10.5; case (int)2.5 + 1: return 20.5; case E1 + 5: return 30.5; default: return -1; }
}
int main(void) {
    mix(E1); mix(E2 + 10); mix(E3); mix(E4); mix(sizeof buf);
    md(a1); md(a2); md(a3); md(a4); md(a5); md(a6); md(a7); md(a8); md(a9); md(a10); md(a11); md(a12); md(a13); md(a14); md(a15); md(a16);
    mf(b1); mf(b2); mf(b3); mf(b4); mf(b5); mf(b6); mf(b7); mf(b8); mf(b9); mf(b10);
    mix(ci); mix(cj + 10); mix(ck); mix(cl); mix(cm); mix(cn); mix(co); mix(cll); mix(cu); mix(cull);
    md(sel); md(sel2); md(ex); md(neg); md(un); md(conv); md(conv2); md(cc); md(cd);
    for (int i = 0; i < 6; i++) { md(arr[i]); mf(farr[i]); }
    md(st.d); mf(st.f); mix(st.i); md(*pc);
    md(lit(1)); md(lit(3)); md(lit(7)); md(lit(0));
    md(1e22); md(1e23); md(8.41e21); md(9.5367431640625e-7); md(123456.789e-2); md(0x10p-4); md(0xAp0); md(1.0e+0);
    md(0.1 + 0.7); md(1e15 + 0.3); md(4.9406564584124654e-324); md(2.2250738585072011e-308);
    mf(1e10f); mf(123456789.0f); mf(0.3f + 0.6f); mf(8388609.5f); mf(1.00000017881393432617187499f); mf(1.00000017881393432617187500f);
    return (int)(H & 0x7fffffff);
}
