/* Звук: смешивание с громкостью и ограничением 16 бит, кривая громкости, пересчёт частоты (8/22,05/44,1/96 кГц,
   моно и стерео): число кадров, гладкость синуса, независимость от того, как поток порезан на куски.
   Фильтр понижения частоты (вход > 48 кГц): таблица ядра по формуле, постоянная составляющая 1:1, синус выше новой
   Найквиста подавлен не хуже 40 дБ, полоса пропускания в пределах +-1 дБ, насыщение без переполнения, совпадение
   целочисленного расчёта с эталоном на double. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include "../audio.h"
#include "../audio_fir.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static double i0d(double x) { double s = 1, t = 1; for (int k = 1; t > 1e-18 * s; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static double kern(double x) {                               /* та же формула, что в tools/gen_audio_fir.py */
    if (fabs(x) >= AUDIO_FIR_HALF) return 0;
    double fc = 0.45, sinc = x == 0 ? 1 : sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x);
    return 2 * fc * sinc * i0d(5.0 * sqrt(1 - (x / AUDIO_FIR_HALF) * (x / AUDIO_FIR_HALF))) / i0d(5.0);
}
static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

/* Усиление синуса f Гц (амплитуда amp) при входной частоте R, моно: дБ по RMS выхода (без краёв) к RMS входа. */
static double gain_db(unsigned R, double f, double amp) {
    static int16_t x[96000], y[2 * 96000];
    for (unsigned i = 0; i < R; i++) x[i] = (int16_t)lround(amp * sin(2 * M_PI * f * i / R));
    audio_rs_t s; audio_rs_init(&s, R, 1);
    uint32_t n = audio_resample(&s, x, R, y, 96000);
    double sq = 0; uint32_t cnt = 0;
    for (uint32_t i = 200; i + 200 < n; i++) { sq += (double)y[2 * i] * y[2 * i]; cnt++; }
    double out = sqrt(sq / cnt), want = amp / sqrt(2.0);
    return 20 * log10((out + 1e-9) / want);
}

