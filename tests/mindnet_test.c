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

    // High precision with near-zero recall is not a useful predictor and must stay untrusted.
    mnet_t sparse; mnet_init(&sparse);
    sparse.n = 120; sparse.tp = 48; sparse.fp = 0; sparse.fn = 192; sparse.tn = 1000;
    CHECK(mnet_precision(&sparse) == 100);
    CHECK(mnet_recall(&sparse) == 20);
    CHECK(!mnet_trusted(&sparse));
    sparse.fn = 144;
    CHECK(mnet_recall(&sparse) == 25);
    CHECK(mnet_trusted(&sparse));

    printf(fails ? "MINDNET TESTS FAILED\n" : "mindnet: sigmoid, online learning, calibration, explanation, trust gating passed\n");

    // Chronological holdout: features are generated first; the label is sampled independently
    // from a conditional probability. No feature contains or is copied from the target.
    {
        mnet_t temporal; mnet_init(&temporal);
        uint32_t state = 0x51A7E123u;
        unsigned train_positive = 0;
        for (int i = 0; i < 8000; i++) {
            int32_t x[MNET_F] = {0};
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            x[3] = (int32_t)(state & 0xFFFFu);
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            x[9] = (int32_t)(state & 0xFFFFu);
            double signal = 8.0 * (x[3] / (double)MNET_ONE - 0.5);
            double probability = 1.0 / (1.0 + exp(-signal));
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            int y = (state / 4294967296.0) < probability;
            train_positive += (unsigned)y;
            mnet_train(&temporal, x, y);
        }
        double baseline_probability = train_positive / 8000.0;
        double model_brier = 0.0, baseline_brier = 0.0;
        unsigned holdout_positive = 0;
        for (int i = 0; i < 2000; i++) {
            int32_t x[MNET_F] = {0};
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            x[3] = (int32_t)(state & 0xFFFFu);
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            x[9] = (int32_t)(state & 0xFFFFu);
            double signal = 8.0 * (x[3] / (double)MNET_ONE - 0.5);
            double probability = 1.0 / (1.0 + exp(-signal));
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            int y = (state / 4294967296.0) < probability;
            holdout_positive += (unsigned)y;
            double p = mnet_predict(&temporal, x) / (double)MNET_ONE;
            double e = p - y;
            model_brier += e * e;
            e = baseline_probability - y;
            baseline_brier += e * e;
        }
        model_brier /= 2000.0;
        baseline_brier /= 2000.0;
        printf("temporal holdout: model Brier=%.4f, train-prevalence baseline=%.4f, train-positive=%u/8000, holdout-positive=%u/2000, trusted=%d\n",
               model_brier, baseline_brier, train_positive, holdout_positive, mnet_trusted(&temporal));
        CHECK(train_positive > 500 && train_positive < 7500);
        CHECK(holdout_positive > 100 && holdout_positive < 1900);
        CHECK(model_brier < baseline_brier * 0.80);
        CHECK(mnet_trusted(&temporal));
    }

    return fails != 0;
}
