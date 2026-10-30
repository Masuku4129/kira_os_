// Загрузка программ: разбор и защита ELF, манифест доверия, журнал организма и профили в KiraFS.
#include "elf.h"
#include "trust.h"
#include "life.h"
#include "timer.h"
#include "kfs.h"
#include "immune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

/* ---- заглушки ядра для life.c ---- */
static uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
static char uout[8192]; static size_t un;
void uart_print(const char* s) { size_t n = strlen(s); if (un + n < sizeof uout) { memcpy(uout + un, s, n); un += n; uout[un] = 0; } }
void uart_putchar(char c) { if (un + 1 < sizeof uout) { uout[un++] = c; uout[un] = 0; } }

/* ---- RAM-диск ---- */
#define SECT (16 + 2048)
static uint8_t disk[SECT * 512];
static int rd(blk_dev_t* d, uint64_t l, uint32_t n, void* b) { (void)d; if (l + n > SECT) return -1; memcpy(b, disk + l * 512, n * 512); return 0; }
static int wr(blk_dev_t* d, uint64_t l, uint32_t n, const void* b) { (void)d; if (l + n > SECT) return -1; memcpy(disk + l * 512, b, n * 512); return 0; }
static int fl(blk_dev_t* d) { (void)d; return 0; }
static blk_dev_t dev = { "ram", SECT, rd, wr, fl, 0 };

/* ---- ELF ---- */
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t* p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
#define LO 0x8000000000ULL
#define HI 0x8000200000ULL
typedef struct { uint32_t type, flags; uint64_t off, va, fsz, msz; } ph_t;
static size_t mk(uint8_t* b, uint64_t entry, const ph_t* ph, int n) {
    memset(b, 0, 65536);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
    w16(b + 16, 2); w16(b + 18, 62); w32(b + 20, 1); w64(b + 24, entry); w64(b + 32, 64);
    w16(b + 52, 64); w16(b + 54, 56); w16(b + 56, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t* p = b + 64 + i * 56;
        w32(p, ph[i].type); w32(p + 4, ph[i].flags); w64(p + 8, ph[i].off); w64(p + 16, ph[i].va); w64(p + 24, ph[i].va);
        w64(p + 32, ph[i].fsz); w64(p + 40, ph[i].msz); w64(p + 48, 4096);
    }
    return 8192;
}
static uint8_t img[65536];
static elf_info_t inf;
static int parse(size_t len) { return elf_parse(img, len, LO, HI, 16, &inf); }

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(*n); if (fread(b, 1, *n, f) != *n) { fclose(f); return 0; } fclose(f); return b;
}

#include "manifest_test.h"

