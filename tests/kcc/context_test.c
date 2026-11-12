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

static int compile(const char *main_src, const char *header_src, const char *target,
                   int emit, void *arena, size_t arena_size, KResult *res) {
    MemFS fs = { main_src, header_src };
    KCfg cfg = { read_mem, &fs, NULL, 0, NULL, 0, target, NULL };
    return kcc_compile(&cfg, arena, arena_size, "main.c", emit, res);
}

static int check_elf(const KResult *r, unsigned machine) {
    return r->elf && r->elflen >= 64 &&
           r->elf[0] == 0x7f && r->elf[1] == 'E' &&
           r->elf[2] == 'L' && r->elf[3] == 'F' &&
           r->elf[4] == 2 && r->elf[5] == 1 &&
           r->elf[18] == (unsigned char)(machine & 0xff) &&
           r->elf[19] == (unsigned char)(machine >> 8);
}

int main(void) {
    static unsigned char arena[16u * 1024u * 1024u];
    KResult first, second, x86, a64, x86_again, opt_x86, opt_a64, opt_x86_again;
    static unsigned char opt_saved[256u * 1024u];
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
    const char *simple = "int main(void) { return 7; }\n";
    const char *optimized =
        "static int twice(int x) { return x * 2 + 1; }\n"
        "int main(void) { int s = 0; for (int i = 0; i < 19; i++) s += twice(i); return s; }\n";

    if (compile(with_header, header, "x86_64", 1, arena, sizeof arena, &first) != 0 ||
        !first.ir || !strstr(first.ir, "func main")) {
        fprintf(stderr, "first compilation failed: %s\n", first.err ? first.err : "no IR");
        return 1;
    }
    if (compile(without_header, header, "x86_64", 1, arena, sizeof arena, &second) != 0 ||
        !second.ir || !strstr(second.ir, "func main") || !strstr(second.ir, "const 0")) {
        fprintf(stderr, "second compilation retained stale PP/parser/IR state: %s\n",
                second.err ? second.err : "invalid IR");
        return 1;
    }
    if (compile(simple, "", "x86_64", 2, arena, sizeof arena, &x86) != 0 ||
        !check_elf(&x86, 62)) {
        fprintf(stderr, "x86-64 backend state reset failed: %s\n", x86.err ? x86.err : "invalid ELF");
        return 1;
    }
    if (compile(simple, "", "aarch64", 2, arena, sizeof arena, &a64) != 0 ||
        !check_elf(&a64, 183)) {
        fprintf(stderr, "AArch64 backend state reset failed: %s\n", a64.err ? a64.err : "invalid ELF");
        return 1;
    }
    if (compile(simple, "", "x86_64", 2, arena, sizeof arena, &x86_again) != 0 ||
        !check_elf(&x86_again, 62)) {
        fprintf(stderr, "x86-64 state leaked across AArch64 compilation: %s\n",
                x86_again.err ? x86_again.err : "invalid ELF");
        return 1;
    }
    if (compile(optimized, "", "x86_64", 2, arena, sizeof arena, &opt_x86) != 0 ||
        !check_elf(&opt_x86, 62) || opt_x86.elflen > sizeof opt_saved) {
        fprintf(stderr, "optimized x86-64 compile failed: %s\n", opt_x86.err ? opt_x86.err : "invalid ELF");
        return 1;
    }
    memcpy(opt_saved, opt_x86.elf, opt_x86.elflen);
    size_t opt_saved_len = opt_x86.elflen;
    if (compile(optimized, "", "aarch64", 2, arena, sizeof arena, &opt_a64) != 0 || !check_elf(&opt_a64, 183)) {
        fprintf(stderr, "optimized AArch64 compile failed: %s\n", opt_a64.err ? opt_a64.err : "invalid ELF");
        return 1;
    }
    if (compile(optimized, "", "x86_64", 2, arena, sizeof arena, &opt_x86_again) != 0 ||
        !check_elf(&opt_x86_again, 62) || opt_x86_again.elflen != opt_saved_len ||
        memcmp(opt_saved, opt_x86_again.elf, opt_saved_len) != 0) {
        fprintf(stderr, "inliner/optimizer state leaked across target switch or repeated compilation\n");
        return 1;
    }
    puts("KCC sequential PP/parser/IR/backend/inliner/optimizer context isolation: PASS");
    return 0;
}
