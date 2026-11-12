// Хост-тест блокирующего мьютекса: честная очередь, передача владения без «обгона», пробуждение, повторный захват.
// Переключение контекста подменено заглушкой: «вернулся из блокировки» означает «поток снова выбран».
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "sched.h"
#include "kmutex.h"
#include "timer.h"

static uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
const void* fpu_initial_state(void) { static uint8_t z[512] __attribute__((aligned(16))); return z; }
void context_switch(uint64_t* old_rsp_slot, uint64_t new_rsp) { (void)old_rsp_slot; (void)new_rsp; }
static jmp_buf escape;
static const char* panic_reason;
void kernel_panic(const char* r) { panic_reason = r; spin_bust_locks(); longjmp(escape, 1); }
void host_halt(void) { longjmp(escape, 2); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void dummy(void* a) { (void)a; }

// Делает t текущим: текущий поток отдаёт остаток кванта (round-robin по обычным потокам)
static void become(thread_t* t) {
    for (int i = 0; i < 12 && sched_current() != t; i++) sched_yield();
    CHECK(sched_current() == t);
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    sched_init(1, 65536);
    kmutex_t m = KMUTEX_INIT;

    // без борьбы: взял - держит - отпустил
    CHECK(!kmutex_held(&m));
    kmutex_lock(&m);
    CHECK(kmutex_held(&m) && m.owner == sched_current());
    CHECK(((thread_t*)sched_current())->mutexes_held == 1);      // сбой с мьютексом изолировать нельзя: учёт
    CHECK(kmutex_trylock(&m) == 0);                              // занят
    kmutex_unlock(&m);
    CHECK(((thread_t*)sched_current())->mutexes_held == 0);
    CHECK(!kmutex_held(&m) && m.owner == 0);
    CHECK(kmutex_trylock(&m) == 1 && kmutex_held(&m));
    kmutex_unlock(&m);

    // три потока: A держит, B и C встают в очередь по порядку, владение передаётся B, потом C
    thread_t* A = sched_create("A", dummy, 0, PRIO_NORMAL);
    thread_t* B = sched_create("B", dummy, 0, PRIO_NORMAL);
    thread_t* C = sched_create("C", dummy, 0, PRIO_NORMAL);
    CHECK(A && B && C);
    sched_sleep_ticks(100000);                                   // "core" уступает: дальше ходят A, B, C (заглушка возвращает сюда же)
    become(A);
    kmutex_lock(&m);
    CHECK(m.owner == A);

    become(B);
    kmutex_lock(&m);                                             // блокируется
    CHECK(B->state == THREAD_BLOCKED && sched_current() != B);
    CHECK(m.head == B && m.tail == B);
    become(C);
    kmutex_lock(&m);
    CHECK(C->state == THREAD_BLOCKED && m.head == B && m.tail == C && B->wait_next == C);

    become(A);
    kmutex_unlock(&m);                                           // владение сразу у B: мьютекс не «свободен» для обгона
    CHECK(m.owner == B && B->state == THREAD_READY && C->state == THREAD_BLOCKED && m.head == C);
    CHECK(kmutex_trylock(&m) == 0);                              // A не может влезть без очереди

    become(B);
    CHECK(kmutex_held(&m));
    kmutex_unlock(&m);
    CHECK(m.owner == C && C->state == THREAD_READY && m.head == 0 && m.tail == 0);
    become(C);
    kmutex_unlock(&m);
    CHECK(m.owner == 0);

    // Чужой поток не может снять мьютекс и испортить счётчик mutexes_held владельца.
    become(A);
    kmutex_lock(&m);
    become(B);
    panic_reason = 0;
    if (setjmp(escape) == 0) kmutex_unlock(&m);
    CHECK(panic_reason && strstr(panic_reason, "non-owner"));
    CHECK(B->mutexes_held == 0);
    become(A);
    CHECK(kmutex_held(&m) && A->mutexes_held == 1);
    kmutex_unlock(&m);
    CHECK(m.owner == 0 && A->mutexes_held == 0);

    // повторный захват тем же потоком - взаимная блокировка: ядро сообщает, а не зависает
    kmutex_lock(&m);
    panic_reason = 0;
    if (setjmp(escape) == 0) kmutex_lock(&m);
    CHECK(panic_reason && strstr(panic_reason, "recursive"));

    printf("%s\n", fails ? "FAILED" : "kmutex: fair queue, ownership hand-off, wake-up, no overtaking, recursion detection passed");
    return fails != 0;
}
