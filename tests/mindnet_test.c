// Хост-тест модели предвидения: сигмоида, обучение на ходу (находит действующие признаки, игнорирует шум),
// калибровка вероятностей, счётчики качества, устойчивость к перекосу классов.
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "mindnet.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static int32_t ru(void) { return (int32_t)(rnd() % (MNET_ONE + 1)); }

int main(void) {
    // сигмоида: середина, симметрия, монотонность, хвосты
    CHECK(mnet_sigmoid(0) == 32768);
    CHECK(mnet_sigmoid(MNET_ONE * 20) <= MNET_ONE && mnet_sigmoid(MNET_ONE * 20) > 65500);
    CHECK(mnet_sigmoid(-MNET_ONE * 20) < 40);
    uint32_t prev = 0; int mono = 1;
    for (int32_t z = -9 * MNET_ONE; z <= 9 * MNET_ONE; z += 997) { uint32_t v = mnet_sigmoid(z); if (v < prev) mono = 0; prev = v; }
    CHECK(mono);
    for (int k = -6; k <= 6; k++) {
        double want = 1.0 / (1.0 + exp(-(double)k * 0.7));
        double got = mnet_sigmoid((int32_t)(k * 0.7 * MNET_ONE)) / 65536.0;
        CHECK(fabs(want - got) < 0.002);
    }

    // Мир: опоздание экрана зависит от признаков 3 (фон) и 9 (опоздания сейчас), остальное - шум.
    // Истинная вероятность: sigmoid(7*x3 + 5*x9 - 7). Положительных примеров ~10%.
    mnet_t m; mnet_init(&m);
    CHECK(!mnet_trusted(&m));
    for (int step = 0; step < 30000; step++) {
        int32_t x[MNET_F];
        for (int i = 0; i < MNET_F; i++) x[i] = ru();
        if (rnd() % 3) x[9] = 0;                                    /* опоздания сейчас бывают не всегда */
        double z = 7.0 * x[3] / MNET_ONE + 5.0 * x[9] / MNET_ONE - 7.0;
        int y = (rnd() / 4294967296.0) < 1.0 / (1.0 + exp(-z));
        mnet_train(&m, x, y);
    }
    printf("weights:"); for (int i = 0; i < MNET_F; i++) printf(" %.2f", m.w[i] / 65536.0); printf(" b=%.2f\n", m.b / 65536.0);
    printf("n=%u pos=%u precision=%u%% recall=%u%% loss=%.3f\n", m.n, m.pos, mnet_precision(&m), mnet_recall(&m), m.loss_x1e4 / 1e4);
    CHECK(m.w[3] > 4 * MNET_ONE && m.w[9] > 3 * MNET_ONE);          /* нашла действующие признаки */
    for (int i = 0; i < MNET_F; i++) if (i != 3 && i != 9) CHECK(m.w[i] < MNET_ONE && m.w[i] > -MNET_ONE);   /* шум - около нуля */
    CHECK(m.b < -4 * MNET_ONE);

    // калибровка: в опасной точке вероятность высокая, в спокойной - низкая
    int32_t hot[MNET_F] = {0}, calm[MNET_F] = {0};
    for (int i = 0; i < MNET_F; i++) { hot[i] = calm[i] = MNET_ONE / 2; }
    hot[3] = MNET_ONE; hot[9] = MNET_ONE; calm[3] = MNET_ONE / 10; calm[9] = 0;
    double ph = mnet_predict(&m, hot) / 65536.0, pc = mnet_predict(&m, calm) / 65536.0;
    printf("p(hot)=%.3f (true %.3f) p(calm)=%.4f (true %.4f)\n", ph, 1 / (1 + exp(-5.0)), pc, 1 / (1 + exp(6.3)));
    CHECK(ph > 0.85 && pc < 0.02);
    CHECK(mnet_top_feature(&m, hot) == 3);                          /* объяснение: главный вклад - фон */
    CHECK(mnet_trusted(&m));

    // Мир без закономерности: модели нельзя доверять (точность предупреждений низкая или их нет)
    mnet_t r; mnet_init(&r);
    for (int step = 0; step < 5000; step++) {
        int32_t x[MNET_F];
        for (int i = 0; i < MNET_F; i++) x[i] = ru();
        mnet_train(&r, x, (rnd() % 20) == 0);
    }
    printf("noise world: precision=%u%% recall=%u%% trusted=%d\n", mnet_precision(&r), mnet_recall(&r), mnet_trusted(&r));
    CHECK(!mnet_trusted(&r));

    printf(fails ? "MINDNET TESTS FAILED\n" : "mindnet: sigmoid, online learning, calibration, explanation, trust gating passed\n");
    return fails != 0;
}
