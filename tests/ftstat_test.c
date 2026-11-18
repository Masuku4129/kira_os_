#include "../ftstat.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
    static fts_t f;
    fts_reset(&f);
    CHECK(fts_avg(&f) == 0 && fts_p99(&f) == 0 && fts_max(&f) == 0 && fts_count(&f) == 0);
    for (uint32_t i = 1; i <= 100; i++) fts_push(&f, i);           // 1..100
    CHECK(fts_count(&f) == 100);
    CHECK(fts_avg(&f) == 50);                                       // 5050 / 100
    CHECK(fts_max(&f) == 100);
    CHECK(fts_p99(&f) == 99);                                       // ровно 99 кадров не больше 99
    fts_reset(&f);
    for (int i = 0; i < 99; i++) fts_push(&f, 10);
    fts_push(&f, 5000);                                             // один тяжёлый кадр из ста: p99 его не видит, максимум видит
    CHECK(fts_p99(&f) == 10 && fts_max(&f) == 5000);
    fts_push(&f, 5000); fts_push(&f, 5000);                          // три тяжёлых из 102 - p99 уже тяжёлый
    CHECK(fts_p99(&f) == 5000);
    fts_reset(&f);
    for (uint32_t i = 0; i < 3 * FTS_N; i++) fts_push(&f, i < 2 * FTS_N ? 1000000 : 7);   // старые кадры вытесняются из окна
    CHECK(fts_p99(&f) == 7);
    CHECK(fts_max(&f) == 1000000);                                  // максимум помнит всё после сброса
    CHECK(fts_count(&f) == 3 * FTS_N);
    fts_reset(&f);
    fts_push(&f, 42);
    CHECK(fts_p99(&f) == 42 && fts_avg(&f) == 42);
    if (fails) return 1;
    printf("ftstat: OK\n");
    return 0;
}
