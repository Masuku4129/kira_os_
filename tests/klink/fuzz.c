/* Фаззинг klink: случайные искажения объектов и скрипта. Единственное требование - ни падений, ни зависаний, ни UB (сборка под
   ASan+UBSan): каждый вызов возвращает 0 или -1 с непустым сообщением, а при 0 результат проходит проверку заголовка ELF.
   Использование: fuzz ITER SEED script.ld obj.o... */
#include "../../klink/klink.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) return NULL;
    fclose(f);
    *n = (size_t)l;
    return b;
}

/* одно искажение: байт, диапазон 0xff/0x00, граничное 32/64-битное значение, обрезка, дублирование куска */
static size_t mutate(unsigned char *b, size_t n, size_t cap) {
    if (n == 0) return n;
    switch (rnd() % 7) {
    case 0: b[rnd() % n] = (unsigned char)rnd(); break;
    case 1: b[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8)); break;
    case 2: { size_t o = rnd() % n, l = rnd() % 16; for (size_t i = 0; i < l && o + i < n; i++) b[o + i] = rnd() & 1 ? 0xff : 0; break; }
    case 3: {
        static const uint32_t v[] = { 0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffff0, 0x10000, 0xffff };
        size_t o = rnd() % n;
        uint32_t x = v[rnd() % 8];
        for (int i = 0; i < 4 && o + (size_t)i < n; i++) b[o + (size_t)i] = (unsigned char)(x >> (8 * i));
        break;
    }
    case 4: n = rnd() % n; break;
    case 5: { size_t o = rnd() % n, l = rnd() % 64, d = rnd() % n; if (o + l <= n && d + l <= n) memmove(b + d, b + o, l); break; }
    default: { size_t o = rnd() % n; if (n + 1 <= cap) { memmove(b + o + 1, b + o, n - o); b[o] = (unsigned char)rnd(); n++; } break; }
    }
    return n;
}

static void on_alarm(int s) {
    (void)s;
    ssize_t r = write(2, "fuzz: hang\n", 11);
    (void)r;
    _exit(3);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: fuzz ITER SEED script.ld obj.o...\n"); return 2; }
    long iters = atol(argv[1]);
    rs = (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ULL + 1;
    size_t slen;
    unsigned char *script = slurp(argv[3], &slen);
    int nobj = argc - 4;
    unsigned char **orig = calloc((size_t)nobj, sizeof *orig), **work = calloc((size_t)nobj, sizeof *work);
    size_t *len = calloc((size_t)nobj, sizeof *len);
    for (int i = 0; i < nobj; i++) {
        orig[i] = slurp(argv[4 + i], &len[i]);
        if (!orig[i]) { fprintf(stderr, "fuzz: cannot read %s\n", argv[4 + i]); return 2; }
        work[i] = malloc(len[i] + 16);
    }
    unsigned char *wscript = malloc(slen + 16);
    size_t arena_cap = (size_t)256 << 20;
    unsigned char *arena = malloc(arena_cap);
    KLInput *in = calloc((size_t)nobj, sizeof *in);
    size_t *wl = calloc((size_t)nobj, sizeof *wl);
    signal(SIGALRM, on_alarm);
    long ok = 0, err = 0;
    for (long it = 0; it < iters; it++) {
        alarm(60);
        size_t sl = slen;
        memcpy(wscript, script, slen);
        for (int i = 0; i < nobj; i++) { memcpy(work[i], orig[i], len[i]); wl[i] = len[i]; }
        int nm = 1 + (int)(rnd() % 6);
        int target = (int)(rnd() % (unsigned)(nobj + 1));              /* nobj - искажаем скрипт */
        for (int m = 0; m < nm; m++) {
            if (target == nobj) sl = mutate(wscript, sl, slen + 16);
            else wl[target] = mutate(work[target], wl[target], len[target] + 16);
        }
        for (int i = 0; i < nobj; i++) { in[i].name = argv[4 + i]; in[i].data = work[i]; in[i].len = wl[i]; }
        KLink k;
        memset(&k, 0, sizeof k);
        k.objs = in; k.nobjs = (unsigned)nobj;
        k.script = (const char *)wscript; k.script_len = sl;
        k.gc_sections = (int)(rnd() & 1);
        k.arena = arena; k.arena_cap = arena_cap;
        int rc = klink_link(&k);
        if (rc == 0) {
            ok++;
            if (!k.out || k.out_len < 64 || memcmp(k.out, "\x7f" "ELF", 4) != 0) { fprintf(stderr, "fuzz: success without an ELF image\n"); return 1; }
        } else {
            err++;
            if (rc != -1 || !k.err[0]) { fprintf(stderr, "fuzz: failure without a message\n"); return 1; }
        }
    }
    printf("fuzz: %ld runs, %ld linked, %ld rejected, no crashes\n", iters, ok, err);
    return 0;
}
