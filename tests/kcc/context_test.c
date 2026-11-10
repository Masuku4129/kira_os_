#include "../../kcc/kcc.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *main_src; const char *header_src; } MemFS;

static const char *read_mem(void *ud, const char *path, size_t *len) {
    MemFS *fs = ud;
    const char *s = NULL;
    if (!strcmp(path, "main.c")) s = fs->main_src;
    else if (!strcmp(path, "once.h")) s = fs->header_src;
    if (!s) return NULL;
    *len = strlen(s);
    return s;
}

static int compile(const char *main_src, const char *header_src, const KOptions *options, int emit, void *arena, size_t arena_size, KResult *res) {
    MemFS fs = { main_src, header_src };
    KCfg cfg = { read_mem, &fs, NULL, 0, NULL, 0, "x86_64", NULL };
    return kcc_compile_opts(&cfg, options, arena, arena_size, "main.c", emit, res);
}

int main(void) {
    static unsigned char arena[16u * 1024u * 1024u];
    KResult first, second;
    const char *with_header =
        "#include \"once.h\"\n"
        "#include \"once.h\"\n"
        "int main(void) { return VALUE; }\n";
    const char *header = "#pragma once\n#define VALUE 17\n";
    const char *without_header =
        "#ifdef VALUE\n"
        "#error macro leaked between compilations\n"
        "#endif\n"
        "int main(void) { return __COUNTER__; }\n";

    if (compile(with_header, header, NULL, 1, arena, sizeof arena, &first) != 0) {
        fprintf(stderr, "first compilation failed: %s\n", first.err ? first.err : "unknown");
        return 1;
    }
    if (!first.ir || !strstr(first.ir, "func main")) {
        fputs("first compilation produced no main IR\n", stderr);
        return 1;
    }
    if (compile(without_header, header, NULL, 1, arena, sizeof arena, &second) != 0) {
        fprintf(stderr, "second compilation failed: %s\n", second.err ? second.err : "unknown");
        return 1;
    }
    if (!second.ir || !strstr(second.ir, "func main") || !strstr(second.ir, "const 0")) {
        fputs("second compilation retained stale preprocessor/parser/IR state\n", stderr);
        return 1;
    }
    KOptions object_options = {0}, default_options = {0};
    KResult object_result, executable_result;
    object_options.obj = 1;
    if (compile(without_header, header, &object_options, 2, arena, sizeof arena, &object_result) != 0 ||
        !object_result.elf || object_result.elflen < 18 || object_result.elf[16] != 1 || object_result.elf[17] != 0) {
        fputs("object-mode option was not applied to its compilation\n", stderr);
        return 1;
    }
    if (compile(without_header, header, &default_options, 2, arena, sizeof arena, &executable_result) != 0 ||
        !executable_result.elf || executable_result.elflen < 18 || executable_result.elf[16] != 2 || executable_result.elf[17] != 0) {
        fputs("compiler options leaked between object and executable compilations\n", stderr);
        return 1;
    }
    puts("KCC compilation-context sequential isolation: PASS");
    return 0;
}
