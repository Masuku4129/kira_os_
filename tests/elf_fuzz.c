/* Фаззинг загрузчика ELF (elf_parse) под ASan/UBSan: искажённые настоящие программы и чистая случайность.
   Требования: ни одного падения и выхода за границы; если разбор вернул ELF_OK, то выданное описание
   обязано удовлетворять всем обещаниям elf.h (окно адресов, выравнивание, без W+X, данные внутри образа,
   точка входа в исполняемом сегменте, число страниц не больше предела).
   Использование: elf_fuzz ИТЕРАЦИЙ ЗЁРНО файл.elf... */
#include "../elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

#define WIN_LO 0x400000ull
#define WIN_HI 0x7fff00000000ull
#define MAXP 4096u

static int fails;
static unsigned long ok_count, err_count;

static void verify(const unsigned char* b, size_t n, const elf_info_t* o) {
    (void)b;
    int entry_ok = 0;
    if (o->nsegs == 0 || o->nsegs > ELF_MAX_SEGS) { printf("FAIL: nsegs=%u\n", o->nsegs); fails++; return; }
    uint64_t pages = 0;
    for (unsigned i = 0; i < o->nsegs; i++) {
        const elf_seg_t* s = &o->seg[i];
        if (s->vaddr & 0xfff) { printf("FAIL: vaddr не выровнен %llx\n", (unsigned long long)s->vaddr); fails++; }
        if (s->write && s->exec) { printf("FAIL: сегмент W+X\n"); fails++; }
        if (s->file_size > s->mem_size) { printf("FAIL: file_size > mem_size\n"); fails++; }
        if (s->file_off > n || s->file_size > n - s->file_off) { printf("FAIL: данные вне образа\n"); fails++; }
        if (s->vaddr < WIN_LO || s->mem_size > WIN_HI - s->vaddr) { printf("FAIL: сегмент вне окна\n"); fails++; }
        pages += (s->mem_size + 0xfff) >> 12;
        if (s->exec && o->entry >= s->vaddr && o->entry < s->vaddr + s->mem_size) entry_ok = 1;
        for (unsigned j = 0; j < i; j++) {
            const elf_seg_t* t = &o->seg[j];
            uint64_t a0 = s->vaddr >> 12, a1 = (s->vaddr + s->mem_size + 0xfff) >> 12;
            uint64_t b0 = t->vaddr >> 12, b1 = (t->vaddr + t->mem_size + 0xfff) >> 12;
            if (a0 < b1 && b0 < a1) { printf("FAIL: сегменты делят страницу\n"); fails++; }
        }
    }
    if (!entry_ok) { printf("FAIL: точка входа вне исполняемого сегмента\n"); fails++; }
    if (pages > MAXP || pages != o->pages) { printf("FAIL: страниц %llu, заявлено %u\n", (unsigned long long)pages, o->pages); fails++; }
}

static void run(const unsigned char* b, size_t n) {
    elf_info_t o;
    memset(&o, 0xa5, sizeof o);
    int r = elf_parse(b, n, WIN_LO, WIN_HI, MAXP, &o);
    if (r == ELF_OK) { ok_count++; verify(b, n, &o); }
    else { err_count++; if (!elf_strerror(r) || !*elf_strerror(r)) { printf("FAIL: нет текста ошибки для %d\n", r); fails++; } }
}

static unsigned char* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

static size_t mutate(unsigned char* b, size_t n) {
    if (n == 0) return n;
    int k = 1 + (int)(rnd() % 3);
    while (k--) {
        switch (rnd() % 6) {
        case 0: b[rnd() % n] = (unsigned char)rnd(); break;
        case 1: b[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8)); break;
        case 2: { size_t o = rnd() % n, l = rnd() % 12; for (size_t i = 0; i < l && o + i < n; i++) b[o + i] = (rnd() & 1) ? 0xff : 0; break; }
        case 3: { /* граничное 64-битное значение в заголовке или таблице сегментов (первые 256 байт) */
            static const uint64_t v[] = {0, 1, 0xfff, 0x1000, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0xfffffffffffff000ull, 0x400000, 0x7fff00000000ull};
            size_t lim = n < 256 ? n : 256; if (lim < 8) break;
            size_t o = rnd() % (lim - 7); uint64_t x = v[rnd() % 10]; memcpy(b + o, &x, 8); break; }
        case 4: n = 1 + rnd() % n; break;                       /* обрезка */
        case 5: { size_t o = rnd() % n; if (o + 1 < n) memmove(b + o, b + o + 1, n - o - 1); n--; break; }
        }
        if (n == 0) break;
    }
    return n;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "elf_fuzz ИТЕРАЦИЙ ЗЁРНО файл.elf...\n"); return 2; }
    long iters = atol(argv[1]);
    rs = 0x9e3779b97f4a7c15ull ^ strtoull(argv[2], NULL, 0);
    if (!rs) rs = 1;
    unsigned char* seed[64]; size_t sl[64]; int ns = 0;
    for (int i = 3; i < argc && ns < 64; i++) { seed[ns] = slurp(argv[i], &sl[ns]); if (seed[ns]) ns++; }
    if (!ns) { fprintf(stderr, "нет исходных файлов\n"); return 2; }
    for (int i = 0; i < ns; i++) run(seed[i], sl[i]);                    /* исходные должны разбираться */
    unsigned long seeds_ok = ok_count;
    unsigned char* buf = malloc(1 << 20);
    for (long it = 0; it < iters; it++) {
        size_t n;
        if (rnd() % 8 == 0) { n = rnd() % 512; for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)rnd(); if (n >= 4 && (rnd() & 1)) memcpy(buf, "\177ELF", 4); }
        else { int s = (int)(rnd() % (unsigned)ns); n = sl[s]; if (n > (1 << 20)) n = 1 << 20; memcpy(buf, seed[s], n); n = mutate(buf, n); }
        /* точная копия в куче: ASan видит любое чтение за концом */
        unsigned char* h = malloc(n ? n : 1); memcpy(h, buf, n); run(h, n); free(h);
    }
    printf("elf_fuzz: %ld входов, принято %lu (из них исходных %lu), отклонено %lu, нарушений %d\n", iters, ok_count, seeds_ok, err_count, fails);
    return fails ? 1 : 0;
}
