/* Звук: смешивание с громкостью и ограничением 16 бит, кривая громкости, пересчёт частоты (8/22,05/44,1/96 кГц,
   моно и стерео): число кадров, гладкость синуса, независимость от того, как поток порезан на куски. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include "../audio.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
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
        CHECK(n1 == n2 && n1 >= 47990 && n1 <= 48010);
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
    printf(fails ? "AUDIO TESTS FAILED\n" : "audio: mixing with per-stream/master volume and clipping, volume curve, resampling 8/22.05/44.1/96 kHz mono+stereo -> 48 kHz (frame count, chunking-independent, sine error bounded) passed\n");
    return fails != 0;
}
