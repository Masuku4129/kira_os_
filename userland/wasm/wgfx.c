/* Графическая программа WebAssembly: окно 320x200, 31 кадр «плазмы» и вращающегося кольца на f32 (свои sin/sqrt
   без libm). Печатает суммы кадров 0 и 30 - эталон (тот же код, собранный обычным компилятором) должен совпасть,
   и время: так видна скорость интерпретатора. Закрытие окна - выход раньше. */
#include "kw.h"
#define W 320
#define H 200
static unsigned px[W * H];

static float fsin(float x) {                                   /* приведение к [-pi, pi], затем ряд до x^11 */
    const float pi = 3.14159265f, tpi = 6.2831853f;
    int k = (int)(x / tpi); x -= (float)k * tpi;
    if (x > pi) x -= tpi;
    if (x < -pi) x += tpi;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static float fsqrt(float v) { float r = v > 1.0f ? v : 1.0f; for (int i = 0; i < 12; i++) r = 0.5f * (r + v / r); return r; }
static unsigned clamp8(float v) { int i = (int)v; return (unsigned)(i < 0 ? 0 : i > 255 ? 255 : i); }

static unsigned frame(int t) {
    float ft = (float)t * 0.08f;
    float cx = 160.0f + 60.0f * fsin(ft), cy = 100.0f + 40.0f * fsin(ft * 1.3f + 1.0f);
    unsigned sum = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float fx = (float)x, fy = (float)y;
            float v = fsin(fx * 0.045f + ft) + fsin(fy * 0.06f - ft * 0.7f) + fsin((fx + fy) * 0.03f + ft * 1.1f);
            float dx = fx - cx, dy = fy - cy, d = fsqrt(dx * dx + dy * dy);
            float ring = d > 30.0f && d < 42.0f ? 1.0f : 0.0f;
            unsigned r = clamp8(128.0f + 40.0f * v + 120.0f * ring), g = clamp8(128.0f + 40.0f * fsin(v * 2.0f + ft)), b = clamp8(128.0f - 40.0f * v);
            unsigned c = r << 16 | g << 8 | b;
            px[y * W + x] = c; sum = sum * 31u + c;
        }
    return sum;
}

int kmain(void) {
    int win = kw_gfx_create(W, H) == 0;
    long long t0 = kw_ticks();
    unsigned s0 = 0, s30 = 0; int n = 0;
    for (int t = 0; t < 31; t++, n++) {
        unsigned s = frame(t);
        if (t == 0) s0 = s;
        if (t == 30) s30 = s;
        if (win) {
            kw_gfx_present(px);
            unsigned ev[4];
            while (kw_gfx_event(ev) > 0) if (ev[0] == 5) { t = 31; break; }
        }
    }
    kw_say("wgfx: frame0="); kw_say_x(s0); kw_say(" frame30="); kw_say_x(s30); kw_say("\n");
    if (win) { kw_say("wgfx: "); kw_say_i(n); kw_say(" frames shown, ticks "); kw_say_i(kw_ticks() - t0); kw_say("\n"); }
    return 0;
}
#ifdef KW_NATIVE
int main(void) { return kmain(); }
#endif
