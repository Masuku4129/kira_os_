// Реальный host-тест kmutex: настоящий scheduler и x86-64 context switch.
// Два ожидающих потока действительно блокируются и пробуждаются передачей владения.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdalign.h>
#include "sched.h"
#include "kmutex.h"
#include "timer.h"

uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
void uart_print(const char *s) { if (s) fputs(s, stdout); }
void uart_print_dec(uint64_t v) { printf("%llu", (unsigned long long)v); }
void uart_print_hex(uint64_t v) { printf("0x%llx", (unsigned long long)v); }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
static alignas(16) uint8_t initial_fpu_state[512];
const void *fpu_initial_state(void) { return initial_fpu_state; }
void kernel_panic(const char *reason) {
    fprintf(stderr, "unexpected kernel panic in kmutex host test: %s\n", reason ? reason : "unknown");
    abort();
}
void host_halt(void) {
    fputs("unexpected idle halt while kmutex test is active\n", stderr);
    abort();
}

static volatile unsigned b_waiting, c_waiting, b_done, c_done;
static volatile unsigned handoff_order[2], handoff_count;
static kmutex_t mutex = KMUTEX_INIT;
static int failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failed = 1; } } while (0)

static void owner(void *arg) {
    (void)arg;
    kmutex_lock(&mutex);
    CHECK(kmutex_held(&mutex));
    while (!b_waiting || !c_waiting) sched_yield();
    CHECK(mutex.owner == sched_current());
    CHECK(mutex.head != 0 && mutex.tail != 0 && mutex.head != mutex.tail);
    CHECK(mutex.head->wait_next == mutex.tail);
    kmutex_unlock(&mutex);
    while (!b_done || !c_done) sched_yield();
    CHECK(mutex.owner == 0 && mutex.head == 0 && mutex.tail == 0);
}

static void waiter_b(void *arg) {
    (void)arg;
    b_waiting = 1;
    kmutex_lock(&mutex);
    handoff_order[handoff_count++] = 1;
    kmutex_unlock(&mutex);
    b_done = 1;
}

static void waiter_c(void *arg) {
    (void)arg;
    c_waiting = 1;
    kmutex_lock(&mutex);
    handoff_order[handoff_count++] = 2;
    kmutex_unlock(&mutex);
    c_done = 1;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    __asm__ volatile ("fninit");
    uint32_t mxcsr = 0x1f80;
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile ("fxsave (%0)" : : "r"(initial_fpu_state) : "memory");
    sched_init(1, 65536);
    sched_set_quantum(2);
    CHECK(sched_create("owner", owner, 0, PRIO_NORMAL) != 0);
    CHECK(sched_create("waiter-b", waiter_b, 0, PRIO_NORMAL) != 0);
    CHECK(sched_create("waiter-c", waiter_c, 0, PRIO_NORMAL) != 0);
    while (!b_done || !c_done || handoff_count != 2) sched_yield();
    CHECK(handoff_order[0] == 1 && handoff_order[1] == 2);
    CHECK(mutex.owner == 0 && mutex.head == 0 && mutex.tail == 0);
    printf("%s\n", failed ? "FAILED" : "kmutex: real context switching, FIFO blocking, direct ownership hand-off and wake-up passed");
    return failed;
}
