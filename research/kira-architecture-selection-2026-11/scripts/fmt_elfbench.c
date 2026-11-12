/* [E] Микробенч разбора: elf_parse() (elf.c ядра) против прототипа kex_parse() (fmt_kex.c) на настоящих userland-программах KIRA.
   Использование: fmt_elfbench REPS N prog.elf prog.kex [prog.relocs.kex]. Для каждого файла сначала проверка: оба разбора дают одно и то же (сегменты, вход).
   Вывод: "rep метрика файл нс_на_разбор" (целые). Окно/лимит страниц как в proc.c: USER_CODE_VA=0x8000000000.. */
#include "fmt_kex.h"
#include "../../../immune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }
static uint8_t* slurp(const char* p, size_t* n) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); rewind(f); uint8_t* b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f); return b; }
#define LO 0x8000000000ULL
#define HI (LO + (1ULL << 32))
int main(int argc, char** argv) {
    int reps = atoi(argv[1]), n = atoi(argv[2]); volatile long sink = 0;
    for (int a = 3; a + 1 < argc; a += 2) {
        size_t le, lk; uint8_t* e = slurp(argv[a], &le); uint8_t* k = slurp(argv[a + 1], &lk);
        elf_info_t ie, ik; int re = elf_parse(e, le, LO, HI, 4096, &ie), rk = kex_parse(k, lk, LO, HI, 4096, &ik);
        if (re || rk || kex_verify_hash(k, lk)) { printf("FAIL %s: elf=%d kex=%d\n", argv[a], re, rk); return 1; }
        int same = ie.nsegs == ik.nsegs && ie.entry == ik.entry && ie.pages == ik.pages;
        for (unsigned i = 0; same && i < ie.nsegs; i++) same = ie.seg[i].vaddr == ik.seg[i].vaddr && ie.seg[i].file_size == ik.seg[i].file_size && ie.seg[i].mem_size == ik.seg[i].mem_size && ie.seg[i].write == ik.seg[i].write && ie.seg[i].exec == ik.seg[i].exec
                 && !memcmp(e + ie.seg[i].file_off, k + ik.seg[i].file_off, ie.seg[i].file_size);
        if (!same) { printf("FAIL %s: разные результаты разбора\n", argv[a]); return 1; }
        const char* nm = strrchr(argv[a + 1], '/'); nm = nm ? nm + 1 : argv[a + 1];   /* метка = имя .kex (edit_r1000.kex = +1000 синтетических перемещений) */
        printf("OK %s elf=%zu kex=%zu\n", nm, le, lk);
        for (int w = 0; w < 2; w++) for (int i = 0; i < n; i++) { sink += elf_parse(e, le, LO, HI, 4096, &ie); sink += kex_parse(k, lk, LO, HI, 4096, &ik); }
        for (int r = 1; r <= reps; r++) {
            long long t0, t1; uint8_t d[32];
            t0 = now_ns(); for (int i = 0; i < n; i++) sink += elf_parse(e, le, LO, HI, 4096, &ie); t1 = now_ns(); printf("%d elf_parse %s %lld\n", r, nm, (t1 - t0) / n);
            t0 = now_ns(); for (int i = 0; i < n; i++) sink += kex_parse(k, lk, LO, HI, 4096, &ik); t1 = now_ns(); printf("%d kex_parse %s %lld\n", r, nm, (t1 - t0) / n);
            t0 = now_ns(); for (int i = 0; i < n / 10 + 1; i++) { sink += kex_parse(k, lk, LO, HI, 4096, &ik); sink += kex_verify_hash(k, lk); } t1 = now_ns(); printf("%d kex_parse+sha256 %s %lld\n", r, nm, (t1 - t0) / (n / 10 + 1));
            t0 = now_ns(); for (int i = 0; i < n / 10 + 1; i++) { sink += elf_parse(e, le, LO, HI, 4096, &ie); sha256(e, le, d); sink += d[0]; } t1 = now_ns(); printf("%d elf_parse+sha256 %s %lld\n", r, nm, (t1 - t0) / (n / 10 + 1));
        }
    }
    return sink == 0x7fffffffffffL;
}
