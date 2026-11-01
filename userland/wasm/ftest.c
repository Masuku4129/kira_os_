/* Проверка вещественной арифметики интерпретатора WASM: тот же код собирается обычным компилятором (эталон) и в
   .wasm - вывод (биты результатов) обязан совпасть. f32/f64: арифметика, sqrt, floor/ceil/trunc/nearest, min/max,
   copysign, преобразования int<->float (в т.ч. беззнаковые и 64-битные), множество Мандельброта, ряд для пи. */
#include "kw.h"

static unsigned long long bits_d(double d) { union { double d; unsigned long long u; } x; x.d = d; return x.u; }
static unsigned bits_f(float f) { union { float f; unsigned u; } x; x.f = f; return x.u; }
#ifdef KW_NATIVE                                                 /* f64.min/max: у C fmin другое для NaN - здесь NaN нет */
#define FMIN(a, b) __builtin_fmin(a, b)
#define FMAX(a, b) __builtin_fmax(a, b)
#else
#define FMIN(a, b) __builtin_wasm_min_f64(a, b)
#define FMAX(a, b) __builtin_wasm_max_f64(a, b)
#endif
static unsigned long long h = 1469598103934665603ull;
static void mix(unsigned long long v) { h = (h ^ v) * 1099511628211ull; }
static volatile double vd[12] = { 0.5, -0.5, 1.5, 2.5, -2.5, 3.7, -3.7, 1e300, -0.0, 123456789.75, 4503599627370497.0, 0.1 };
static volatile float vf[8] = { 0.5f, -1.5f, 2.5f, 8388609.0f, -0.0f, 3.14159f, 1e-40f, 65504.0f };

int kmain(void) {
    for (int i = 0; i < 12; i++) {
        double x = vd[i];
        mix(bits_d(__builtin_floor(x))); mix(bits_d(__builtin_ceil(x))); mix(bits_d(__builtin_trunc(x)));
        mix(bits_d(__builtin_rint(x))); mix(bits_d(__builtin_sqrt(x < 0 ? -x : x))); mix(bits_d(x * 3.0 - x / 7.0));
        mix(bits_d(FMIN(x, 1.0))); mix(bits_d(FMAX(x, -1.0))); mix(bits_d(__builtin_copysign(2.0, x)));
        mix(bits_f((float)x));
    }
    for (int i = 0; i < 8; i++) {
        float x = vf[i];
        mix(bits_f(__builtin_floorf(x))); mix(bits_f(__builtin_ceilf(x))); mix(bits_f(__builtin_truncf(x)));
        mix(bits_f(__builtin_rintf(x))); mix(bits_f(__builtin_sqrtf(x < 0 ? -x : x))); mix(bits_f(x * x + 1.25f));
        mix(bits_d((double)x * 1e10));
    }
    for (long long k = -5; k <= 5; k++) {                        /* преобразования целых */
        long long v = k * 123456789012345ll; unsigned long long u = (unsigned long long)v;
        mix(bits_d((double)v)); mix(bits_d((double)u)); mix(bits_f((float)v)); mix(bits_f((float)u));
        mix(bits_d((double)(int)k)); mix(bits_d((double)(unsigned)(k + 5) * 4294967.0));
        double d = (double)v / 3.0;
        mix((unsigned long long)(long long)d); mix((unsigned long long)(unsigned)((k + 6) * 70000.5));
    }
    int inside = 0; unsigned long long iters = 0;                 /* Мандельброт 64x48 */
    for (int py = 0; py < 48; py++) for (int px = 0; px < 64; px++) {
        double cx = -2.0 + px * 3.0 / 64, cy = -1.2 + py * 2.4 / 48, zx = 0, zy = 0; int n = 0;
        while (n < 200 && zx * zx + zy * zy < 4.0) { double t = zx * zx - zy * zy + cx; zy = 2 * zx * zy + cy; zx = t; n++; }
        iters += (unsigned long long)n; if (n == 200) inside++;
    }
    double pi = 0; for (int k = 0; k < 100000; k++) pi += (k & 1 ? -4.0 : 4.0) / (2 * k + 1);
    float pf = 0; for (int k = 0; k < 2000; k++) pf += 1.0f / ((float)k * k + 1.0f);
    kw_say("ftest: mandel inside="); kw_say_i(inside); kw_say(" iters="); kw_say_i((long long)iters);
    kw_say(" pi="); kw_say_x(bits_d(pi)); kw_say(" f32sum="); kw_say_x(bits_f(pf)); kw_say(" hash="); kw_say_x(h); kw_say("\n");
    return 0;
}
#ifdef KW_NATIVE
int main(void) { return kmain(); }
#endif
