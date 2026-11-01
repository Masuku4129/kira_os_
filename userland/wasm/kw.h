/* Программы WebAssembly для Киры: один файл .wasm работает на x86-64 и ARM64 (интерпретатор - программа wasm).
   Импорт - модуль "kira"; точка входа - экспортируемая kmain(). f32/f64 поддержаны (IEEE 754).
   Сборка: clang --target=wasm32 -O2 -nostdlib -c prog.c; wasm-ld --no-entry --export=kmain ... (см. build_clang.sh).
   С KW_NATIVE тот же код собирается обычным компилятором хоста - эталон для проверки интерпретатора. */
#ifndef KW_H
#define KW_H
#ifdef KW_NATIVE
#include <stdio.h>
#include <stdlib.h>
static inline int kw_write(const char* s, int n) { return (int)fwrite(s, 1, (size_t)n, stdout); }
static inline void kw_sleep(int t) { (void)t; }
static inline long long kw_ticks(void) { return 0; }
static inline void kw_exit(int c) { exit(c); }
static inline int kw_gfx_create(int w, int h) { (void)w; (void)h; return 0; }   /* эталон: окна нет, кадры только считаются */
static inline int kw_gfx_present(const unsigned* px) { (void)px; return 0; }
static inline int kw_gfx_event(void* ev) { (void)ev; return 0; }
#else
#define KW_IMPORT(n) __attribute__((import_module("kira"), import_name(n)))
KW_IMPORT("write") int kw_write(const char* s, int n);
KW_IMPORT("sleep") void kw_sleep(int ticks);
KW_IMPORT("ticks") long long kw_ticks(void);
KW_IMPORT("exit") void kw_exit(int code);
KW_IMPORT("gfx_create") int kw_gfx_create(int w, int h);
KW_IMPORT("gfx_present") int kw_gfx_present(const unsigned* px);
KW_IMPORT("gfx_event") int kw_gfx_event(void* ev16);
/* компилятор может вызвать их сам (копирование структур) */
void* memcpy(void* d, const void* s, unsigned long n);
void* memset(void* d, int c, unsigned long n);
#endif
static inline int kw_len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static inline void kw_say(const char* s) { kw_write(s, kw_len(s)); }
static inline void kw_say_i(long long v) {
    char t[24]; int i = 23; unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v; t[i] = 0;
    do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) t[--i] = '-';
    kw_say(t + i);
}
static inline void kw_say_x(unsigned long long v) {
    char t[17]; for (int i = 15; i >= 0; i--) { t[i] = "0123456789abcdef"[v & 15]; v >>= 4; } t[16] = 0; kw_say(t);
}
#endif
