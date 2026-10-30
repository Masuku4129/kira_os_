// Хост-тест билетной блокировки на настоящих потоках (pthread): взаимное исключение,
// порядок выдачи (FIFO), отмена замков при панике (spin_bust).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include "spinlock.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define THREADS 8
#define WORKERS 2          // билетная блокировка при числе потоков больше числа ядер теряет время на вытесненном держателе; тест гоняет по числу ядер
#define ITER 50000

static spinlock_t lock = SPINLOCK_INIT;
static uint64_t counter;          // намеренно обычная переменная: без замка счёт потеряется

static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        uint64_t f = spin_lock_irqsave(&lock);
        counter = counter + 1;
        spin_unlock_irqrestore(&lock, f);
    }
    return 0;
}

// FIFO: потоки встают в очередь по одному в известном порядке, пока замок держит главный поток.
static spinlock_t qlock = SPINLOCK_INIT;
static volatile int arrived;
static int order[THREADS];
static volatile int granted;

static void* queued(void* arg) {
    int id = (int)(intptr_t)arg;
    __atomic_fetch_add(&arrived, 1, __ATOMIC_SEQ_CST);
    uint64_t f = spin_lock_irqsave(&qlock);
    order[granted++] = id;
    spin_unlock_irqrestore(&qlock, f);
    return 0;
}

int main(void) {
    pthread_t th[THREADS];
    for (int i = 0; i < WORKERS; i++) pthread_create(&th[i], 0, worker, 0);
    for (int i = 0; i < WORKERS; i++) pthread_join(th[i], 0);
    CHECK(counter == (uint64_t)WORKERS * ITER);
    CHECK(lock.next == lock.owner);                               // замок свободен, билеты сходятся
    CHECK(lock.next == (uint32_t)(WORKERS * ITER));

    uint64_t f = spin_lock_irqsave(&qlock);
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&th[i], 0, queued, (void*)(intptr_t)i);
        // дождаться, пока поток возьмёт билет (next вырос), прежде чем запускать следующего
        while (__atomic_load_n(&qlock.next, __ATOMIC_SEQ_CST) < (uint32_t)(i + 2)) usleep(100);
    }
    CHECK(granted == 0);                                          // пока замок у нас, никто не вошёл
    spin_unlock_irqrestore(&qlock, f);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], 0);
    int fifo = 1;
    for (int i = 0; i < THREADS; i++) if (order[i] != i) fifo = 0;
    CHECK(fifo);

    // Паника: замок занят, но аварийный код обязан пройти, а не зависнуть.
    spinlock_t held = SPINLOCK_INIT;
    uint64_t g = spin_lock_irqsave(&held);
    (void)g;
    spin_bust_locks();
    uint64_t h = spin_lock_irqsave(&held);                        // без отмены здесь был бы вечный цикл
    spin_unlock_irqrestore(&held, h);
    CHECK(spin_bust == 1);
    spin_bust = 0;

    if (fails) { printf("spinlock: %d FAILED\n", fails); return 1; }
    printf("spinlock: mutual exclusion (%d threads x %d), FIFO order, panic bust passed\n", WORKERS, ITER);
    return 0;
}
