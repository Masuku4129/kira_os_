/* klink: командная строка хоста. Опции - подмножество ld.lld, реально используемое build_clang.sh:
 *   klink -T script.ld [-z max-page-size=N] [-z noseparate-code] [-z norelro] [-nostdlib] [--gc-sections] [-e sym] -o out obj...
 * Прочие опции отвергаются: молча проигнорированная опция могла бы изменить смысл сборки. */
#include "klink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    unsigned char *b = malloc(cap);
    while (b) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (n < cap) break;
        cap *= 2;
        unsigned char *nb = realloc(b, cap);
        if (!nb) { free(b); b = NULL; break; }
        b = nb;
    }
    int bad = ferror(f);
    fclose(f);
    if (!b || bad) { free(b); return NULL; }
    *len = n;
    return b;
}

int main(int argc, char **argv) {
    const char *outp = "a.out", *scr = NULL, *entry = NULL;
    uint64_t page = 0;
    int gc = 0, nin = 0, nostd = 0;
    const char **inputs = calloc((size_t)argc + 1, sizeof *inputs);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(a, "-T") && i + 1 < argc) scr = argv[++i];
        else if (!strcmp(a, "-e") && i + 1 < argc) entry = argv[++i];
        else if (!strcmp(a, "--gc-sections")) gc = 1;
        else if (!strcmp(a, "--no-gc-sections")) gc = 0;
        else if (!strcmp(a, "-nostdlib")) nostd = 1;     /* клиентских библиотек у klink нет: опция только подтверждает это */
        else if (!strcmp(a, "-z") && i + 1 < argc) {
            const char *z = argv[++i];
            if (!strncmp(z, "max-page-size=", 14)) {
                char *end;
                page = strtoull(z + 14, &end, 0);
                if (*end || page == 0) { fprintf(stderr, "klink: bad -z %s\n", z); return 2; }
            } else if (strcmp(z, "noseparate-code") && strcmp(z, "norelro")) {
                fprintf(stderr, "klink: unsupported option -z %s\n", z);
                return 2;
            }
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "klink: unsupported option %s\n", a);
            return 2;
        } else inputs[nin++] = a;
    }
    (void)nostd;
    if (!scr || nin == 0) {
        fprintf(stderr, "usage: klink -T script.ld [-z max-page-size=N] [-nostdlib] [--gc-sections] [-e sym] -o out obj...\n");
        return 2;
    }
    KLInput *in = calloc((size_t)nin, sizeof *in);
    size_t total = 0, slen = 0;
    for (int i = 0; i < nin; i++) {
        size_t n = 0;
        unsigned char *d = slurp(inputs[i], &n);
        if (!d) { fprintf(stderr, "klink: cannot read %s\n", inputs[i]); return 1; }
        in[i].name = inputs[i];
        in[i].data = d;
        in[i].len = n;
        total += n;
    }
    unsigned char *sc = slurp(scr, &slen);
    if (!sc) { fprintf(stderr, "klink: cannot read %s\n", scr); return 1; }
    KLink k;
    memset(&k, 0, sizeof k);
    k.objs = in;
    k.nobjs = (unsigned)nin;
    k.script = (const char *)sc;
    k.script_len = slen;
    k.entry = entry;
    k.gc_sections = gc;
    k.page_size = page;
    k.arena_cap = total * 8 + slen * 64 + ((size_t)64 << 20);
    k.arena = malloc(k.arena_cap);
    if (!k.arena) { fprintf(stderr, "klink: out of memory\n"); return 1; }
    if (klink_link(&k) != 0) {
        fprintf(stderr, "klink: error: %s\n", k.err);
        return 1;
    }
    FILE *o = fopen(outp, "wb");
    if (!o || fwrite(k.out, 1, k.out_len, o) != k.out_len || fclose(o) != 0) {
        fprintf(stderr, "klink: cannot write %s\n", outp);
        return 1;
    }
    chmod(outp, 0755);
    if (getenv("KLINK_STATS")) fprintf(stderr, "klink: %u allocatable input sections, %u removed by --gc-sections, %zu bytes\n", k.sections_in, k.sections_gc, k.out_len);
    return 0;
}
