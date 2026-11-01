/* Интерпретатор WASM (userland/wasm.c) на хосте: системные вызовы подменены, модуль - файл из аргумента.
   Вывод модуля - в stdout (сравнивается с той же программой, собранной обычным компилятором). */
#define KIRA_USER_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
typedef struct { u32 type, a, b, c; } kevent_t;
#define KIRA_ENTRY()
static const char* g_path; static u8* g_data; static long g_len;
static u8* g_arena; static u64 g_used;
static void* k_heap_grow(u64 pages) { if (!g_arena) g_arena = calloc(64u << 20, 1); if (g_used + pages * 4096 > (64u << 20)) return 0; void* p = g_arena + g_used; g_used += pages * 4096; return p; }
static i64 k_grant_name(char* b, u64 cap) { snprintf(b, cap, "%s", g_path); return (i64)strlen(b); }
static i64 k_grant_size(void) { return g_len; }
static i64 k_grant_read(void* b, u64 len, u64 off) { if ((long)off >= g_len) return 0; if ((long)(off + len) > g_len) len = (u64)(g_len - (long)off); memcpy(b, g_data + off, len); return (i64)len; }
static u64 k_strlen(const char* s) { return strlen(s); }
static void k_write(const char* s, u64 n) { fwrite(s, 1, n, stdout); }
static void k_sleep(u64 t) { (void)t; }
static u64 k_ticks(void) { return 0; }
static u64 k_clock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u32 g_px[1024 * 768];
static u32* k_gfx_create(u32 w, u32 h) { (void)w; (void)h; return g_px; }
static void k_gfx_present(void) {}
static int k_gfx_event(kevent_t* e) { (void)e; return 0; }
#define umain wasm_main
#include "../userland/wasm.c"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    g_data = malloc((size_t)g_len); if (fread(g_data, 1, (size_t)g_len, f) != (size_t)g_len) return 2; fclose(f);
    g_path = argv[1];
    return wasm_main();
}
