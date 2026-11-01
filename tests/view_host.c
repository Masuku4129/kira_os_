/* Разбор форматов программы просмотра на хосте: view.c с подменой системных вызовов, суммы пикселей - против
   эталона ci/mkfiles.py (файлы и суммы передаются аргументами: путь сумма ...). */
#define KIRA_USER_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
typedef struct { u32 type, a, b, c; } kevent_t;
enum { KEV_KEY_DOWN = 1, KEV_KEY_UP = 2, KEV_MOUSE_MOVE = 3, KEV_MOUSE_BUTTON = 4, KEV_CLOSE = 5 };
#define KIRA_ENTRY()
static const char* g_path; static u8* g_data; static long g_len;
static char g_log[512];
static u8* g_arena; static u64 g_used;                           /* куски подряд, как у ядра */
static void* k_heap_grow(u64 pages) { if (g_used + pages * 4096 > (8u << 20)) return 0; void* p = g_arena + g_used; g_used += pages * 4096; return p; }
static i64 k_grant_name(char* b, u64 cap) { snprintf(b, cap, "%s", g_path); return (i64)strlen(b); }
static i64 k_grant_size(void) { return g_len; }
static i64 k_grant_read(void* b, u64 len, u64 off) { if ((long)off >= g_len) return 0; if ((long)(off + len) > g_len) len = (u64)(g_len - (long)off); memcpy(b, g_data + off, len); return (i64)len; }
static u32 g_px[720 * 540];
static u32* k_gfx_create(u32 w, u32 h) { (void)w; (void)h; return g_px; }
static void k_gfx_present(void) {}
static int k_gfx_event(kevent_t* e) { e->type = KEV_CLOSE; return 1; }
static void k_write(const char* s, u64 n) { snprintf(g_log, sizeof g_log, "%.*s", (int)n, s); }
static void k_sleep(u64 t) { (void)t; }
#define umain view_main
#include "../userland/view.c"
static void run_one(const char* path, u8* data, long len) {
    g_path = path; g_data = data; g_len = len;
    if (!g_arena) g_arena = calloc(8u << 20, 1);
    g_used = 0; heap_end = 0; heap_left = 0; err[0] = 0; info[0] = 0; img = 0; mode = 0; scroll = 0;
    view_main();
}
int main(int argc, char** argv) {
    int fails = 0;
    if (argc == 3 && !strcmp(argv[1], "fuzz")) {             /* испорченные файлы: разбор не должен падать */
        FILE* f = fopen(argv[2], "rb"); if (!f) return 1;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        u8* orig = malloc((size_t)n); if (fread(orig, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
        u8* d = malloc((size_t)n); srand(7);
        for (int k = 0; k < 3000; k++) {
            memcpy(d, orig, (size_t)n);
            int flips = 1 + rand() % 8;
            for (int j = 0; j < flips; j++) d[8 + rand() % (n - 8)] = (u8)rand();
            run_one(argv[2], d, k % 7 == 0 ? rand() % n : n);  /* и обрезанные */
        }
        printf("view: fuzz %s (3000 damaged copies) passed\n", argv[2]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "cmp")) {               /* cmp файл эталон.rgb ...: пиксели против декодера libjpeg */
        for (int i = 2; i + 1 < argc; i += 2) {
            FILE* f = fopen(argv[i], "rb"); if (!f) return 1;
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            u8* d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
            run_one(argv[i], d, n);
            FILE* r = fopen(argv[i + 1], "rb"); if (!r) return 1;
            u32 rw = 0, rh = 0; if (fread(&rw, 4, 1, r) != 1 || fread(&rh, 4, 1, r) != 1) return 1;
            if (err[0] || rw != iw || rh != ih) { printf("FAIL %s: %s, %ux%u vs %ux%u\n", argv[i], err, iw, ih, rw, rh); fails++; fclose(r); continue; }
            u64 tot = 0; int mx = 0;
            for (u32 k = 0; k < iw * ih; k++) {
                u8 c[3]; if (fread(c, 1, 3, r) != 3) return 1;
                int e[3] = { (int)(img[k] >> 16) - c[0], (int)((img[k] >> 8) & 255) - c[1], (int)(img[k] & 255) - c[2] };
                for (int j = 0; j < 3; j++) { int a = e[j] < 0 ? -e[j] : e[j]; tot += (u64)a; if (a > mx) mx = a; }
            }
            fclose(r);
            double mean = (double)tot / (3.0 * iw * ih);
            int ok = mean < 1.0 && mx <= 8;               /* IDCT и пересчёт цвета - не побитово как в libjpeg */
            printf("%s %s: %s, mean error %.2f, max %d\n", ok ? "ok  " : "FAIL", argv[i], info, mean, mx);
            if (!ok) fails++;
            free(d);
        }
        printf("%s\n", fails ? "VIEW JPEG FAILED" : "view: JPEG baseline (4:2:0, 4:2:2, 4:4:4 + restart, grayscale) matches libjpeg within IDCT rounding - passed");
        return fails != 0;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        FILE* f = fopen(argv[i], "rb"); if (!f) { printf("FAIL open %s\n", argv[i]); return 1; }
        fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
        g_data = malloc((size_t)g_len); if (fread(g_data, 1, (size_t)g_len, f) != (size_t)g_len) return 1; fclose(f);
        g_path = argv[i];
        if (!g_arena) g_arena = calloc(8u << 20, 1);
        memset(g_arena, 0, 8u << 20); g_used = 0;
        heap_end = 0; heap_left = 0; err[0] = 0; info[0] = 0; img = 0; mode = 0; scroll = 0;
        view_main();
        const char* want = argv[i + 1];
        int ok = strcmp(want, "-") ? strstr(g_log, want) != 0 : strstr(g_log, " sum ") == 0 && !err[0];
        printf("%s %s", ok ? "ok  " : "FAIL", g_log);
        if (!ok) { printf("     want: %s\n", want); fails++; }
        free(g_data);
    }
    printf("%s\n", fails ? "VIEW FAILED" : "view: PNG (filters, dynamic/stored), BMP, PPM, GIF (LZW resets, interlace, local palette), text, binary passed");
    return fails != 0;
}