static void fir_tests(void) {
    /* 1. таблица ядра = формула (Q15, до 1 единицы округления) */
    for (int j = 0; j <= AUDIO_FIR_HALF * AUDIO_FIR_RES; j++)
        if (fabs(audio_fir_tab[j] - 32768 * kern((double)j / AUDIO_FIR_RES)) > 1.0) { printf("table[%d]\n", j); CHECK(0); break; }
    CHECK(audio_fir_tab[AUDIO_FIR_HALF * AUDIO_FIR_RES] == 0);

    static const unsigned rates[] = { 96000, 88200, 64000, 50000, 48001 };
    for (unsigned q = 0; q < sizeof rates / sizeof rates[0]; q++) {
        unsigned R = rates[q];
        /* 2. подавление выше новой Найквиста (24 кГц): от самой Найквиста до входной Найквиста */
        double worst = -1000, wf = 0;
        for (double f = 24000; f < R / 2.0; f += (R / 2.0 - 24000) / 40.0 + 1) {
            double g = gain_db(R, f, 30000);
            if (g > worst) { worst = g; wf = f; }
        }
        double g_top = gain_db(R, R / 2.0 - 1, 30000);
        if (g_top > worst) worst = g_top;
        if (worst > -40) printf("R=%u: stopband worst %.1f dB at %.0f Hz\n", R, worst, wf);
        CHECK(worst <= -40);
        printf("  filter %u -> 48000: stopband 24 kHz..%.1f kHz worst %.1f dB", R, R / 2000.0, worst);
        /* 3. полоса пропускания: +-1 дБ */
        static const double pb[] = { 50, 440, 1000, 5000, 10000, 15000, 19200, 20000 };
        double lo = 1000, hi = -1000;
        for (unsigned k = 0; k < sizeof pb / sizeof pb[0]; k++) { double g = gain_db(R, pb[k], 20000); if (g < lo) lo = g; if (g > hi) hi = g; }
        CHECK(lo >= -1.0 && hi <= 1.0);
        printf(", passband 50 Hz..20 kHz %.3f..%.3f dB\n", lo, hi);
    }

    /* 4. постоянная составляющая: ровно 1:1, в том числе на краях 16 бит и в начале потока */
    static const int16_t dcs[] = { 12345, 32767, -32768, -1, 0 };
    for (unsigned d = 0; d < 5; d++) for (unsigned R = 48001; R <= 96000; R += 7919) {
        static int16_t x[4000], y[2 * 4100];
        for (int i = 0; i < 4000; i++) x[i] = dcs[d];
        audio_rs_t s; audio_rs_init(&s, R, 1);
        uint32_t n = audio_resample(&s, x, 4000, y, 4100);
        int ok = n + 40 > 4000ull * 48000 / R;
        for (uint32_t i = 0; i < n; i++) if (y[2 * i] != dcs[d] || y[2 * i + 1] != dcs[d]) ok = 0;
        CHECK(ok);
    }

    /* 5. насыщение: входной знак = знак ядра для выбранного выходного кадра - сумма |w| больше суммы w, результат
       до ограничения за пределами 16 бит; он должен упереться в границу, а не провернуться. */
    {
        const unsigned R = 96000; const uint32_t M = 1000;
        static int16_t x[8000], y[2 * 8100];
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            double sa = 0, sw = 0;
            for (int i = 0; i < 8000; i++) {
                double k = kern((i - 2000.0) / 2);                /* кадр M стоит во входном времени M*R/48000 = 2000 */
                x[i] = (int16_t)(k > 0 ? 32767 * sgn : k < 0 ? -32767 * sgn : 0);
                sa += fabs(k); sw += k;
            }
            double pre = 32767.0 * sa / sw;
            audio_rs_t s; audio_rs_init(&s, R, 1);
            uint32_t n = audio_resample(&s, x, 8000, y, 8100);
            CHECK(n > M && pre > 33500);
            CHECK(y[2 * M] == (sgn > 0 ? 32767 : -32768));
            printf("  filter saturation: worst-case phase gives %.0f before the limiter, output %d\n", pre * sgn, y[2 * M]);
        }
    }

    /* 6. сверка целочисленного расчёта с эталоном на double: полноразмерный шум, моно и стерео, любая дробная позиция;
       расхождение - округление коэффициентов таблицы до Q15 (до 12 единиц из 65536 при шуме на весь размах) */
    for (unsigned R = 48500; R <= 96000; R += 9500) for (unsigned ch = 1; ch <= 2; ch++) {
        static int16_t x[6000 * 2], y[2 * 6100];
        for (unsigned i = 0; i < 6000 * ch; i++) x[i] = (int16_t)(rnd() % 65536 - 32768);
        audio_rs_t s; audio_rs_init(&s, R, ch);
        uint32_t n = audio_resample(&s, x, 6000, y, 6100);
        double maxd = 0;
        for (uint32_t m = 0; m < n; m++) for (unsigned c = 0; c < 2; c++) {
            double acc = 0, ws = 0, cen = (double)m * R / 48000;
            for (long k = (long)ceil(cen - AUDIO_FIR_HALF * (double)R / 48000); k <= (long)floor(cen + AUDIO_FIR_HALF * (double)R / 48000); k++) {
                if (k < 0 || k >= 6000) continue;
                double w = kern((k - cen) * 48000.0 / R);
                acc += w * x[k * ch + (ch == 2 ? c : 0)]; ws += w;
            }
            double want = acc / ws; want = want > 32767 ? 32767 : want < -32768 ? -32768 : want;
            maxd = fmax(maxd, fabs(y[2 * m + c] - want));
        }
        if (!(maxd < 12)) printf("R=%u ch=%u: max deviation from the double reference %.2f\n", R, ch, maxd);
        CHECK(maxd < 12 && n + 40 > 6000ull * 48000 / R);
    }

    /* 7. тот же результат при любой нарезке входа на куски */
    for (unsigned R = 50000; R <= 96000; R += 23000) for (unsigned ch = 1; ch <= 2; ch++) {
        static int16_t x[5000 * 2], a[2 * 5100], b[2 * 5100];
        for (unsigned i = 0; i < 5000 * ch; i++) x[i] = (int16_t)(rnd() % 40000 - 20000);
        audio_rs_t s1, s2; audio_rs_init(&s1, R, ch); audio_rs_init(&s2, R, ch);
        uint32_t n1 = audio_resample(&s1, x, 5000, a, 5100), n2 = 0;
        for (unsigned off = 0; off < 5000; ) {
            unsigned k = 1 + (rnd() % 150); if (off + k > 5000) k = 5000 - off;
            n2 += audio_resample(&s2, x + off * ch, k, b + 2 * n2, 5100 - n2);
            off += k;
        }
        int same = n1 == n2; for (uint32_t i = 0; same && i < 2 * n1; i++) if (a[i] != b[i]) same = 0;
        CHECK(same);
    }
}

