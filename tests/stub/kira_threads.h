/* Потоки процесса и futex для хост-тестов программ: pthread и futex Linux (то же поведение, что у системных вызовов Kira).
   Подключать после stub/kira.h и ТОЛЬКО без _GNU_SOURCE (в корне репозитория есть свой sched.h, он закрывает системный для pthread.h). */
#ifndef KIRA_STUB_THREADS_H
#define KIRA_STUB_THREADS_H
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#define K_FOREVER 0xFFFFFFFFull
typedef struct { void (*fn)(u64); u64 arg; } stub_thread_t;
static void* stub_thread_main(void* p) { stub_thread_t s = *(stub_thread_t*)p; free(p); s.fn(s.arg); return 0; }
static inline i64 k_thread_create(void (*fn)(u64), void* stack, u64 size, u64 arg) {
    (void)stack; (void)size;
    stub_thread_t* s = (stub_thread_t*)malloc(sizeof *s); s->fn = fn; s->arg = arg;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t; int rc = pthread_create(&t, &at, stub_thread_main, s); pthread_attr_destroy(&at);
    if (rc) { free(s); return -7; }
    return 1;
}
static inline i64 k_futex_wait(volatile u32* w, u32 expected, u64 timeout) { (void)timeout; syscall(SYS_futex, (u32*)w, FUTEX_WAIT_PRIVATE, expected, 0, 0, 0); return 0; }
static inline i64 k_futex_wake(volatile u32* w, u32 n) { syscall(SYS_futex, (u32*)w, FUTEX_WAKE_PRIVATE, n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n, 0, 0, 0); return 0; }
#endif
