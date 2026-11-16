/* Фаззинг интерпретатора AML под ASAN/UBSan: случайные и обрезанные байты. Цель - ни одного выхода за границы, зависания и падения;
   ошибки разбора и выполнения - нормальный исход. Три вида входа: 1) настоящая таблица (tests/data/acpi/laptop.aml) с портчеными байтами,
   2) она же, обрезанная в случайном месте, 3) чистая случайность и случайные тела методов внутри верной оболочки. После загрузки выполняются
   все методы (аргументы - случайные числа), читаются все значения. Запуск: aml_fuzz [итераций] [зерно] [путь к laptop.aml]. */
#include "aml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }

static uint64_t hw_state;
static uint64_t calls, sleeps;
static int rw(void* u, int w, unsigned sp, uint64_t a, unsigned bits, uint64_t* v, uint32_t pci) {
    (void)u; (void)sp; (void)a; (void)bits; (void)pci;
    if (rnd() % 16 == 0) return -1;
    if (w) hw_state = *v; else *v = hw_state ^ rnd();
    return 0;
}
static void slp(void* u, uint32_t ms) { (void)u; (void)ms; sleeps++; }
static void ntf(void* u, aml_node_t* n, uint32_t v) { (void)u; char p[64]; (void)aml_node_path(n, p, sizeof p); (void)v; }

static void fix_table(uint8_t* t, uint32_t len) {
    t[4] = (uint8_t)len; t[5] = (uint8_t)(len >> 8); t[6] = (uint8_t)(len >> 16); t[7] = (uint8_t)(len >> 24);
    t[9] = 0; uint8_t s = 0; for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + t[i]);
    t[9] = (uint8_t)(0 - s);
}

static aml_ctx_t* C;
static int walk_cb(aml_node_t* n, int depth, void* u) {
    (void)depth; (void)u;
    aml_val_t args[7], out;
    unsigned argc = aml_method_argc(n);
    for (unsigned i = 0; i < argc && i < 7; i++) args[i] = aml_int(rnd() % 4 ? rnd() % 100 : ((uint64_t)rnd() << 32 | rnd()));
    (void)aml_call(C, n, args, argc, &out);
    calls++;
    return 0;
}

static void run_one(const uint8_t* tbl, uint32_t len) {
    static uint8_t arena[1 << 18];
    aml_ops_t ops = { rw, slp, 0, 0, ntf, 0, 0 };
    C = aml_create(arena, sizeof arena, &ops);
    if (!C) { fprintf(stderr, "арена\n"); exit(1); }
    aml_set_limits(C, 20000, 40, 12, 50);
    if (aml_load(C, tbl, len) == 0 || 1) {                  /* даже после ошибки загрузки дерево должно оставаться целым */
        aml_load_done(C);
        aml_walk(C, walk_cb, 0);
        aml_node_t* n = aml_lookup(C, 0, "\\_SB.PCI0._PRT");
        aml_val_t v; if (n) (void)aml_call(C, n, 0, 0, &v);
        aml_stats_t st; aml_stats(C, &st);
    }
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 2000;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;
    const char* path = argc > 3 ? argv[3] : "tests/data/acpi/laptop.aml";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: нет %s\n", path); return 1; }
    static uint8_t good[1 << 16]; size_t gl = fread(good, 1, sizeof good, f); fclose(f);
    static uint8_t buf[1 << 16];
    for (long it = 0; it < iters; it++) {
        uint32_t len = (uint32_t)gl;
        memcpy(buf, good, gl);
        switch (it % 5) {
        case 0: case 1: {                                   /* порча нескольких байт настоящей таблицы */
            unsigned k = 1 + rnd() % 8;
            for (unsigned i = 0; i < k; i++) buf[36 + rnd() % (len - 36)] = (uint8_t)rnd();
            break; }
        case 2: len = 36 + rnd() % (len - 36); break;      /* обрезка */
        case 3: {                                           /* чистая случайность за верным заголовком */
            len = 37 + rnd() % 600;
            for (uint32_t i = 36; i < len; i++) buf[i] = (uint8_t)rnd();
            break; }
        default: {                                          /* Scope(\_SB){ Method(XXXX){ случайное тело } } вокруг случайных байт */
            uint32_t n = 1 + rnd() % 120;
            uint8_t* p = buf + 36;
            *p++ = 0x10; *p++ = (uint8_t)(0x40 | ((n + 17) & 0xF)); *p++ = (uint8_t)((n + 17) >> 4); *p++ = '\\'; *p++ = '_'; *p++ = 'S'; *p++ = 'B'; *p++ = '_';
            *p++ = 0x14; *p++ = (uint8_t)(0x40 | ((n + 7) & 0xF)); *p++ = (uint8_t)((n + 7) >> 4); *p++ = 'F'; *p++ = 'U'; *p++ = 'Z'; *p++ = 'Z'; *p++ = (uint8_t)(rnd() % 8);
            for (uint32_t i = 0; i < n; i++) *p++ = (uint8_t)(rnd() % 3 ? rnd() % 0xA6 : rnd());
            len = (uint32_t)(p - buf);
            break; }
        }
        fix_table(buf, len);
        run_one(buf, len);
        if (it % 500 == 0) { printf("fuzz: %ld/%ld, вызовов %llu\n", it, iters, (unsigned long long)calls); fflush(stdout); }
    }
    printf("aml_fuzz: PASS (%ld входов, вызовов %llu, снов %llu)\n", iters, (unsigned long long)calls, (unsigned long long)sleeps);
    return 0;
}