int main(void) {
    /* корректные образцы */
    ph_t good[] = { { 1, 5, 4096, LO, 100, 100 }, { 1, 4, 8192, LO + 4096, 50, 50 }, { 1, 6, 4096, LO + 8192, 10, 5000 } };
    size_t len = mk(img, LO + 16, good, 2); (void)len;
    CHECK(parse(12000) == ELF_OK && inf.nsegs == 2 && inf.entry == LO + 16 && inf.seg[0].exec && !inf.seg[0].write && !inf.seg[1].exec && inf.pages == 2);
    mk(img, LO, good, 3);
    CHECK(parse(12000) == ELF_OK && inf.nsegs == 3 && inf.seg[2].write && inf.pages == 2 + 2 /* 5000 байт = 2 страницы */);

    /* отказы */
    ph_t t;
    t = good[0]; t.flags = 7; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);                 /* W+X */
    t = good[0]; t.va = LO - 4096; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);             /* ниже окна */
    t = good[0]; t.va = HI; mk(img, HI, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);                    /* в окне данных/стека */
    t = good[0]; t.msz = HI - LO + 1; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);         /* вылезает за окно */
    t = good[0]; t.va = LO + 1; mk(img, LO + 1, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);           /* не по границе страницы */
    t = good[0]; t.off = 4097; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);
    t = good[0]; t.fsz = 200; t.msz = 100; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT);   /* файла больше памяти */
    t = good[0]; t.fsz = 100000; t.msz = 100000; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SHORT || parse(12000) == ELF_ERR_LIMIT);
    t = good[0]; t.off = 0xFFFFFFFFFFFFF000ULL; mk(img, LO, &t, 1); CHECK(parse(12000) == ELF_ERR_SHORT); /* переполнение off+fsz */
    mk(img, LO + 4096, good, 1); CHECK(parse(12000) == ELF_ERR_ENTRY);                                    /* вход вне сегмента */
    { ph_t d[2] = { good[0], good[0] }; mk(img, LO, d, 2); CHECK(parse(12000) == ELF_ERR_SEGMENT); }     /* пересечение страниц */
    { ph_t d[1] = { { 1, 6, 4096, LO, 10, 10 } }; mk(img, LO, d, 1); CHECK(parse(12000) == ELF_ERR_ENTRY); } /* единственный сегмент не исполняемый */
    { ph_t d[2] = { good[0], { 0x6474E551, 7, 0, 0, 0, 0 } }; mk(img, LO, d, 2); CHECK(parse(12000) == ELF_ERR_SEGMENT); } /* исполняемый стек */
    { ph_t d[1] = { { 2, 4, 0, 0, 0, 0 } }; mk(img, LO, d, 1); CHECK(parse(12000) == ELF_ERR_SEGMENT); }  /* нет PT_LOAD */
    { ph_t big[ELF_MAX_SEGS + 1]; for (int i = 0; i <= ELF_MAX_SEGS; i++) big[i] = (ph_t){ 1, 5, 4096, LO + 4096ULL * i, 10, 10 }; mk(img, LO, big, ELF_MAX_SEGS + 1); CHECK(parse(12000) == ELF_ERR_LIMIT); }
    mk(img, LO, good, 1); img[0] = 0; CHECK(parse(12000) == ELF_ERR_MAGIC);
    mk(img, LO, good, 1); img[4] = 1; CHECK(parse(12000) == ELF_ERR_FORMAT);                              /* ELF32 */
    mk(img, LO, good, 1); w16(img + 16, 3); CHECK(parse(12000) == ELF_ERR_FORMAT);                         /* ET_DYN */
    mk(img, LO, good, 1); w16(img + 18, 3); CHECK(parse(12000) == ELF_ERR_FORMAT);                         /* не x86-64 */
    mk(img, LO, good, 1); w64(img + 32, 0xFFFFFFFFFFFFFFF0ULL); CHECK(parse(12000) == ELF_ERR_SHORT);     /* phoff за файлом */
    mk(img, LO, good, 1); CHECK(elf_parse(img, 63, LO, HI, 16, &inf) == ELF_ERR_SHORT && elf_parse(0, 0, LO, HI, 16, &inf) == ELF_ERR_SHORT);

    /* усечение на каждой длине и случайные порчи не роняют разбор */
    mk(img, LO + 16, good, 3);
    for (size_t l = 0; l < 400; l++) (void)elf_parse(img, l, LO, HI, 16, &inf);
    srand(7);
    for (int it = 0; it < 20000; it++) {
        uint8_t save[400]; memcpy(save, img, 400);
        int flips = 1 + rand() % 4;
        for (int f = 0; f < flips; f++) img[rand() % 400] ^= (uint8_t)(1 + rand() % 255);
        int r = elf_parse(img, 12000, LO, HI, 16, &inf);
        if (r == ELF_OK) {                                                                       /* принятое обязано быть безопасным для загрузчика */
            CHECK(inf.nsegs >= 1 && inf.nsegs <= ELF_MAX_SEGS && inf.pages <= 16);
            int entry_in = 0;
            for (unsigned s = 0; s < inf.nsegs; s++) {
                const elf_seg_t* g = &inf.seg[s];
                CHECK(!(g->write && g->exec) && g->vaddr >= LO && g->vaddr + g->mem_size <= HI && g->file_size <= g->mem_size && g->file_off + g->file_size <= 12000);
                if (g->exec && inf.entry >= g->vaddr && inf.entry < g->vaddr + g->mem_size) entry_in = 1;
            }
            CHECK(entry_in);
        }
        memcpy(img, save, 400);
    }

    /* настоящие программы, собранные сборкой (если она уже выполнялась) */
    const char* apps[] = { "plasma", "paint", "demo", "atk-mem", "atk-priv", "atk-sys", "atk-gs", "cube3d", "fpu", "burn", "mt", "ipc" };
    int real = 0;
    for (unsigned i = 0; i < sizeof apps / sizeof apps[0]; i++) {
        char p[64]; snprintf(p, sizeof p, "userland/%s.elf", apps[i]);
        size_t n; uint8_t* b = slurp(p, &n);
        if (!b) continue;
        CHECK(elf_parse(b, n, LO, HI, 48, &inf) == ELF_OK && inf.entry >= LO);   /* предел страниц тот же, что у ядра (proc.c) */
        uint8_t d[32]; sha256(b, n, d);
        CHECK(trust_check(TEST_MANIFEST, TEST_MANIFEST_COUNT, apps[i], d) == TRUST_OK);          /* манифест соответствует файлам */
        b[n / 2] ^= 1; sha256(b, n, d);
        CHECK(trust_check(TEST_MANIFEST, TEST_MANIFEST_COUNT, apps[i], d) == TRUST_TAMPERED);
        free(b); real++;
    }

    /* доверие */
    uint8_t da[32], db[32]; sha256("a", 1, da); sha256("b", 1, db);
    trust_entry_t tbl[2]; tbl[0].name = "alpha"; memcpy(tbl[0].sha, da, 32); tbl[1].name = "beta"; memcpy(tbl[1].sha, db, 32);
    CHECK(trust_check(tbl, 2, "alpha", da) == TRUST_OK && trust_check(tbl, 2, "beta", db) == TRUST_OK);
    CHECK(trust_check(tbl, 2, "alpha", db) == TRUST_TAMPERED && trust_check(tbl, 2, "gamma", da) == TRUST_UNKNOWN);
    CHECK(trust_check(tbl, 2, "alph", da) == TRUST_UNKNOWN && trust_check(tbl, 0, "alpha", da) == TRUST_UNKNOWN);
    da[31] ^= 1; sha256("a", 1, db); CHECK(trust_check(tbl, 2, "alpha", da) == TRUST_TAMPERED);       /* расхождение в последнем байте */

    /* журнал организма и профили */
    memset(disk, 0, sizeof disk);
    life_note("start", "x", 0); CHECK(life_dropped() == 1);                                         /* без ФС события не копятся и считаются потерянными */
    CHECK(kfs_format(&dev, 16) == 0);
    fake_ticks = 5 * TIMER_HZ; life_init();                                                                   /* 5 с после загрузки */
    kfs_stat_t st;
    CHECK(kfs_stat("/var", &st) == 0 && kfs_stat("/var/prog", &st) == 0 && kfs_stat("/var/life.log", &st) == 0);
    fake_ticks = 12 * TIMER_HZ; life_note("start", "paint", 0); life_note("exit", "paint", 0);
    fake_ticks = 13 * TIMER_HZ; life_note("start", "atk-mem", 0); life_note("killed", "atk-mem", -1);
    life_note("start", "atk-mem", 0); life_note("killed", "atk-mem", -1);
    life_note("denied", "evil", 2);
    life_tick();
    uint32_t runs, faults, denied; int last;
    CHECK(life_profile("paint", &runs, &faults, &denied, &last) == 0 && runs == 1 && faults == 0 && denied == 0 && last == 0);
    CHECK(life_profile("atk-mem", &runs, &faults, &denied, &last) == 0 && runs == 2 && faults == 2 && last == -1);
    CHECK(life_profile("evil", &runs, &faults, &denied, &last) == 0 && runs == 0 && denied == 1);
    CHECK(life_profile("nope", 0, 0, 0, 0) == -1 && life_profile("../x", 0, 0, 0, 0) == -1 && life_profile("", 0, 0, 0, 0) == -1);
    un = 0; life_print();
    CHECK(strstr(uout, "boot") && strstr(uout, "12s start paint") && strstr(uout, "13s killed atk-mem -1") && strstr(uout, "denied evil 2"));
    /* перезагрузка: журнал и профили на месте, счёт продолжается */
    CHECK(kfs_mount(&dev, 16) == 0);
    fake_ticks = TIMER_HZ; life_init(); life_note("start", "paint", 0); life_tick();
    CHECK(life_profile("paint", &runs, 0, 0, 0) == 0 && runs == 2);
    un = 0; life_print();
    CHECK(strstr(uout, "12s start paint") && strstr(uout, "1s start paint") && strstr(uout, "boot"));
    /* переполнение очереди: лишнее считается потерянным, остальное записывается */
    uint32_t d0 = life_dropped();
    for (int i = 0; i < 40; i++) life_note("start", "bulk", 0);
    CHECK(life_dropped() - d0 == 24);
    life_tick();
    CHECK(life_profile("bulk", &runs, 0, 0, 0) == 0 && runs == 16);
    /* журнал ограничен по размеру и обрезается по строкам */
    for (int i = 0; i < 400; i++) { life_note("exit", "loop", i); if (i % 10 == 9) life_tick(); }
    life_tick();
    kfs_stat("/var/life.log", &st);
    CHECK(st.size <= 2048 && st.size > 1000);
    un = 0; life_print();
    CHECK(strstr(uout, "exit loop 399") && !strstr(uout, "start paint") && uout[0] >= '0' && uout[0] <= '9');   /* обрезано по границе строки */

    printf("programs: ELF validation (%d real binaries, 20000 corruptions), trust manifest, life log and profiles in KiraFS passed\n", real);
    return 0;
}
