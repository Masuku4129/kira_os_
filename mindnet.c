#include "mindnet.h"

/* 1/(1+e^-z) для z = -8 .. 8 с шагом 1/16 (257 точек, Q16). */
static const uint32_t sig_tab[257] = {
    22, 23, 25, 27, 28, 30, 32, 34, 36, 39, 41, 44,
    47, 50, 53, 56, 60, 64, 68, 72, 77, 82, 87, 92,
    98, 105, 111, 119, 126, 134, 143, 152, 162, 172, 184, 195,
    208, 221, 236, 251, 267, 284, 302, 321, 342, 364, 387, 412,
    439, 467, 497, 528, 562, 598, 636, 677, 720, 766, 815, 867,
    922, 980, 1042, 1109, 1179, 1253, 1333, 1417, 1506, 1601, 1701, 1808,
    1921, 2041, 2168, 2303, 2446, 2598, 2758, 2928, 3108, 3298, 3500, 3713,
    3938, 4176, 4427, 4692, 4971, 5266, 5577, 5904, 6249, 6611, 6992, 7392,
    7812, 8252, 8714, 9197, 9702, 10230, 10782, 11357, 11955, 12579, 13226, 13898,
    14595, 15316, 16062, 16832, 17625, 18442, 19282, 20143, 21025, 21928, 22849, 23788,
    24743, 25712, 26695, 27689, 28693, 29705, 30723, 31744, 32768, 33792, 34813, 35831,
    36843, 37847, 38841, 39824, 40793, 41748, 42687, 43608, 44511, 45393, 46254, 47094,
    47911, 48704, 49474, 50220, 50941, 51638, 52310, 52957, 53581, 54179, 54754, 55306,
    55834, 56339, 56822, 57284, 57724, 58144, 58544, 58925, 59287, 59632, 59959, 60270,
    60565, 60844, 61109, 61360, 61598, 61823, 62036, 62238, 62428, 62608, 62778, 62938,
    63090, 63233, 63368, 63495, 63615, 63728, 63835, 63935, 64030, 64119, 64203, 64283,
    64357, 64427, 64494, 64556, 64614, 64669, 64721, 64770, 64816, 64859, 64900, 64938,
    64974, 65008, 65039, 65069, 65097, 65124, 65149, 65172, 65194, 65215, 65234, 65252,
    65269, 65285, 65300, 65315, 65328, 65341, 65352, 65364, 65374, 65384, 65393, 65402,
    65410, 65417, 65425, 65431, 65438, 65444, 65449, 65454, 65459, 65464, 65468, 65472,
    65476, 65480, 65483, 65486, 65489, 65492, 65495, 65497, 65500, 65502, 65504, 65506,
    65508, 65509, 65511, 65513, 65514,
};

void mnet_init(mnet_t* m) {
    uint8_t* p = (uint8_t*)m;
    for (unsigned i = 0; i < sizeof *m; i++) p[i] = 0;
    m->b = -3 * MNET_ONE;            /* до опыта: опоздания редки (~5%) */
}

uint32_t mnet_sigmoid(int32_t z) {
    int64_t t = (int64_t)z * 16;                 /* шаг таблицы 1/16 */
    int64_t i = (t >> 16) + 128;
    if (i < 0) return sig_tab[0];
    if (i >= 256) return sig_tab[256];
    uint32_t frac = (uint32_t)(t & 0xFFFF);      /* линейная интерполяция между соседними точками */
    return sig_tab[i] + (uint32_t)(((uint64_t)(sig_tab[i + 1] - sig_tab[i]) * frac) >> 16);
}

static int32_t dot(const mnet_t* m, const int32_t* x) {
    int64_t z = m->b;
    for (int i = 0; i < MNET_F; i++) z += ((int64_t)m->w[i] * x[i]) >> 16;
    if (z > 0x7FFFFFFF) z = 0x7FFFFFFF;
    if (z < -0x7FFFFFFF) z = -0x7FFFFFFF;
    return (int32_t)z;
}

