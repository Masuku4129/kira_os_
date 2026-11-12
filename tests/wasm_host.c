/* Интерпретатор WASM (userland/wasm.c) на хосте: системные вызовы подменены, модуль - файл из аргумента.
   Вывод модуля - в stdout (сравнивается с той же программой, собранной обычным компилятором). */
#define KIRA_USER_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
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
static int g_quiet; static int g_count;
static void k_write(const char* s, u64 n) { if (!g_quiet) fwrite(s, 1, n, stdout); }
static void k_sleep(u64 t) { (void)t; }
static u64 k_ticks(void) { return 0; }
static u64 k_clock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u32 g_px[1024 * 768];
static u32* k_gfx_create(u32 w, u32 h) { (void)w; (void)h; return g_px; }
static void k_gfx_present(void) {}
static unsigned g_yields;
static void k_yield(void) { g_yields++; }
/* окно кода JIT на хосте: страницы RW, затем одним вызовом R-X (как SYS_JIT на Кире); освобождаются при сбросе состояния */
static void* g_jwin[64]; static size_t g_jlen[64]; static int g_jn;
static void* k_jit_alloc(u64 pages) {
    if (g_jn >= 64) return 0;
    void* p = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    g_jwin[g_jn] = p; g_jlen[g_jn++] = pages * 4096; return p;
}
static int k_jit_seal(void* p, u64 pages) { if (getenv("WASM_JITDUMP")) { FILE* f = fopen(getenv("WASM_JITDUMP"), "wb"); if (f) { fwrite(p, 1, pages * 4096, f); fclose(f); } } return mprotect(p, pages * 4096, PROT_READ | PROT_EXEC); }
static void jit_free_all(void) { for (int i = 0; i < g_jn; i++) munmap(g_jwin[i], g_jlen[i]); g_jn = 0; }
static int k_gfx_event(kevent_t* e) { (void)e; return 0; }
#define umain wasm_main
#include "../userland/wasm.c"
#ifdef WASM_FUEL
/* Состояние интерпретатора - в исходное, чтобы гонять разбор и выполнение много раз в одном процессе. */
static void wasm_reset(void) {
    ntypes = nfuncs = nimports = ntable = nglobals = 0; table = 0; mem = 0; mem_pages = mem_max = 0; entry = ~0u;
    trapped = 0; trap_msg = 0; heap_end = 0; heap_left = 0; code = 0; cn = ccap = 0; sp = 0; exit_req = 0; exit_code = 0; fb = 0;
    for (u32 i = 0; i < MAXG; i++) globals[i] = 0;
    g_used = 0; jit_free_all();
    wasm_host_calls = 0; wasm_output_bytes = 0; wasm_sleep_ticks = 0;
    wasm_gfx_frames = 0; wasm_gfx_created = 0;
}
static int fuzz_module(const char* path, int iters) {
    FILE* f = fopen(path, "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* orig = malloc((size_t)n); if (fread(orig, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    u8* d = malloc((size_t)n); srand(11); g_quiet = 1; g_path = path;
    int ran = 0, trapsn = 0;
    for (int k = 0; k <= iters; k++) {
        memcpy(d, orig, (size_t)n);
        if (k) {
            int flips = 1 + rand() % 6;
            for (int j = 0; j < flips; j++) {
                int pos = 8 + rand() % (int)(n - 8);
                d[pos] = (k % 3 == 0) ? (u8)rand() : (u8)(d[pos] + 1 - 2 * (rand() & 1));   /* и случайный байт, и соседний код команды */
            }
        }
        g_data = d; g_len = k % 11 == 10 ? 8 + rand() % (int)(n - 8) : n;
        wasm_reset(); wasm_fuel = 3000000;
        wasm_main();
        ran++; trapsn += trapped;
    }
    g_quiet = 0;
    printf("wasm: fuzz %s (%d damaged copies: %d stopped with a trap, the rest finished) passed\n", path, iters, trapsn);
    free(orig); free(d);
    return ran == iters + 1 ? 0 : 1;
}
#endif
int main(int argc, char** argv) {
    if (getenv("WASM_NOJIT")) jit_on = 0;                          /* только интерпретатор (как на ARM64) */
#ifdef WASM_FUEL
    if (argc == 4 && !strcmp(argv[1], "fuzz")) return fuzz_module(argv[2], atoi(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "trap")) {
        FILE *f = fopen(argv[3], "rb");
        if (!f) return 2;
        fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
        if (g_len <= 0) { fclose(f); return 2; }
        g_data = malloc((size_t)g_len);
        if (!g_data || fread(g_data, 1, (size_t)g_len, f) != (size_t)g_len) { fclose(f); return 2; }
        fclose(f);
        g_path = argv[3]; g_quiet = 1; wasm_fuel = 50000000ull;
        (void)wasm_main();
        g_quiet = 0;
        if (trapped && trap_msg && !strcmp(trap_msg, argv[2])) {
            printf("wasm: expected host quota trap: %s\\n", argv[2]);
            return 0;
        }
        fprintf(stderr, "wasm: expected trap %s, got %s\\n", argv[2], trap_msg ? trap_msg : "normal completion");
        return 1;
    }
    if (argc == 4 && !strcmp(argv[1], "fuel")) {
        char *end = NULL;
        unsigned long long limit = strtoull(argv[2], &end, 10);
        if (!end || *end || !limit) return 2;
        FILE *f = fopen(argv[3], "rb");
        if (!f) return 2;
        fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
        if (g_len <= 0) { fclose(f); return 2; }
        g_data = malloc((size_t)g_len);
        if (!g_data || fread(g_data, 1, (size_t)g_len, f) != (size_t)g_len) { fclose(f); return 2; }
        fclose(f);
        g_path = argv[3]; g_quiet = 1; wasm_fuel = (u64)limit;
        (void)wasm_main();
        g_quiet = 0;
        if (trapped && trap_msg && !strcmp(trap_msg, "fuel exhausted")) {
            puts("wasm: bounded fuel exhaustion trapped in selected execution mode");
            return 0;
        }
        fprintf(stderr, "wasm: expected fuel exhaustion, got %s\n", trap_msg ? trap_msg : "normal completion");
        return 1;
    }
#endif
    if (argc < 2) return 2;
#ifdef WASM_FUEL
    if (argc == 3 && !strcmp(argv[1], "count")) { argv++; argc--; wasm_fuel = 1ull << 62; g_count = 1; jit_on = 0; }
#endif
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    g_data = malloc((size_t)g_len); if (fread(g_data, 1, (size_t)g_len, f) != (size_t)g_len) return 2; fclose(f);
    g_path = argv[1];
#ifdef WASM_FUEL
    if (g_count) { g_quiet = 1; int r = wasm_main(); printf("wasm: %llu instructions executed\n", (1ull << 62) - wasm_fuel); return r; }
#endif
    return wasm_main();
}
