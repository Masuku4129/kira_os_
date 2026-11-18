// Платформа хоста для движка k3d: потоки pthread и futex Linux (то же, что на Kira даёт k_thread_create/k_futex_*).
// Общий для хост-теста (tests/k3d_test.c) и замеров (tests/bench/bench_k3d.c).
#ifndef K3D_HOST_H
#define K3D_HOST_H
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "../userland/k3d.h"

typedef struct { void (*fn)(k3d_u64); k3d_u64 arg; } host_start_t;
static int host_spawn_fail_after = -1;   // -1: не отказывать

static void* host_thread_main(void* p) { host_start_t s = *(host_start_t*)p; free(p); s.fn(s.arg); return 0; }
static int host_spawn(void (*fn)(k3d_u64), void* stack, k3d_u64 size, k3d_u64 arg) {
    (void)stack; (void)size;
    if (host_spawn_fail_after == 0) return -1;
    if (host_spawn_fail_after > 0) host_spawn_fail_after--;
    host_start_t* s = (host_start_t*)malloc(sizeof *s); s->fn = fn; s->arg = arg;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);   // выход потока движок подтверждает сам (exited), join не нужен
    pthread_t t; int rc = pthread_create(&t, &at, host_thread_main, s);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(s); return -1; }
    return 0;
}
static void host_futex_wait(volatile k3d_u32* w, k3d_u32 expected) { syscall(SYS_futex, (k3d_u32*)w, FUTEX_WAIT_PRIVATE, expected, 0, 0, 0); }
static void host_futex_wake(volatile k3d_u32* w, k3d_u32 n) { syscall(SYS_futex, (k3d_u32*)w, FUTEX_WAKE_PRIVATE, n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n, 0, 0, 0); }
static k3d_u64 host_clock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (k3d_u64)t.tv_sec * 1000000000ull + (k3d_u64)t.tv_nsec; }
static void host_sys(k3d_sys_t* s) { s->spawn = host_spawn; s->futex_wait = host_futex_wait; s->futex_wake = host_futex_wake; s->clock_ns = host_clock_ns; }

// Движок с памятью из malloc (выровненной); mem освобождает вызывающий.
static k3d_ctx* host_create(const k3d_config_t* cfg, void** mem) {
    k3d_u64 n = k3d_mem_size(cfg); if (!n) { *mem = 0; return 0; }
    *mem = aligned_alloc(64, (n + 63) & ~63ull); memset(*mem, 0xA5, n);                 // мусор в памяти: движок обязан сам всё инициализировать
    return k3d_create(cfg, *mem, n);
}
#endif
