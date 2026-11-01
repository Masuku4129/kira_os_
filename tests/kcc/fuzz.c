/* Fuzz kcc: испорченные исходники корпуса не должны ронять компилятор (ASAN/UBSAN) и не должны его зависать.
 * Результат каждого запуска - либо успех, либо диагностика вида "файл:строка: error: ...". */
#include "../../kcc/kcc.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *cur_src;
static size_t cur_len;
static char hbuf[16][16384];
static int hnext;

static const char *rd(void *ud, const char *path, size_t *len) {
    (void)ud;
    if (!strcmp(path, "fuzz.c")) { *len = cur_len; return cur_src; }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = hbuf[hnext++ & 15];
    *len = fread(b, 1, sizeof hbuf[0] - 1, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

static const char *const dict[] = { "{", "}", "(", ")", "[", "]", ";", ",", "#define A(x) x##x\n", "#if ", "#endif\n", "##", "#",
    "\"", "'", "/*", "*/", "\\\n", "struct ", "union ", "enum ", "typedef ", "...", "->", "sizeof", "int ", "char *", "static ",
    "case ", "default:", "goto ", "0x", "1e5", "<<=", "?", ":", "__attribute__((packed))", "asm(\"\")", "\n#include \"x.h\"\n",
    "void", "(*)", "[0]", "[-1]", "= {", "L\"", "_Static_assert(", "typeof(", "({", "})", "__VA_ARGS__", "defined" };

static size_t mutate(char *b, size_t n, size_t cap) {
    int k = 1 + (int)(rnd() % 4);
    while (k--) {
        if (n == 0) { b[0] = 'x'; n = 1; }
        size_t p = rnd() % n;
        switch (rnd() % 7) {
        case 0: b[p] = (char)(rnd() & 0xff); break;
        case 1: { static const char cs[] = " \n{}();,#\"'\\/*<>=+-&|!~?:[]."; b[p] = cs[rnd() % (sizeof cs - 1)]; break; }
        case 2: { size_t l = rnd() % 16; if (p + l > n) l = n - p; memmove(b + p, b + p + l, n - p - l); n -= l; break; }
        case 3: { size_t l = rnd() % 24; if (p + l > n) l = n - p; if (n + l < cap) { memmove(b + p + l, b + p, n - p); memcpy(b + p, b + p + l, l); n += l; } break; }
        case 4: n = p + 1; break;
        case 5: case 6: {
            const char *s = dict[rnd() % (sizeof dict / sizeof dict[0])];
            size_t l = strlen(s);
            if (n + l < cap) { memmove(b + p + l, b + p, n - p); memcpy(b + p, s, l); n += l; }
            break;
        }
        }
    }
    return n;
}

static void on_alarm(int s) {
    (void)s;
    static const char m[] = "fuzz: kcc завис на входе\n";
    if (write(2, m, sizeof m - 1) < 0) _exit(3);
    _exit(3);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fuzz iters files...\n"); return 2; }
    long iters = atol(argv[1]);
    int nf = argc - 2;
    size_t memsz = (size_t)128 << 20;
    void *mem = malloc(memsz);
    const size_t cap = 1 << 16;
    char *buf = malloc(cap), *orig = malloc(cap);
    if (!mem || !buf || !orig) return 2;
    const char *incs[2] = { "tests/kcc/inc", "kcc/include" };
    KCfg cfg = { rd, NULL, incs, 2, NULL, 0, "x86_64" };
    signal(SIGALRM, on_alarm);
    long ok = 0, rej = 0;
    for (long it = 0; it < iters; it++) {
        FILE *f = fopen(argv[2 + it % nf], "rb");
        if (!f) { fprintf(stderr, "fuzz: cannot open %s\n", argv[2 + it % nf]); return 2; }
        size_t n = fread(orig, 1, cap / 2, f);
        fclose(f);
        memcpy(buf, orig, n);
        if (it >= nf) n = mutate(buf, n, cap);
        cur_src = buf; cur_len = n;
        KResult r;
        alarm(20);
        int rc = kcc_compile(&cfg, mem, memsz, "fuzz.c", (int)(it % 3), &r);
        alarm(0);
        if (rc == 0) ok++;
        else {
            rej++;
            if (!r.err || !strchr(r.err, ':') || !strstr(r.err, "error")) {
                fprintf(stderr, "fuzz: ошибка без диагностики (итерация %ld, %s): %s\n", it, argv[2 + it % nf], r.err ? r.err : "(null)");
                return 1;
            }
        }
    }
    printf("fuzz: итераций %ld, принято %ld, отвергнуто %ld\n", iters, ok, rej);
    free(mem); free(buf); free(orig);
    return 0;
}
