// Хост-тест масштабирования: политика размеров и планировщик на N ядрах (N - аргумент: 1, 2, 3, 4, 8, 16, 64).
// Ядра изображены переключением host_cpu; контекстное переключение подменено, меняется только current.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "sched.h"
#include "scale.h"
#include "timer.h"
#include "spinlock.h"
#include "percpu.h"

static uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
const void* fpu_initial_state(void) { static uint8_t z[512] __attribute__((aligned(16))); return z; }
void context_switch(uint64_t* old_rsp_slot, uint64_t new_rsp) { (void)old_rsp_slot; (void)new_rsp; }
void kernel_panic(const char* r) { printf("PANIC: %s\n", r); exit(2); }
void host_halt(void) { exit(3); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void dummy(void* a) { (void)a; }

extern unsigned host_cpu;
static unsigned long long ticks_of[1 << 16];

static void round_all(unsigned n) {
    fake_ticks++;
    for (unsigned c = 0; c < n; c++) {
        host_cpu = c;
        sched_tick();
        sched_irq_exit();
        ticks_of[sched_current()->id]++;
    }
}

static void policy(void) {
    unsigned prev_t = 0, prev_p = 0;
    for (unsigned c = 1; c <= 256; c++) {
        unsigned t = scale_threads(c, 4ULL << 20), p = scale_procs(c, 4ULL << 20);
        CHECK(t >= c + 12);                      // холостые потоки всех ядер и службы всегда помещаются
        CHECK(t >= prev_t && p >= prev_p);       // больше ядер - не меньше ёмкость
        CHECK(p >= c);                           // хотя бы по программе на ядро при достаточной ОЗУ
        prev_t = t; prev_p = p;
    }
    CHECK(scale_cpus(0) == 1 && scale_cpus(200) == 200 && scale_cpus(1000) == SCALE_MAX_CPUS);
    CHECK(scale_threads(64, 64 * 1024) >= 64 + 12);         // мало ОЗУ не отнимает место у холостых потоков
    CHECK(scale_procs(64, 32 * 1024) == 4 && scale_procs(1, 1024) == 2);   // и число программ ограничено памятью
    CHECK(scale_procs(1, 4ULL << 20) == 8 && scale_procs(8, 4ULL << 20) == 32);
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
    if (n < 1 || n > 256) { printf("bad N\n"); return 1; }
    policy();

    for (unsigned c = 0; c < n; c++) percpu_alloc(c);
    host_cpu = 0;
    sched_init(n, 8ULL << 20);
    unsigned cap = sched_thread_capacity();
    CHECK(cap == scale_threads(n, 8ULL << 20));
    for (unsigned c = 1; c < n; c++) {
        host_cpu = c;
        CHECK(sched_attach_cpu() == 0);                    // все ядра помещаются, никто не остался без холостого потока
        CHECK(sched_current()->prio == PRIO_IDLE);
    }
    host_cpu = 0;
    CHECK(sched_cpu_count() == n);

    // n рабочих потоков: каждое ядро, кроме ядра 0 (его занимает критический поток core), получает свой
    thread_t* w[256];
    for (unsigned i = 0; i < n; i++) { w[i] = sched_create("w", dummy, 0, PRIO_NORMAL); CHECK(w[i] != 0); }
    for (unsigned r = 0; r < 200; r++) round_all(n);
    if (n > 1) {
        int distinct = 1;
        for (unsigned a = 1; a < n; a++) {
            host_cpu = a;
            const thread_t* ta = sched_current();
            CHECK(ta->prio == PRIO_NORMAL);                 // у каждого ядра рабочий поток, а не холостой
            for (unsigned b = a + 1; b < n; b++) {
                host_cpu = b;
                if (sched_current() == ta) distinct = 0;
            }
        }
        CHECK(distinct);                                    // один поток не исполняется на двух ядрах
        unsigned ran = 0;
        for (unsigned i = 0; i < n; i++) if (ticks_of[w[i]->id] > 0) ran++;
        CHECK(ran == n);                                    // и лишнему потоку (n-й на n-1 ядре) время тоже достаётся
    }
    host_cpu = 0;

    // Таблица: можно занять всю ёмкость и ни на один слот больше
    unsigned made = 0;
    for (unsigned i = 0; i < cap + 8; i++) if (sched_create("fill", dummy, 0, PRIO_IDLE)) made++;
    CHECK(made == cap - (1 + n + n));                       // минус core, холостые n ядер и n рабочих

    printf("cores=%u capacity=%u procs=%u %s\n", n, cap, scale_procs(n, 8ULL << 20), fails ? "FAILED" : "ok");
    return fails != 0;
}
