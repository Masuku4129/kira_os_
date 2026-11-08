/* Преобразования int<->float<->double всех размеров и знаковости; границы. Отрицательное -> беззнаковое (UB) не проверяется. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
static long long sv[] = { 0, 1, -1, 127, -128, 255, 32767, -32768, 65535, 2147483647LL, -2147483647LL - 1, 4294967295LL, 4294967296LL,
    16777216, 16777217, 16777219, -16777217, 9007199254740992LL, 9007199254740993LL, 9007199254740995LL, 9223372036854775807LL,
    -9223372036854775807LL - 1, 4611686018427387905LL, 1234567890123456789LL, -1234567890123456789LL, 9223372036854774784LL, 9223372036854775295LL };
static unsigned long long uv[] = { 0, 1, 255, 4294967295ULL, 9223372036854775807ULL, 9223372036854775808ULL, 9223372036854775809ULL,
    9223372036854776832ULL, 9223372036854777857ULL, 18446744073709551615ULL, 18446744073709550591ULL, 18446744073709550592ULL,
    17293822569102704641ULL, 13835058055282163713ULL, 12345678901234567890ULL, 9007199254740993ULL, 16777217ULL, 549755813889ULL };
static double dv[] = { 0.0, 0.5, -0.5, 0.99999, 1.0, -1.0, 1.5, -1.5, 127.9, -128.9, 255.9, 32767.5, -32768.5, 65535.9, 2147483647.9, -2147483648.9,
    4294967295.5, 4294967296.0, 9007199254740992.0, 9223372036854774784.0, -9223372036854775808.0, 9223372036854775808.0,
    1.8446744073709550e19, 1e18, 123456789012.75, 0.1, 1e-300, 4.9e-324, 3.4e38, 1.17549435e-38, 1e-40 };
static float fv[] = { 0.0f, 0.5f, 1.5f, -2.5f, 100.75f, 16777216.0f, 1e9f, 2147483520.0f, -2147483648.0f, 4294967040.0f, 1.8446742e19f,
    9.223372e18f, -9.223372e18f, 3.4e38f, 1e-45f, 1.1754944e-38f, 0.1f, 255.5f, 127.99f };
int main(void) {
    int n;
    for (n = 0; n < (int)(sizeof sv / sizeof sv[0]); n++) {
        long long v = sv[n];
        md((double)v); mf((float)v);
        int i = (int)v; md((double)i); mf((float)i);
        unsigned u = (unsigned)v; md((double)u); mf((float)u);
        short s = (short)v; md(s); mf(s);
        unsigned short us = (unsigned short)v; md(us); mf(us);
        signed char c = (signed char)v; md(c); mf(c);
        unsigned char uc = (unsigned char)v; md(uc); mf(uc);
        md(v * 1.0); md(v + 0.5f); mf(v * 1.0f);
    }
    for (n = 0; n < (int)(sizeof uv / sizeof uv[0]); n++) {
        unsigned long long v = uv[n];
        md((double)v); mf((float)v); md((double)(v >> 1)); mf((float)(v | 1)); md(v * 1.0); md(v + 0.5); mf(v / 3.0f);
    }
    for (n = 0; n < (int)(sizeof dv / sizeof dv[0]); n++) {
        double d = dv[n];
        mf((float)d); md((double)(float)d);
        if (d > -2147483649.0 && d < 2147483648.0) { mix((unsigned long long)(long long)(int)d); mix((unsigned long long)(long long)(long)d); }
        if (d > -129.0 && d < 128.0) mix((unsigned long long)(long long)(signed char)d);
        if (d > -32769.0 && d < 32768.0) mix((unsigned long long)(long long)(short)d);
        if (d > -1.0 && d < 256.0) mix((unsigned)(unsigned char)d);
        if (d > -1.0 && d < 65536.0) mix((unsigned)(unsigned short)d);
        if (d > -1.0 && d < 4294967296.0) mix((unsigned)d);
        if (d > -9223372036854775809.0 && d < 9223372036854775808.0) mix((unsigned long long)(long long)d);
        if (d > -1.0 && d < 18446744073709551616.0) mix((unsigned long long)d);
    }
    for (n = 0; n < (int)(sizeof fv / sizeof fv[0]); n++) {
        float f = fv[n];
        md(f); md((double)f * 3);
        if (f > -2147483649.0f && f < 2147483648.0f) mix((unsigned long long)(long long)(int)f);
        if (f > -1.0f && f < 4294967296.0f) mix((unsigned)f);
        if (f > -9223372036854775809.0f && f < 9223372036854775808.0f) mix((unsigned long long)(long long)f);
        if (f > -1.0f && f < 18446744073709551616.0f) mix((unsigned long long)f);
        if (f > -129.0f && f < 128.0f) mix((unsigned long long)(long long)(signed char)f);
        if (f > -1.0f && f < 256.0f) mix((unsigned char)f);
    }
    /* константные преобразования (сворачивает парсер) */
    md((double)9007199254740993LL); mf((float)16777217); mf((float)18446744073709551615ULL); md((double)18446744073709551615ULL);
    mix((unsigned long long)(int)-2.9); mix((unsigned)4294967295.0); mix((unsigned long long)9223372036854775808.0);
    mix((unsigned long long)(long long)-9223372036854775808.0); mix((unsigned char)255.5); mf((float)0.1); md((double)0.1f);
    mf((float)1e-46); md(1.0f / 3); md((double)(1.0f / 3));
    return (int)(H & 0x7fffffff);
}
