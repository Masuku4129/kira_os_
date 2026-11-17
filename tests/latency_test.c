// Хост-тест измерения времени реакции (latency.c): корзины, границы, квантили, пороги, сброс по эпохе, слияние по ядрам, места, перевод тактов,
// многопоточная запись под ThreadSanitizer (--threads).
#include "latency.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef __SANITIZE_THREAD__
#include <pthread.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void test_bins(void) {
    CHECK(lat_bin(0) == 0 && lat_bin(15) == 0 && lat_bin(16) == 1);
    CHECK(lat_bin(16) == 1 && lat_bin(19) == 1 && lat_bin(20) == 2 && lat_bin(24) == 3 && lat_bin(28) == 4 && lat_bin(31) == 4 && lat_bin(32) == 5);
    /* границы: значение лежит в своей корзине, корзины идут сплошь без дыр и перекрытий, границы растут */
    for (unsigned b = 0; b + 1 < LAT_BINS; b++) {
        CHECK(lat_bin_hi(b) == lat_bin_lo(b + 1));
        CHECK(lat_bin_lo(b) < lat_bin_hi(b));
        CHECK(lat_bin(lat_bin_lo(b)) == b);
        CHECK(lat_bin(lat_bin_hi(b) - 1) == b);
    }
    CHECK(lat_bin_hi(LAT_BINS - 1) == ~0ULL);
    CHECK(lat_bin(~0ULL) == LAT_BINS - 1 && lat_bin(1ULL << 40) == LAT_BINS - 1);
    /* 201 мс - начало последней корзины */
    CHECK(lat_bin_lo(LAT_BINS - 1) == (1ULL << 27) + (2ULL << 25));
    /* случайные значения: попадание в границы своей корзины */
    srand(7);
    for (int i = 0; i < 20000; i++) {
        uint64_t v = ((uint64_t)rand() << 31 ^ (uint64_t)rand()) >> (rand() % 50);
        unsigned b = lat_bin(v);
        CHECK(v >= lat_bin_lo(b) && v < lat_bin_hi(b));
    }
}

static void test_hist(void) {
    lat_hist_t h; lat_hist_clear(&h);
    CHECK(lat_hist_quantile(&h, 999000) == 0);
    CHECK(lat_hist_add(&h, 1000, 7, 0) == 0);              /* порога нет */
    CHECK(h.count == 1 && h.max_ns == 1000 && h.max_site == 7 && h.over == 0);
    CHECK(lat_hist_add(&h, 500, 8, 600) == 0 && h.max_site == 7);
    CHECK(lat_hist_add(&h, 700, 9, 600) == 1 && h.over == 1 && h.max_ns == 1000);    /* строго больше порога */
    CHECK(lat_hist_add(&h, 600, 9, 600) == 0);              /* равно порогу - не превышение */
    CHECK(lat_hist_add(&h, 5000, 11, 600) == 1 && h.max_ns == 5000 && h.max_site == 11 && h.over == 2 && h.sum_ns == 1000 + 500 + 700 + 600 + 5000);
    /* квантили: p0 .. p100 на известном наборе */
    lat_hist_clear(&h);
    for (int i = 0; i < 1000; i++) lat_hist_add(&h, 100, 0, 0);                  /* 1000 замеров по 100 нс */
    CHECK(lat_hist_quantile(&h, 500000) == 100 && lat_hist_quantile(&h, 999000) == 100);   /* верх корзины не выше максимума */
    lat_hist_add(&h, 100000, 5, 0);                                              /* один выброс: 1001 замер */
    CHECK(lat_hist_quantile(&h, 990000) < 200);                                  /* p99: ранг 991 - всё ещё в основной массе */
    CHECK(lat_hist_quantile(&h, 999000) < 200);                                  /* p99.9: ранг 1000 */
    CHECK(lat_hist_quantile(&h, 1000000) == 100000);                             /* p100 - максимум точно (верх корзины 106496 режется максимумом) */
    for (int i = 0; i < 9; i++) lat_hist_add(&h, 100000, 5, 0);                  /* выбросов 10 из 1010: доля 0,99% - p99.9 уже в выбросе, p99 ещё нет */
    CHECK(lat_hist_quantile(&h, 999000) == 100000);
    CHECK(lat_hist_quantile(&h, 990000) < 200);
    /* квантиль - оценка сверху: ни разу не меньше точного значения */
    lat_hist_clear(&h);
    uint64_t vals[500];
    srand(3);
    for (int i = 0; i < 500; i++) { vals[i] = (uint64_t)(rand() % 3000000) + 1; lat_hist_add(&h, vals[i], 0, 0); }
    for (int a = 0; a < 500; a++) for (int b = a + 1; b < 500; b++) if (vals[a] > vals[b]) { uint64_t t = vals[a]; vals[a] = vals[b]; vals[b] = t; }
    uint32_t ppms[] = { 500000, 900000, 990000, 999000 };
    for (int k = 0; k < 4; k++) {
        unsigned rank = (unsigned)(((uint64_t)500 * ppms[k] + 999999) / 1000000);
        CHECK(lat_hist_quantile(&h, ppms[k]) >= vals[rank - 1]);
    }
}

