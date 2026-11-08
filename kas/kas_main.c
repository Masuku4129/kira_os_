/* kas: командная строка (хост). kas [-f elf64] [-o out.o] file.s
 * Ключ -f elf64 принимается для совместимости с nasm (других форматов нет). */
#include "kas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KAS_HOST_ARENA ((size_t)64 << 20)

int main(int argc, char **argv) {
    const char *in = NULL, *outp = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            if (strcmp(argv[++i], "elf64")) { fprintf(stderr, "kas: формат '%s' не поддержан (только elf64)\n", argv[i]); return 2; }
        } else if (argv[i][0] == '-') { fprintf(stderr, "kas: неизвестный ключ %s\n", argv[i]); return 2; }
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "использование: kas [-f elf64] [-o out.o] file.s\n"); return 2; }
    if (!outp) outp = "a.o";
    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "kas: не открыть %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 1; }
    char *src = malloc((size_t)n + 1);
    void *mem = malloc(KAS_HOST_ARENA); /* malloc даёт выравнивание 16 на хосте */
    if (!src || !mem) { fprintf(stderr, "kas: нет памяти\n"); return 1; }
    size_t got = fread(src, 1, (size_t)n, f);
    fclose(f);
    KasArena a;
    kas_arena_init(&a, mem, KAS_HOST_ARENA);
    const uint8_t *obj; size_t olen; char err[400];
    if (kas_assemble(&a, in, src, got, &obj, &olen, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    FILE *o = fopen(outp, "wb");
    if (!o || fwrite(obj, 1, olen, o) != olen) { fprintf(stderr, "kas: не записать %s\n", outp); return 1; }
    fclose(o);
    return 0;
}