uint32_t mnet_predict(const mnet_t* m, const int32_t* x) { return mnet_sigmoid(dot(m, x)); }

/* -ln(p) x1e4 для p в Q16 (грубо: по двоичному логарифму с линейной долей) */
static uint32_t nlog_x1e4(uint32_t p) {
    if (p < 16) p = 16;
    if (p >= MNET_ONE) return 0;
    unsigned k = 0; uint32_t v = p;
    while (v < MNET_ONE) { v <<= 1; k++; }       /* p = v * 2^-k, v в [1, 2) */
    uint32_t frac = (v - MNET_ONE);              /* log2(v) ~ frac (Q16) */
    int64_t l2 = (int64_t)k * MNET_ONE - frac;   /* -log2(p), Q16 */
    return (uint32_t)(l2 * 6931 / MNET_ONE);     /* ln 2 = 0.6931 */
}

void mnet_train(mnet_t* m, const int32_t* x, int y) {
    uint32_t p = mnet_predict(m, x);
    int warn = p >= MNET_ONE / 2;
    /* качество - до шага обучения: так оценивается то, что модель действительно предсказала */
    m->tp -= m->tp >> 8; m->fp -= m->fp >> 8; m->fn -= m->fn >> 8; m->tn -= m->tn >> 8;
    if (warn && y) m->tp += 16; else if (warn) m->fp += 16; else if (y) m->fn += 16; else m->tn += 16;
    uint32_t l = nlog_x1e4(y ? p : MNET_ONE - p);
    m->loss_x1e4 = m->n ? m->loss_x1e4 - (m->loss_x1e4 >> 6) + (l >> 6) : l;
    m->n++;
    if (y) m->pos++;

    /* Шаг градиента: w += lr * (y - p) * x. Скорость - сначала крупная, с опытом мельче (не меньше 1/64). */
    int64_t err = (int64_t)(y ? MNET_ONE : 0) - (int64_t)p;
    unsigned sh = m->n < 500 ? 2 : m->n < 5000 ? 3 : m->n < 50000 ? 5 : 6;
    for (int i = 0; i < MNET_F; i++) {
        int64_t g = (err * x[i]) >> 16;
        int64_t w = (int64_t)m->w[i] + (g >> sh) - (m->w[i] >> 20);   /* слабое затухание весов: старое забывается */
        if (w > 32 * MNET_ONE) w = 32 * MNET_ONE;
        if (w < -32 * MNET_ONE) w = -32 * MNET_ONE;
        m->w[i] = (int32_t)w;
    }
    int64_t b = (int64_t)m->b + (err >> sh);
    if (b > 16 * MNET_ONE) b = 16 * MNET_ONE;
    if (b < -16 * MNET_ONE) b = -16 * MNET_ONE;
    m->b = (int32_t)b;
}

int mnet_top_feature(const mnet_t* m, const int32_t* x) {
    int best = -1; int64_t bv = 0;
    for (int i = 0; i < MNET_F; i++) {
        int64_t c = ((int64_t)m->w[i] * x[i]) >> 16;
        if (c > bv) { bv = c; best = i; }
    }
    return best;
}

unsigned mnet_precision(const mnet_t* m) { return m->tp + m->fp ? (unsigned)((uint64_t)m->tp * 100 / (m->tp + m->fp)) : 0; }
unsigned mnet_recall(const mnet_t* m) { return m->tp + m->fn ? (unsigned)((uint64_t)m->tp * 100 / (m->tp + m->fn)) : 0; }

int mnet_trusted(const mnet_t* m) {
    /* Precision alone can trust a predictor that raises one correct alarm but misses
       almost every incident. Require a minimum recall as well as recent true positives. */
    return m->n >= 120 && m->tp >= 48 &&
           mnet_precision(m) >= 50 && mnet_recall(m) >= 25;
}