static void test_merge(void) {
    lat_hist_t a, b, m; lat_hist_clear(&a); lat_hist_clear(&b); lat_hist_clear(&m);
    for (int i = 0; i < 10; i++) lat_hist_add(&a, 2000, 1, 1500);
    for (int i = 0; i < 5; i++) lat_hist_add(&b, 8000, 2, 1500);
    lat_hist_merge(&m, &a); lat_hist_merge(&m, &b);
    CHECK(m.count == 15 && m.over == 15 && m.max_ns == 8000 && m.max_site == 2 && m.sum_ns == 10 * 2000 + 5 * 8000);
    unsigned total = 0; for (unsigned i = 0; i < LAT_BINS; i++) total += m.bin[i];
    CHECK(total == 15 && m.bin[lat_bin(2000)] == 10 && m.bin[lat_bin(8000)] == 5);
}

static void test_set(void) {
    static lat_cpu_t mem[4]; lat_set_t s;
    lat_set_init(&s, mem, 4);
    CHECK(lat_set_limit(&s, LAT_IRQ, 5000) == 0 && lat_set_limit(&s, LAT_KINDS, 1) == -1);
    CHECK(lat_set_record(&s, 0, LAT_IRQ, 4000, 100) == 0);
    CHECK(lat_set_record(&s, 1, LAT_IRQ, 6000, 101) == 1);
    CHECK(lat_set_record(&s, 3, LAT_IRQ, 90000, 103) == 1);
    CHECK(lat_set_record(&s, 1, LAT_ISR, 90000, 5) == 0);          /* у isr порога нет */
    CHECK(lat_set_record(&s, 4, LAT_IRQ, 1, 0) == 0 && s.dropped == 1);   /* ядра нет в таблице */
    CHECK(lat_set_record(&s, 0, LAT_KINDS, 1, 0) == 0);
    lat_hist_t m; lat_set_merge(&s, LAT_IRQ, &m);
    CHECK(m.count == 3 && m.over == 2 && m.max_ns == 90000 && m.max_site == 103);
    lat_set_merge(&s, LAT_ISR, &m);
    CHECK(m.count == 1 && m.over == 0);
    lat_hist_t c1; CHECK(lat_set_cpu_hist(&s, 1, LAT_IRQ, &c1) == 0 && c1.count == 1 && c1.max_ns == 6000);
    CHECK(lat_set_cpu_hist(&s, 9, LAT_IRQ, &c1) == -1);
    /* ожидающее превышение: наибольшее и число; забрать можно один раз */
    uint64_t ns, site; uint32_t n;
    CHECK(lat_set_take_pending(&s, 1, LAT_IRQ, &ns, &site, &n) == 1 && ns == 6000 && site == 101 && n == 1);
    CHECK(lat_set_take_pending(&s, 1, LAT_IRQ, &ns, &site, &n) == 0);
    lat_set_record(&s, 3, LAT_IRQ, 7000, 5); lat_set_record(&s, 3, LAT_IRQ, 9000, 6); lat_set_record(&s, 3, LAT_IRQ, 8000, 7);
    CHECK(lat_set_take_pending(&s, 3, LAT_IRQ, &ns, &site, &n) == 1 && n == 4 && ns == 90000 && site == 103);   /* с прежним 90000 */
    CHECK(lat_set_take_pending(&s, 0, LAT_IRQ, &ns, &site, &n) == 0);                  /* на ядре 0 превышений не было */
    /* сброс: гистограммы пусты, пороги целы, ядра обнуляются при следующей записи и не мешают чтению */
    lat_set_reset(&s);
    lat_set_merge(&s, LAT_IRQ, &m);
    CHECK(m.count == 0 && m.over == 0 && m.max_ns == 0);
    CHECK(s.limit[LAT_IRQ] == 5000);
    CHECK(lat_set_take_pending(&s, 3, LAT_IRQ, &ns, &site, &n) == 0);
    CHECK(lat_set_record(&s, 1, LAT_IRQ, 6000, 1) == 1);                                 /* порог остался */
    lat_set_merge(&s, LAT_IRQ, &m);
    CHECK(m.count == 1 && m.over == 1 && m.max_ns == 6000);
    CHECK(lat_set_cpu_hist(&s, 0, LAT_IRQ, &c1) == 0 && c1.count == 0);                  /* ядро 0 ещё прежней эпохи */
    /* предел порога 0 = выключен */
    lat_set_limit(&s, LAT_IRQ, 0);
    CHECK(lat_set_record(&s, 1, LAT_IRQ, 1000000000ULL, 1) == 0);
}

