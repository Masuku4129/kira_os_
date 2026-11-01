/* kcc: командная строка (хост). kcc [-I dir] [-D n[=v]] [-S-ir] [-o out] file.c
 * Арена выделяется один раз; ядро компилятора malloc не использует. */
#include "kcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *read_file(void *ud, const char *path, size_t *len) {
    (void)ud;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = ka_new((size_t)sz + 1);          /* буфер живёт в арене компилятора */
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *incs[64], *defs[64], *in = NULL, *outp = NULL, *target = "x86_64";
    int ni = 0, nd = 0, ir = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-S-ir")) ir = 1;
        else if (!strcmp(a, "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strncmp(a, "-I", 2) && a[2]) { if (ni < 64) incs[ni++] = a + 2; }
        else if (!strcmp(a, "-I") && i + 1 < argc) { if (ni < 64) incs[ni++] = argv[++i]; }
        else if (!strncmp(a, "-D", 2) && a[2]) { if (nd < 64) defs[nd++] = a + 2; }
        else if (!strcmp(a, "-D") && i + 1 < argc) { if (nd < 64) defs[nd++] = argv[++i]; }
        else if (!strcmp(a, "-target") && i + 1 < argc) target = argv[++i];
        else if (a[0] == '-') { fprintf(stderr, "kcc: unknown option '%s'\n", a); return 2; }
        else if (!in) in = a;
        else { fprintf(stderr, "kcc: only one input file is supported at this stage\n"); return 2; }
    }
    if (!in) { fprintf(stderr, "usage: kcc [-I dir] [-D n[=v]] [-S-ir] -o out file.c\n"); return 2; }
    if (!ir && !outp) { fprintf(stderr, "kcc: no output: use -o file (ELF) or -S-ir\n"); return 2; }
    size_t memsz = (size_t)512 << 20;
    void *mem = malloc(memsz);
    if (!mem) { fprintf(stderr, "kcc: out of memory\n"); return 2; }
    KCfg cfg = { read_file, NULL, incs, ni, defs, nd, target, NULL };
    KResult r;
    int rc = kcc_compile(&cfg, mem, memsz, in, ir ? 1 : 2, &r);
    if (r.warn) fputs(r.warn, stderr);
    if (rc) { fprintf(stderr, "%s\n", r.err); return 1; }
    FILE *o = outp ? fopen(outp, "wb") : stdout;
    if (!o) { fprintf(stderr, "kcc: cannot write '%s'\n", outp); return 2; }
    if (ir) fputs(r.ir, o);
    else if (fwrite(r.elf, 1, r.elflen, o) != r.elflen) { fprintf(stderr, "kcc: write error on '%s'\n", outp); return 2; }
    if (outp) fclose(o);
    return 0;
}
