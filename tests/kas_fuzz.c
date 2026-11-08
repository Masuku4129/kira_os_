/* Фаззинг kas на повреждённых исходниках: kas_fuzz [-n итераций] [-s зерно] файл.s ...
 * Критерий: ни падений, ни зависаний, ни выхода за арену (под ASan/UBSan); каждая ошибка имеет текст,
 * каждый успех - корректный ELF-заголовок. Мутации детерминированы (зерно печатается). */
#include "../kas/kas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA ((size_t)16 << 20)
static uint64_t rs;
static uint32_t rnd(void) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rs >> 33); }

static const char *TOK[] = {"%macro X 1", "%endmacro", "%if", "%endif", "%define", "%error", "[", "]", "(", ")", "'", "\"", ",", ":", "$", "$$",
    "0xFFFFFFFFFFFFFFFF", "99999999999999999999", "-", "*", "<<", "rax", "r8d", "ah", "cr0", "gs:", "rel", "abs", "qword", "short", "near",
    "times 99999999", "resb 99999999", "equ", "jmp", "call", "push", "mov", "section", "bits 16", "bits 32", "bits 64", "align 3", "\t", "..@1.", "%1", "%%x", ".l", "wrt ..plt"};

static size_t mutate(char *b, size_t n, size_t cap) {
    int k = 1 + (int)(rnd() % 4);
    while (k--) {
        uint32_t op = rnd() % 7;
        if (!n) { b[0] = 'x'; n = 1; }
        size_t p = rnd() % n;
        if (op == 0) b[p] = (char)rnd();
        else if (op == 1) { size_t l = rnd() % 16; if (p + l > n) l = n - p; memmove(b + p, b + p + l, n - p - l); n -= l; }
        else if (op == 2) { n = p; }
        else if (op == 3 || op == 4) {
            const char *t = TOK[rnd() % (sizeof TOK / sizeof TOK[0])];
            size_t l = strlen(t);
            if (n + l + 1 < cap) { memmove(b + p + l, b + p, n - p); memcpy(b + p, t, l); n += l; }
        } else if (op == 5) {
            size_t a = p, e = p; while (a > 0 && b[a - 1] != '\n') a--; while (e < n && b[e] != '\n') e++;
            size_t l = e - a;
            if (n + l + 1 < cap) { memmove(b + e + 1 + l, b + e, n - e); b[e] = '\n'; memcpy(b + e + 1, b + a, l); n += l + 1; }
        } else { b[p] = (char)("0123456789abcdefx-+*[](),:'\" \n;%$."[rnd() % 33]); }
    }
    return n;
}

int main(int argc, char **argv) {
    long iters = 2000; uint64_t seed = 12345; int first = 1;
    for (; first < argc && argv[first][0] == '-'; first += 2) {
        if (first + 1 >= argc) { fprintf(stderr, "kas_fuzz: ключ без значения\n"); return 2; }
        if (!strcmp(argv[first], "-n")) iters = atol(argv[first + 1]);
        else if (!strcmp(argv[first], "-s")) seed = strtoull(argv[first + 1], NULL, 10);
        else { fprintf(stderr, "kas_fuzz: неизвестный ключ %s\n", argv[first]); return 2; }
    }
    if (first >= argc) { fprintf(stderr, "kas_fuzz: нет входных файлов\n"); return 2; }
    uint8_t *mem = malloc(ARENA);
    char *buf = malloc(1 << 20);
    if (!mem || !buf) return 2;
    long ok = 0, bad = 0, total = 0;
    printf("kas_fuzz: зерно %llu, итераций на файл %ld\n", (unsigned long long)seed, iters);
    for (int a = first; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { fprintf(stderr, "kas_fuzz: не открыть %s\n", argv[a]); return 2; }
        size_t n0 = fread(buf, 1, (1 << 20) - 4096, f);
        fclose(f);
        rs = seed;
        char *w = malloc(1 << 20);
        for (long it = 0; it < iters; it++) {
            memcpy(w, buf, n0);
            size_t n = it ? mutate(w, n0, (1 << 20) - 1) : n0;
            KasArena ar;
            kas_arena_init(&ar, mem, ARENA);
            const uint8_t *out = NULL; size_t ol = 0; char err[400] = "";
            int r = kas_assemble(&ar, argv[a], w, n, &out, &ol, err, sizeof err);
            total++;
            if (r == 0) {
                if (ol < 64 || memcmp(out, "\x7f" "ELF", 4)) { fprintf(stderr, "FAIL: %s итерация %ld: успех без ELF\n", argv[a], it); return 1; }
                ok++;
            } else {
                if (!err[0]) { fprintf(stderr, "FAIL: %s итерация %ld: ошибка без текста\n", argv[a], it); return 1; }
                bad++;
            }
        }
        free(w);
    }
    printf("kas_fuzz: прогонов %ld, собралось %ld, отклонено с диагностикой %ld, падений 0\n", total, ok, bad);
    free(mem); free(buf);
    return 0;
}
