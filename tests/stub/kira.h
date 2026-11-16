/* Подмена userland/kira.h для хост-теста: те же имена, но вызовы идут прямо в файловый сервис ядра. */
#ifndef KIRA_USER_H
#define KIRA_USER_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "fsrv.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
typedef struct { u32 type, a, b, c; } kevent_t;
enum { KEV_KEY_DOWN = 1, KEV_KEY_UP = 2, KEV_MOUSE_MOVE = 3, KEV_MOUSE_BUTTON = 4, KEV_CLOSE = 5 };
extern char stub_log[4096];
static inline void k_write(const char* s, u64 n) { strncat(stub_log, s, n); }
#define STUB_UNUSED __attribute__((unused))
/* Управляемая «система»: тест решает, дать ли окно и кучу, когда закрыть окно, как идёт время. */
STUB_UNUSED static int stub_gfx_ok;                        /* 1 - k_gfx_create даёт окно */
STUB_UNUSED static int stub_heap_ok = 1;                   /* 0 - k_heap_grow отказывает */
STUB_UNUSED static u64 stub_last_heap_pages;
STUB_UNUSED static unsigned stub_ev_calls, stub_close_at = ~0u, stub_presents;
STUB_UNUSED static u64 stub_ticks_now, stub_ticks_step = 1;   /* каждый вызов k_ticks двигает время */
STUB_UNUSED static u32 stub_pid = 3;
STUB_UNUSED static u64 stub_heap_max_pages;                  /* >0: k_heap_grow отказывает на запросах больше этого числа страниц */
STUB_UNUSED static char stub_args[96];                         /* строка аргументов запуска */
static inline void k_sleep(u64 t) { (void)t; }
static inline u64 k_ticks(void) { return stub_ticks_now += stub_ticks_step; }
static inline u64 k_getpid(void) { return stub_pid; }
static inline u64 k_strlen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static inline void* k_heap_grow(u64 pages) { stub_last_heap_pages = pages; return stub_heap_ok && (!stub_heap_max_pages || pages <= stub_heap_max_pages) ? calloc((size_t)pages, 4096) : 0; }
static inline u32* k_gfx_create(u32 w, u32 h) { return stub_gfx_ok ? (u32*)calloc((size_t)w * h, 4) : 0; }
static inline void k_gfx_present(void) { stub_presents++; }
static inline int k_gfx_event(kevent_t* e) {
    if (++stub_ev_calls == stub_close_at) { e->type = KEV_CLOSE; e->a = e->b = e->c = 0; return 1; }
    return 0;
}
static inline i64 stub_fs(int op, const char* name, void* buf, u64 len, u64 off) {
    static fsrv_req_t r; static uint8_t rb[1u << 20];
    memset(&r, 0, sizeof r); r.data = rb; strcpy(r.prog, "paint"); r.op = (u32)op; if (name) strncpy(r.name, name, FSRV_NAME_MAX);
    r.off = off; r.len = len;
    if (op == FS_OP_WRITE && len <= sizeof rb) memcpy(r.data, buf, len);
    fsrv_execute(&r);
    if (op == FS_OP_READ && r.result > 0) memcpy(buf, r.data, (size_t)r.result);
    return r.result;
}
static inline i64 k_file_read(const char* n, void* b, u64 cap, u64 off) { return stub_fs(FS_OP_READ, n, b, cap, off); }
static inline i64 k_file_write(const char* n, const void* b, u64 len) { return stub_fs(FS_OP_WRITE, n, (void*)b, len, 0); }
/* args и часы (потоки и futex - в stub/kira_threads.h: pthread.h не дружит с -I. и _GNU_SOURCE у части тестов) */
static inline i64 k_args(char* buf, u64 cap) { u64 n = strlen(stub_args); if (!n) return -1; if (n >= cap) n = cap - 1; memcpy(buf, stub_args, n); buf[n] = 0; return (i64)n; }
static inline u64 k_clock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
#define KIRA_ENTRY()
#endif
