#include "ftstat.h"

void fts_reset(fts_t* f) { f->n = f->pos = 0; f->sum = f->total = 0; f->max = 0; }

void fts_push(fts_t* f, uint32_t us) {
    f->us[f->pos] = us;
    f->pos = (f->pos + 1) % FTS_N;
    if (f->n < FTS_N) f->n++;
    f->sum += us;
    f->total++;
    if (us > f->max) f->max = us;
}

uint32_t fts_count(const fts_t* f) { return (uint32_t)f->total; }
uint32_t fts_avg(const fts_t* f) { return f->total ? (uint32_t)(f->sum / f->total) : 0; }
uint32_t fts_max(const fts_t* f) { return f->max; }

uint32_t fts_p99(const fts_t* f) {
    if (!f->n) return 0;
    uint32_t t[FTS_N];
    for (uint32_t i = 0; i < f->n; i++) t[i] = f->us[i];
    for (uint32_t gap = f->n / 2; gap > 0; gap /= 2)                 // сортировка Шелла: без рекурсии и выделений, ядру достаточно
        for (uint32_t i = gap; i < f->n; i++) {
            uint32_t v = t[i], j = i;
            for (; j >= gap && t[j - gap] > v; j -= gap) t[j] = t[j - gap];
            t[j] = v;
        }
    uint32_t rank = (f->n * 99 + 99) / 100;                          // ближайший ранг: ceil(0.99 * n), считая с 1
    return t[rank - 1];
}