static void test_sites(void) {
    static lat_cpu_t mem[2]; lat_set_t s;
    lat_set_init(&s, mem, 2);
    lat_set_record(&s, 0, LAT_IRQOFF, LAT_SITE_MIN_NS - 1, 0x10);               /* короче порога мест: в таблицу не идёт */
    lat_site_t out[LAT_SITES];
    CHECK(lat_set_sites(&s, LAT_IRQOFF, out) == 0);
    lat_set_record(&s, 0, LAT_IRQOFF, 300000, 0x10); lat_set_record(&s, 1, LAT_IRQOFF, 500000, 0x10); lat_set_record(&s, 1, LAT_IRQOFF, 200000, 0x20);
    unsigned n = lat_set_sites(&s, LAT_IRQOFF, out);
    CHECK(n == 2 && out[0].site == 0x10 && out[0].count == 2 && out[0].max_ns == 500000 && out[0].sum_ns == 800000 && out[1].site == 0x20);
    /* таблица ядра на 8 мест: девятое, более долгое, вытесняет самое короткое, более короткое - отбрасывается */
    for (unsigned i = 0; i < LAT_SITES; i++) lat_set_record(&s, 0, LAT_ISR, 1000000 + i * 1000, 100 + i);
    lat_set_record(&s, 0, LAT_ISR, 150000, 999);                                 /* короче всех в таблице: не входит */
    n = lat_set_sites(&s, LAT_ISR, out);
    CHECK(n == LAT_SITES);
    for (unsigned i = 0; i < n; i++) CHECK(out[i].site != 999);
    lat_set_record(&s, 0, LAT_ISR, 9000000, 888);                                 /* длиннее всех: вытесняет самое короткое (site 100) */
    n = lat_set_sites(&s, LAT_ISR, out);
    CHECK(n == LAT_SITES && out[0].site == 888 && out[0].max_ns == 9000000);
    for (unsigned i = 0; i < n; i++) CHECK(out[i].site != 100);
    lat_set_reset(&s);
    CHECK(lat_set_sites(&s, LAT_ISR, out) == 0);
}

static void test_conv(void) {
    /* 3 ГГц: 1 такт = 1/3 нс; 3000 тактов = 1000 нс */
    uint64_t mul = lat_mul_for_khz(3000000);
    uint64_t ns = lat_cycles_to_ns(3000, mul);
    CHECK(ns >= 999 && ns <= 1000);
    CHECK(lat_cycles_to_ns(3000000000ULL * 100, mul) >= 99999999000ULL);                 /* 100 с без переполнения */
    CHECK(lat_mul_for_khz(0) == 0);
    /* частота 1 ГГц: такт = нс точно */
    CHECK(lat_cycles_to_ns(12345, lat_mul_for_khz(1000000)) == 12345);
    CHECK(lat_cycles_to_ns(1ULL << 62, lat_mul_for_khz(2400000)) > 0);                    /* 128-битное произведение: не теряется */
}

static void test_names(void) {
    for (int k = 0; k < LAT_KINDS; k++) CHECK(lat_kind_from_name(lat_kind_name((unsigned)k)) == k);
    CHECK(lat_kind_from_name("нет") == -1 && lat_kind_from_name("") == -1 && lat_kind_from_name(0) == -1 && lat_kind_from_name("irqx") == -1 && lat_kind_from_name("ir") == -1);
    CHECK(!strcmp(lat_kind_name(LAT_IRQOFF), "irqoff") && !strcmp(lat_kind_name(99), "?"));
}

#ifdef __SANITIZE_THREAD__
static lat_set_t ts; static lat_cpu_t tmem[2]; static volatile int stop;
static void* writer(void* a) {
    unsigned cpu = (unsigned)(uintptr_t)a;
    for (int i = 0; i < 300000; i++) lat_set_record(&ts, cpu, LAT_ISR, (uint64_t)(i % 5000) + 1, (uint64_t)(i % 7));
    return 0;
}
static void* reader(void* a) {
    (void)a;
    lat_hist_t m; lat_site_t out[LAT_SITES];
    while (!stop) { lat_set_merge(&ts, LAT_ISR, &m); (void)lat_hist_quantile(&m, 999000); (void)lat_set_sites(&ts, LAT_ISR, out); }
    return 0;
}
static void test_threads(void) {
    lat_set_init(&ts, tmem, 2);
    pthread_t w0, w1, r, rs;
    pthread_create(&w0, 0, writer, (void*)0); pthread_create(&w1, 0, writer, (void*)1); pthread_create(&r, 0, reader, 0);
    pthread_create(&rs, 0, (void* (*)(void*))lat_set_reset, &ts);        /* сброс на ходу: ничего не должно ломаться */
    pthread_join(w0, 0); pthread_join(w1, 0); stop = 1; pthread_join(r, 0); pthread_join(rs, 0);
    lat_hist_t m; lat_set_merge(&ts, LAT_ISR, &m);
    CHECK(m.count <= 600000);
}
#endif

int main(int argc, char** argv) {
    (void)argc; (void)argv;
#ifdef __SANITIZE_THREAD__
    if (argc > 1 && !strcmp(argv[1], "--threads")) { test_threads(); printf(fails ? "latency threads: FAIL\n" : "latency threads: PASS\n"); return fails != 0; }
#endif
    test_bins(); test_hist(); test_merge(); test_set(); test_sites(); test_conv(); test_names();
    printf(fails ? "latency: %d FAIL\n" : "latency: PASS\n", fails);
    return fails != 0;
}
