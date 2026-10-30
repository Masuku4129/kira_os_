// Хост-тест НАСТОЯЩЕГО переключения контекста: sched.c + реальный context.s
// + реальные fxsave/fxrstor. Три потока держат значения в xmm0..xmm3 и вызывают
// "тик таймера" прямо из asm-вставки, не сохраняя xmm (как настоящее прерывание, которое
// обязано оставить регистры нетронутыми). Если ядро неверно сохраняет состояние SSE
// между потоками, результаты расходятся с эталоном.
//
//   ./ctx_fpu_test        - ожидаем PASS
//   ./ctx_fpu_test --nofpu - fxsave/fxrstor отключены: тест ДОЛЖЕН поймать порчу
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sched.h"
#include "fpu.h"

uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
void kernel_panic(const char* r) { printf("PANIC: %s\n", r); exit(3); }
extern void tick_hook(void);
void host_halt(void) { tick_hook(); } // "hlt": время идёт до следующего прерывания
static uint8_t init_state[512] __attribute__((aligned(16)));
const void* fpu_initial_state(void) { return init_state; }

// tick_hook() определён в tests/tick_hook.c и собирается без SSE (как ядро).
extern void tick_hook(void);

#define ROUNDS 40
#define STEPS  3000
#define NW 3

typedef struct { float v[4] __attribute__((aligned(16))); } v4f;
static volatile int done[NW];
static volatile int bad[NW];
static volatile unsigned long long rounds_done[NW];
static v4f result[NW][ROUNDS];
static v4f expected[NW];

// Эталонное вычисление без вмешательства планировщика
static void compute(unsigned id, v4f* out) {
    v4f a = {{1.0f + id, 2.0f, 3.0f + id, 4.0f}};
    v4f k = {{1.0001f, 0.9999f, 1.0002f, 0.9998f}};
    v4f c = {{0.5f * (id + 1), 0.25f, 0.125f * (id + 1), 0.0625f}};
    __asm__ volatile ("movaps (%0), %%xmm0\n\tmovaps (%1), %%xmm1\n\tmovaps (%2), %%xmm2" : : "r"(&a), "r"(&k), "r"(&c) : "xmm0", "xmm1", "xmm2");
    for (int i = 0; i < STEPS; i++) {
        __asm__ volatile ("mulps %%xmm1, %%xmm0\n\taddps %%xmm2, %%xmm0" : : : "xmm0");
    }
    __asm__ volatile ("movaps %%xmm0, (%0)" : : "r"(out) : "memory");
}

static void worker(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    // выравнивание стека нового потока: на входе rsp = 8 (mod 16), после push rbp -> rbp = 0 (mod 16)
    if (((uintptr_t)__builtin_frame_address(0)) % 16 != 0) { printf("worker %u: bad stack alignment\n", id); bad[id] = 1000; }

    for (int r = 0; r < ROUNDS; r++) {
        v4f a = {{1.0f + id, 2.0f, 3.0f + id, 4.0f}};
        v4f k = {{1.0001f, 0.9999f, 1.0002f, 0.9998f}};
        v4f c = {{0.5f * (id + 1), 0.25f, 0.125f * (id + 1), 0.0625f}};
        __asm__ volatile ("movaps (%0), %%xmm0\n\tmovaps (%1), %%xmm1\n\tmovaps (%2), %%xmm2" : : "r"(&a), "r"(&k), "r"(&c) : "xmm0", "xmm1", "xmm2");
        for (int i = 0; i < STEPS; i++) {
            __asm__ volatile ("mulps %%xmm1, %%xmm0\n\taddps %%xmm2, %%xmm0" : : : "xmm0");
            if (i % 7 == 0) {
                // "прерывание": вызов без сохранения xmm; rsp здесь выровнен по 16 (non-leaf, без red zone)
                __asm__ volatile ("call tick_hook"
                                  : : : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
            }
        }
        __asm__ volatile ("movaps %%xmm0, (%0)" : : "r"(&result[id][r]) : "memory");
        rounds_done[id]++;
    }
    for (int r = 0; r < ROUNDS; r++)
        if (memcmp(&result[id][r], &expected[id], sizeof(v4f)) != 0) bad[id]++;
    done[id] = 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    int nofpu = argc > 1 && !strcmp(argv[1], "--nofpu");

    // шаблон начального состояния: чистый FPU
    __asm__ volatile ("fninit");
    uint32_t mx = 0x1F80; __asm__ volatile ("ldmxcsr %0" : : "m"(mx));
    __asm__ volatile ("fxsave (%0)" : : "r"(init_state) : "memory");

    for (unsigned i = 0; i < NW; i++) compute(i, &expected[i]);

    sched_init(1, 65536);
    sched_set_quantum(2);
    for (unsigned i = 0; i < NW; i++) {
        char nm[8] = {'w', (char)('0' + i), 0};
        if (!sched_create(nm, worker, (void*)(uintptr_t)i, PRIO_NORMAL)) { printf("create failed\n"); return 2; }
    }
    // core (main) отдаёт процессор воркерам: засыпает; пробуждаем тиками из воркеров
    unsigned long long guard = 0;
    for (;;) {
        int all = 1;
        for (int i = 0; i < NW; i++) if (!done[i]) all = 0;
        if (all) break;
        if (++guard > 100000000ULL) { printf("timeout\n"); return 2; }
        sched_sleep_ticks(1);
    }

    unsigned long long sw = sched_total_switches();
    int total_bad = 0;
    for (int i = 0; i < NW; i++) total_bad += bad[i];
    printf("%s: context switches=%llu, workers=%d, rounds each=%d, mismatches=%d\n",
           nofpu ? "no-FPU-save control" : "real FPU save/restore", sw, NW, ROUNDS, total_bad);
    if (nofpu) { printf(total_bad ? "CONTROL OK: corruption detected without FPU save\n" : "CONTROL FAILED: test is blind\n"); return total_bad ? 0 : 1; }
    printf(total_bad == 0 && sw > 500 ? "PASS\n" : "FAIL\n");
    return !(total_bad == 0 && sw > 500);
}