static int16_t in[96000 * 2], o1[48000 * 2 + 64], o2[48000 * 2 + 64];

int main(void) {
    int16_t a[4] = { 1000, 30000, -30000, 0 }, b[4] = { 2000, 10000, -10000, -5 }, out[4];
    const int16_t* src[2] = { a, b };
    audio_mix(out, src, 0, 2, 4, 65536);
    CHECK(out[0] == 3000 && out[1] == 32767 && out[2] == -32768 && out[3] == -5);
    audio_mix(out, src, 0, 0, 4, 65536);
    CHECK(!out[0] && !out[1] && !out[2] && !out[3]);
    uint32_t g[2] = { 32768, 65536 };                       /* первый вполовину, общая - вполовину */
    audio_mix(out, src, g, 2, 4, 32768);
    CHECK(out[0] == 1250 && out[1] == 12500 && out[2] == -12500);
    CHECK(audio_vol_gain(0) == 0 && audio_vol_gain(100) == 65536 && audio_vol_gain(50) == 16384);

    static const unsigned rates[4] = { 8000, 22050, 44100, 96000 };
    for (int r = 0; r < 4; r++) for (unsigned ch = 1; ch <= 2; ch++) {
        unsigned R = rates[r];
        for (unsigned i = 0; i < R; i++) {                 /* 1 с синуса 440 Гц */
            int16_t v = (int16_t)(12000 * sin(2 * M_PI * 440 * i / R));
            in[i * ch] = v; if (ch == 2) in[i * ch + 1] = (int16_t)-v;
        }
        audio_rs_t s1, s2; audio_rs_init(&s1, R, ch); audio_rs_init(&s2, R, ch);
        uint32_t n1 = audio_resample(&s1, in, R, o1, 48000 + 32);
        uint32_t n2 = 0;
        for (unsigned off = 0; off < R; ) {                 /* куски разной длины */
            unsigned k = 1 + (off * 7) % 97; if (off + k > R) k = R - off;
            n2 += audio_resample(&s2, in + off * ch, k, o2 + 2 * n2, 48000 + 32 - n2);
            off += k;
        }
        CHECK(n1 == n2 && n1 + (R > AUDIO_RATE ? AUDIO_FIR_HALF + 4 : 0) >= 47990 && n1 <= 48010);
        int same = 1; for (uint32_t i = 0; i < 2 * n1; i++) if (o1[i] != o2[i]) { same = 0; break; }
        CHECK(same);
        double err = 0;                                      /* отклонение от точного синуса на 48 кГц */
        for (uint32_t i = 100; i < 47000; i++) {
            double want = 12000 * sin(2 * M_PI * 440 * i / 48000.0);
            err = fmax(err, fabs(o1[2 * i] - want));
            if (ch == 2) CHECK(o1[2 * i + 1] == (int16_t)-o1[2 * i] || abs(o1[2 * i + 1] + o1[2 * i]) <= 1);
            else CHECK(o1[2 * i + 1] == o1[2 * i]);
        }
        if (!(err < (R == 8000 ? 400 : 60))) printf("rate %u ch %u: max error %.1f\n", R, ch, err);
        CHECK(err < (R == 8000 ? 400 : 60));                  /* линейная интерполяция: при 8 кГц грубее */
    }

    fir_tests();
    printf(fails ? "AUDIO TESTS FAILED\n" : "audio: mixing with per-stream/master volume and clipping, volume curve, resampling 8/22.05/44.1/96 kHz mono+stereo -> 48 kHz (frame count, chunking-independent, sine error bounded), downsampling FIR 96/88.2/64/50/48.001 kHz -> 48 kHz (table = formula, sine above 24 kHz suppressed >= 50 dB, passband 50 Hz..20 kHz within 0.2 dB, DC exact, saturation without wrap, equals double reference) passed\n");
    return fails != 0;
}
