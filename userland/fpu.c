/* Живой тест состояния FPU/SSE между процессами. Запускается в нескольких экземплярах одновременно
   (команда fpu в консоли запускает три): каждый считает x87 (80 бит), double и векторы SSE и сверяет каждый
   результат с эталоном. Если ядро не сохраняет регистры при переключении потоков, экземпляры портят друг другу
   вычисления, и результаты расходятся. Раньше тест был потоками ядра. */
#include "kira.h"
KIRA_ENTRY()

typedef float v4sf __attribute__((vector_size(16)));       /* SSE на x86-64, NEON на ARM64 */
#if defined(__aarch64__)
typedef double ext_t;                       /* long double на ARM64 - программная четверная точность: вместо неё ещё double */
#define EXT_ONE 1.0
#define EXT_K   1.0000001
#define EXT_C   0.001
#define FP_NAME "FP/NEON + double"
#else
typedef long double ext_t;                  /* x87, 80 бит */
#define EXT_ONE 1.0L
#define EXT_K   1.0000001L
#define EXT_C   0.001L
#define FP_NAME "x87 + SSE + double"
#endif
typedef struct { u64 w[4]; } digest_t;

#define STEPS 20000
#define RUN_TICKS 300

__attribute__((noinline)) static digest_t work(unsigned id) {
    v4sf a = {1.0f + (float)id, 2.0f, 3.0f + (float)id, 4.0f};
    v4sf k = {1.0001f, 0.9999f, 1.0002f, 0.9998f};
    v4sf c = {0.5f * (float)(id + 1), 0.25f, 0.125f * (float)(id + 1), 0.0625f};
    double d = 1.0 + (double)id;
    float s = 0.5f;
    ext_t ld = EXT_ONE + (ext_t)id;
    for (int i = 0; i < STEPS; i++) {
        a = a * k + c;
        d = d * 1.0000001 + 0.001;
        s = s * 0.999f + (float)(i & 15) * 0.01f;
        ld = ld * EXT_K + EXT_C;
    }
    union { v4sf v; u64 q[2]; } ua; ua.v = a;
    union { double d; u64 q; } ud; ud.d = d;
    union { float f; u32 q; } us; us.f = s;
    union { ext_t l; u64 q[2]; } ul; ul.q[0] = 0; ul.q[1] = 0; ul.l = ld;
    digest_t r;
    r.w[0] = ua.q[0] ^ (ud.q * 0x9E3779B97F4A7C15ULL);
    r.w[1] = ua.q[1] ^ ((u64)us.q << 17);
    r.w[2] = ud.q;
    r.w[3] = ul.q[0];                       /* 64-битная мантисса x87 */
    return r;
}

static int same(const digest_t* x, const digest_t* y) {
    return x->w[0] == y->w[0] && x->w[1] == y->w[1] && x->w[2] == y->w[2] && x->w[3] == y->w[3];
}

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    char o[21]; int i = 0; while (n) o[i++] = t[--n];
    k_write(o, (u64)i);
}

int umain(void) {
    unsigned id = (unsigned)k_getpid();
    digest_t ref = work(id);
    digest_t ref2 = work(id);
    digest_t other = work(id + 1);
    int distinct = !same(&ref, &other);          /* контроль самого теста: разные параметры - разные эталоны */
    u64 rounds = 0, bad = (same(&ref, &ref2) ? 0 : 1);
    say("fpu: " FP_NAME ", every result checked against a reference, 3 s...\n");
    u64 end = k_ticks() + RUN_TICKS;
    while (k_ticks() < end) {
        digest_t r = work(id);
        if (!same(&r, &ref)) bad++;
        rounds++;
    }
    say("fpu: rounds "); say_u(rounds); say(", mismatches "); say_u(bad);
    if (distinct && bad == 0 && rounds > 0) { say(" - PASS: FP/vector state is private to every process\n"); return 0; }
    say(" - FAIL\n");
    return 1;
}
