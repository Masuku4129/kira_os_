/* [E] Мутационный фаззер парсеров форматов KIRA (в процессе, без libFuzzer: libclang_rt.fuzzer/asan в этой VM для clang нет; gcc -fsanitize=address,undefined есть).
   Цели: elf (elf_parse), kdrv (kdrv_header_check), krec (kdrv_rec_check), ksighdr/ksig (kdrv_sig_header_check/kdrv_sig_verify), kstate (kdrv_state_decode),
   kbi (kbi_validate + обход), mb2 (kbi_from_multiboot2/kbi_to_multiboot2), fdt (kbi_from_fdt), kfs (kfs_mount/list/read на мутированном образе), kex (прототип).
   Использование: fmt_fuzz ЦЕЛЬ N SEED [crash_dir] ; вход - точный malloc-буфер (ASAN ловит чтение за границей на 1 байт).
   Мутации: бит, байт, "интересные" значения 16/32/64 бит в любой позиции, +-дельта, обрезка, вставка, удаление, дубль, сплайс; 70% позиций - в "горячем" начале.
   Обратная связь: принятый парсером мутант (ret==OK) попадает в корпус (до 512) - слепое наращивание структуры. Инварианты проверяются на ПРИНЯТЫХ входах.
   При падении входной буфер сохраняется в crash_dir/crash-ЦЕЛЬ-N.bin. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "elf.h"
#include "kdrv_format.h"
#include "kdrv_sig.h"
#include "kdrv_state.h"
#include "bootinfo.h"
#include "immune.h"
#include "kfs.h"
#include "fmt_kex.h"

/* ---- генератор и утилиты ---- */
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static const uint64_t INTR[] = { 0, 1, 2, 3, 7, 8, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0xfff, 0x1000, 0x1001, 0x7fffffff, 0x80000000ULL, 0xffffffffULL, 0x100000000ULL,
    0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL, 0xfffffffffffff000ULL, 0x8000000000ULL, 0x8000000fffULL, 0x80ffffffffULL, 56, 64, 136, 144, 216, 4096, 0x400000 };
typedef struct { uint8_t* p; size_t n; } buf_t;
static buf_t corpus[512]; static unsigned ncorp, nseed; static size_t hot = 256;
static const char* tname; static const char* crash_dir = "."; static uint8_t* cur; static size_t curn; static unsigned long long iter_no;
static void dump_crash(void) {
    char path[512]; snprintf(path, sizeof path, "%s/crash-%s-%llu.bin", crash_dir, tname, iter_no);
    FILE* f = fopen(path, "wb"); if (f && cur) { fwrite(cur, 1, curn, f); fclose(f); } fprintf(stderr, "\nCRASH: вход (%zu Б) сохранён в %s\n", curn, path);
}
void __asan_on_error(void) { dump_crash(); }
static void on_sig(int s) { (void)s; dump_crash(); _exit(70); }
static void add_seed(const void* p, size_t n) { corpus[ncorp].p = malloc(n ? n : 1); memcpy(corpus[ncorp].p, p, n); corpus[ncorp].n = n; ncorp++; nseed = ncorp; }
static size_t pos(size_t n) { if (!n) return 0; if (rnd() % 10 < 7 && n > hot) return rnd() % hot; return rnd() % n; }
static buf_t mutate(const buf_t* s) {
    size_t cap = s->n + 64 + 4096; uint8_t* t = malloc(cap); size_t n = s->n; memcpy(t, s->p, n);
    int k = 1 + (int)(rnd() % 4);
    while (k--) {
        size_t p = pos(n);
        switch (rnd() % 12) {
        case 0: if (n) t[p] ^= (uint8_t)(1u << (rnd() & 7)); break;
        case 1: if (n) t[p] = (uint8_t)rnd(); break;
        case 2: case 3: { unsigned w = 1u << (rnd() % 4); if (p + w <= n) { uint64_t v = INTR[rnd() % (sizeof INTR / sizeof *INTR)]; memcpy(t + p, &v, w); } break; }
        case 4: { unsigned w = 1u << (rnd() % 4); if (p + w <= n) { uint64_t v = 0; memcpy(&v, t + p, w); v += (uint64_t)(int64_t)((int)(rnd() % 33) - 16); memcpy(t + p, &v, w); } break; }
        case 5: if (n > 1) n = 1 + rnd() % n; break;                                                    /* обрезка */
        case 6: { size_t l = 1 + rnd() % 64; if (n + l < cap) { memmove(t + p + l, t + p, n - p); for (size_t i = 0; i < l; i++) t[p + i] = (uint8_t)rnd(); n += l; } break; }
        case 7: { size_t l = 1 + rnd() % 64; if (p + l > n) l = n - p; memmove(t + p, t + p + l, n - p - l); n -= l; break; }
        case 8: { size_t l = 1 + rnd() % 64; if (p + l > n) l = n - p; if (n + l < cap) { memmove(t + p + l, t + p, n - p); memcpy(t + p, t + p + l, l); n += l; } break; }
        case 9: { buf_t* o = &corpus[rnd() % ncorp]; if (o->n) { size_t q = rnd() % o->n, l = 1 + rnd() % 32; if (q + l > o->n) l = o->n - q; if (p + l > n) l = n - p; memcpy(t + p, o->p + q, l); } break; }
        case 10: { size_t l = 1 + rnd() % 32; if (p + l > n) l = n - p; memset(t + p, (rnd() & 1) ? 0 : 0xff, l); break; }
        case 11: if (n >= 2) { size_t q = pos(n); uint8_t x = t[p]; t[p] = t[q]; t[q] = x; } break;
        }
    }
    buf_t r; r.n = n; r.p = malloc(n ? n : 1); memcpy(r.p, t, n); free(t); return r;                 /* точный размер: ASAN видит выход за границу */
}

/* ---- цели ---- */
#define LO 0x8000000000ULL
#define HI (LO + (1ULL << 32))
static int hist[64]; static void count(int r) { int i = r + 32; if (i < 0) i = 0; if (i > 63) i = 63; hist[i]++; }
static int check_info(const elf_info_t* o, size_t len, uint64_t lo, uint64_t hi, uint32_t maxp, int kex) {   /* инварианты принятого образа */
    if (o->nsegs == 0 || o->nsegs > ELF_MAX_SEGS) return 1;
    uint64_t pages = 0; int entry = 0;
    for (unsigned i = 0; i < o->nsegs; i++) {
        const elf_seg_t* s = &o->seg[i];
        if (s->write && s->exec) return 2;
        if (s->vaddr & 4095) return 3;
        if (!kex && (s->file_off & 4095)) return 4;
        if (s->file_off > len || s->file_size > len - s->file_off) return 5;
        if (s->file_size > s->mem_size || s->mem_size == 0) return 6;
        if (s->vaddr < lo || s->vaddr >= hi || s->mem_size > hi - s->vaddr) return 7;
        pages += (s->mem_size + 4095) / 4096;
        if (s->exec && o->entry >= s->vaddr && o->entry < s->vaddr + s->mem_size) entry = 1;
        for (unsigned k = 0; k < i; k++) { uint64_t a = o->seg[k].vaddr, ae = a + (o->seg[k].mem_size + 4095) / 4096 * 4096, b = s->vaddr, be = b + (s->mem_size + 4095) / 4096 * 4096; if (a < be && b < ae) return 8; }
    }
    if (pages != o->pages || pages > maxp) return 9;
    return entry ? 0 : 10;
}
static int t_elf(uint8_t* b, size_t n) {
    elf_info_t o; int r = elf_parse(b, n, LO, HI, 4096, &o); count(r);
    if (r == ELF_OK) { int c = check_info(&o, n, LO, HI, 4096, 0); if (c) { fprintf(stderr, "ИНВАРИАНТ elf %d\n", c); abort(); } }
    elf_info_t o2; int r2 = elf_parse(b, n, 0, ~0ULL, 0xffffffffu, &o2);                               /* крайнее окно: арифметика переполнений */
    if (r2 == ELF_OK) { int c = check_info(&o2, n, 0, ~0ULL, 0xffffffffu, 0); if (c) { fprintf(stderr, "ИНВАРИАНТ elf(wide) %d\n", c); abort(); } }
    if (r == ELF_OK && r2 != ELF_OK && r2 != ELF_ERR_LIMIT && r2 != ELF_ERR_SEGMENT) abort();
    return r == ELF_OK;
}
static int t_kex(uint8_t* b, size_t n) {
    elf_info_t o; int r = kex_parse(b, n, LO, HI, 4096, &o); count(r);
    if (r == KEX_OK) { int c = check_info(&o, n, LO, HI, 4096, 1); if (c) { fprintf(stderr, "ИНВАРИАНТ kex %d\n", c); abort(); } }
    elf_info_t o2; int r2 = kex_parse(b, n, 0, ~0ULL, 0xffffffffu, &o2); (void)r2;
    if (r == KEX_OK) (void)kex_verify_hash(b, n);
    return r == KEX_OK;
}
static int t_kdrv(uint8_t* b, size_t n) {
    kdrv_header_t h; int r = kdrv_header_check(b, (uint32_t)n, &h); count(r);
    if (r == KDRV_OK) {
        if (h.nmatch < 1 || h.nmatch > KDRV_MATCH_MAX || h.elf_size == 0 || h.elf_size > KDRV_ELF_MAX || n - sizeof h != h.elf_size) abort();
        unsigned l = 0; while (h.name[l]) { char c = h.name[l]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) abort(); l++; } if (!l || l > KDRV_NAME_MAX) abort();
        (void)kdrv_match_dev(&h, 0x1234, 0x5678, 0x020000);
    }
    return r == KDRV_OK;
}
static int t_krec(uint8_t* b, size_t n) {
    kdrv_rec_t rc; int r = kdrv_rec_check(b, (uint32_t)n, &rc); count(r);
    if (r == KDRV_OK) { if (rc.caps_ok & ~(uint32_t)CAP_DRIVER_) abort(); if (!memchr(rc.source, 0, sizeof rc.source)) abort(); (void)kdrv_rec_publisher_required(&rc); }
    return r == KDRV_OK;
}
static const uint8_t PKG[] = "KIRA KDRV signature test package v1";
static const kdrv_sig_key_t TKEY = { "TEST-ED25519-001", { 0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a }, 0, 0 };
static int t_ksighdr(uint8_t* b, size_t n) { int r = kdrv_sig_header_check(b, n); count(r); return r == KDRV_SIG_OK; }
static int t_ksig(uint8_t* b, size_t n) {          /* вход = подпись; пакет фиксирован. Принять может ТОЛЬКО точно исходную подпись */
    int r = kdrv_sig_verify(PKG, sizeof PKG - 1, b, n, &TKEY, 1, 1800000000LL); count(r);
    if (r == KDRV_SIG_OK && !(n == 144 && memcmp(b, corpus[0].p, 144) == 0)) { fprintf(stderr, "ПРИНЯТА ИЗМЕНЁННАЯ ПОДПИСЬ (n=%zu)\n", n); abort(); }   /* подделка - критический дефект */
    return r == KDRV_SIG_OK;
}
static int t_kstate(uint8_t* b, size_t n) {
    kdrv_state_t st; kdrv_state_init(&st); int r = kdrv_state_decode(&st, b, n); count(r);
    if (r == 0) { if (st.nfloor > KDRV_STATE_MAX_FLOORS || st.nrevoked > KDRV_STATE_MAX_REVOKED) abort(); uint8_t o[KDRV_STATE_FILE_SIZE]; kdrv_state_encode(&st, o); }
    return r == 0;
}
static int walk_kbi(const uint8_t* b, uint32_t n) {                       /* обход принятого blob: ASAN проверяет, что указатели внутри */
    volatile uint64_t acc = 0; const kbi_header_t* h = (const kbi_header_t*)b; const kbi_mmap_entry_t* m = kbi_mmap(b); const kbi_module_t* md = kbi_modules(b);
    for (uint32_t i = 0; m && i < h->mmap_count; i++) acc += m[i].base + m[i].length + m[i].type;
    for (uint32_t i = 0; md && i < h->modules_count; i++) { acc += md[i].base; if (md[i].name_len) { if ((uint64_t)md[i].name_off + md[i].name_len > n) abort(); acc += b[md[i].name_off]; } }
    if (h->cmdline_len) { if ((uint64_t)h->cmdline_off + h->cmdline_len > n) abort(); acc += b[h->cmdline_off]; }
    return (int)acc;
}
static int t_kbi(uint8_t* b, size_t n) { int r = kbi_validate(b, (uint32_t)n); count(r); if (r == 0) walk_kbi(b, (uint32_t)n); return r == 0; }
static int t_mb2(uint8_t* b, size_t n) {
    const void* res = 0; uint32_t rs2 = 0; int r = kbi_from_multiboot2(b, (uint32_t)n, 0x100000, 0x200000, &res, &rs2); count(r);
    if (r == 0) { if (!res || kbi_validate(res, rs2) != 0) { fprintf(stderr, "mb2 -> невалидный kbi\n"); abort(); } walk_kbi(res, rs2); }
    return r == 0;
}
static int t_kbi2mb(uint8_t* b, size_t n) {                                 /* kbi_to_multiboot2 на (возможно невалидном) входе */
    static uint8_t out[65536]; uint32_t os = 0; int r = kbi_to_multiboot2(b, (uint32_t)n, out, sizeof out, &os); count(r);
    if (r == 0 && os > sizeof out) abort();
    return r == 0;
}
static long fdt_short;
static int t_fdt(uint8_t* b, size_t n) {
    if (n < 40) { fdt_short++; return 0; }         /* ИЗВЕСТНАЯ НаХОДКА F1: fdt_valid читает 40 байт заголовка, не сверяя max_len с 40 (repro: 4 байта d00dfeed). Короткие входы пропускаются, чтобы фаззинг шёл глубже */
    static uint8_t out[1 << 16]; uint32_t os = 0; int r = kbi_from_fdt(b, (uint32_t)n, 0x40000000ULL, 0x40080000ULL, 0x100000, out, sizeof out, &os); count(r);
    if (r == 0) { if (os > sizeof out || kbi_validate(out, os) != 0) { fprintf(stderr, "fdt -> невалидный kbi\n"); abort(); } walk_kbi(out, os); }
    return r == 0;
}
/* kfs: RAM-диск */
#define KSECT 2048u
static uint8_t kdisk[KSECT * 512];
static int krd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b) { (void)d; if (lba + n > KSECT) return -1; memcpy(b, kdisk + lba * 512, (size_t)n * 512); return 0; }
static int kwr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b) { (void)d; if (lba + n > KSECT) return -1; memcpy(kdisk + lba * 512, b, (size_t)n * 512); return 0; }
static int kfl(blk_dev_t* d) { (void)d; return 0; }
static blk_dev_t kdev = { "ram", KSECT, krd, kwr, kfl, 0 };
static int kfs_cb(void* c, const char* nm, const kfs_stat_t* st) { (*(int*)c)++; (void)nm; (void)st; return 0; }
static int t_kfs(uint8_t* b, size_t n) {                                    /* вход = образ диска (дополняется нулями/обрезается до KSECT секторов) */
    memset(kdisk, 0, sizeof kdisk); memcpy(kdisk, b, n < sizeof kdisk ? n : sizeof kdisk);
    int r = kfs_mount(&kdev, 0); count(r);
    if (r == 0) {
        int cnt = 0; kfs_list("/", kfs_cb, &cnt); kfs_list("/bin", kfs_cb, &cnt); kfs_info_t in; kfs_info(&in);
        static uint8_t rb[8192]; kfs_stat_t st; if (kfs_stat("/bin/prog", &st) == 0) { (void)kfs_read("/bin/prog", 0, rb, sizeof rb); (void)kfs_read("/bin/prog", 100000, rb, sizeof rb); }
        if (kfs_stat("/etc/note", &st) == 0) (void)kfs_read("/etc/note", 0, rb, sizeof rb);
    }
    return r == 0;
}

/* ---- сидовые корпуса ---- */
static uint8_t* slurp(const char* p, size_t* n) { FILE* f = fopen(p, "rb"); if (!f) { fprintf(stderr, "нет файла %s\n", p); exit(2); } fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); rewind(f); uint8_t* b = malloc(*n ? *n : 1); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f); return b; }
static void seed_files(int argc, char** argv) { for (int i = 0; i < argc; i++) { size_t n; uint8_t* b = slurp(argv[i], &n); add_seed(b, n); free(b); } }
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: fmt_fuzz ЦЕЛЬ N SEED [crash_dir] [файлы-сиды...]\n"); return 2; }
    tname = argv[1]; long long N = atoll(argv[2]); rs ^= (uint64_t)atoll(argv[3]) * 0x9E3779B97F4A7C15ULL; if (!rs) rs = 1; crash_dir = argc > 4 ? argv[4] : ".";
    int sa = 5; int (*fn)(uint8_t*, size_t) = 0; int expect_ok_seed = 1;
    signal(SIGSEGV, on_sig); signal(SIGABRT, on_sig); signal(SIGILL, on_sig); signal(SIGFPE, on_sig); signal(SIGBUS, on_sig);
    if (!strcmp(tname, "elf")) { fn = t_elf; seed_files(argc - sa, argv + sa); hot = 512; }
    else if (!strcmp(tname, "kex")) { fn = t_kex; seed_files(argc - sa, argv + sa); hot = 256; }
    else if (!strcmp(tname, "kdrv")) {
        fn = t_kdrv; hot = 160; kdrv_header_t h; memset(&h, 0, sizeof h); h.magic = KDRV_MAGIC; h.format = KDRV_FORMAT; h.hdr_size = sizeof h; strcpy(h.name, "edu"); h.version = 3; h.caps = CAP_MMIO | CAP_IRQ;
        h.elf_size = 64; h.nmatch = 2; h.match[0] = (kdrv_match_t){ 0x1234, 0x11e8, 0, 0 }; h.match[1] = (kdrv_match_t){ KDRV_ANY, KDRV_ANY, 0x020000, 0xFFFF00 };
        uint8_t pk[sizeof h + 64]; memcpy(pk, &h, sizeof h); memset(pk + sizeof h, 0x7f, 64); add_seed(pk, sizeof pk); if (argc > sa) seed_files(argc - sa, argv + sa);
    } else if (!strcmp(tname, "krec")) {
        fn = t_krec; hot = 216; kdrv_rec_t r; memset(&r, 0, sizeof r); r.magic = KREC_MAGIC; r.format = KDRV_FORMAT; r.rec_size = sizeof r; r.confirmed = KREC_CONFIRMED; r.caps_ok = CAP_MMIO; strcpy(r.source, "https://example/drv"); memset(r.elf_sha, 0xab, 32);
        add_seed(&r, sizeof r); r.confirmed = KREC_CONFIRMED_PUBLISHER; add_seed(&r, sizeof r);
    } else if (!strcmp(tname, "ksighdr") || !strcmp(tname, "ksig")) {
        hot = 144; fn = !strcmp(tname, "ksig") ? t_ksig : t_ksighdr; if (argc > sa) seed_files(argc - sa, argv + sa); else { fprintf(stderr, "нужен файл подписи-сида\n"); return 2; }
    } else if (!strcmp(tname, "kstate")) {
        fn = t_kstate; hot = KDRV_STATE_FILE_SIZE; kdrv_state_t st; kdrv_state_init(&st); st.max_time = 1750000000; st.nfloor = 2; strcpy(st.floor[0].name, "edu"); st.floor[0].floor = 5; strcpy(st.floor[1].name, "e1000"); st.floor[1].floor = 2; st.nrevoked = 1; memset(st.revoked[0], 0x42, 16);
        uint8_t o[KDRV_STATE_FILE_SIZE]; kdrv_state_encode(&st, o); add_seed(o, sizeof o);
    } else if (!strcmp(tname, "kbi") || !strcmp(tname, "kbi2mb") || !strcmp(tname, "mb2")) {
        hot = 200; fn = !strcmp(tname, "kbi") ? t_kbi : !strcmp(tname, "kbi2mb") ? t_kbi2mb : t_mb2;
        uint8_t b[512]; memset(b, 0, sizeof b); kbi_header_t* h = (kbi_header_t*)b; h->magic = KIRA_BOOTINFO_MAGIC; h->version = 1; h->header_size = sizeof *h; h->arch = KBI_ARCH_X86_64; h->boot_mode = KBI_BOOT_UEFI;
        h->mmap_off = sizeof *h; h->mmap_count = 2; h->mmap_entry_size = sizeof(kbi_mmap_entry_t); kbi_mmap_entry_t* m = (kbi_mmap_entry_t*)(b + h->mmap_off); m[0].base = 0; m[0].length = 0x9f000; m[0].type = KBI_MEM_USABLE; m[1].base = 0x100000; m[1].length = 0x7f00000; m[1].type = KBI_MEM_USABLE;
        h->modules_off = h->mmap_off + 2 * sizeof *m; h->modules_count = 1; h->module_entry_size = sizeof(kbi_module_t); kbi_module_t* md = (kbi_module_t*)(b + h->modules_off); md->base = 0x400000; md->length = 4096; md->name_off = h->modules_off + sizeof *md; md->name_len = 5; memcpy(b + md->name_off, "init\0", 5);
        h->cmdline_off = md->name_off + 8; h->cmdline_len = 6; memcpy(b + h->cmdline_off, "quiet\0", 6); h->flags = KBI_F_CMDLINE; h->total_size = (h->cmdline_off + 6 + 7) & ~7u; h->kernel_phys_base = 0x100000; h->kernel_size = 0x200000;
        { int vr = kbi_validate(b, h->total_size); if (vr != 0) { fprintf(stderr, "сид kbi невалиден: %d\n", vr); return 2; } }
        if (fn == t_mb2) { static uint8_t mb[65536]; uint32_t os; if (kbi_to_multiboot2(b, h->total_size, mb, sizeof mb, &os) != 0) { fprintf(stderr, "сид mb2 не построен\n"); return 2; } add_seed(mb, os); }
        else add_seed(b, h->total_size);
    } else if (!strcmp(tname, "fdt")) { fn = t_fdt; hot = 4096; seed_files(argc - sa, argv + sa); }
    else if (!strcmp(tname, "kfs")) {
        fn = t_kfs; hot = 16 * 512 + 4096 * 3; memset(kdisk, 0, sizeof kdisk); if (kfs_format(&kdev, 0) != 0) { fprintf(stderr, "kfs_format\n"); return 2; }
        kfs_mkdir("/bin"); kfs_mkdir("/etc"); uint8_t d[20000]; for (unsigned i = 0; i < sizeof d; i++) d[i] = (uint8_t)(i * 7); kfs_write("/bin/prog", d, sizeof d); kfs_write("/etc/note", "hello", 5);
        for (int i = 0; i < 20; i++) { char nm[32]; snprintf(nm, sizeof nm, "/bin/f%d", i); kfs_write(nm, d, 100 + i * 50); }
        static uint8_t snap[KSECT * 512]; memcpy(snap, kdisk, sizeof snap); size_t used = sizeof snap; while (used > 4096 && !snap[used - 1]) used--;   /* хвост нулей не нужен */
        add_seed(snap, used);
    } else { fprintf(stderr, "неизвестная цель %s\n", tname); return 2; }
    if (!ncorp) { fprintf(stderr, "пустой корпус\n"); return 2; }
    for (unsigned i = 0; i < nseed; i++) { cur = corpus[i].p; curn = corpus[i].n; int ok = fn(corpus[i].p, corpus[i].n); if (expect_ok_seed && !ok && strcmp(tname, "ksighdr") && strcmp(tname, "kbi2mb")) { fprintf(stderr, "ВНИМАНИЕ: сид %u не принят парсером\n", i); } }
    memset(hist, 0, sizeof hist);
    long long accepted = 0, grew = 0; clock_t t0 = clock();
    for (long long it = 1; it <= N; it++) {
        iter_no = (unsigned long long)it; buf_t* s = &corpus[rnd() % ncorp]; buf_t m = mutate(s);
        /* структурная ветка: у форматов с контрольной суммой иногда пересчитываем её после мутации */
        if (!strcmp(tname, "kstate") && m.n == KDRV_STATE_FILE_SIZE && (rnd() & 1)) sha256(m.p, 920, m.p + 920);
        if (!strcmp(tname, "kex") && m.n >= 96 && (rnd() & 1)) { uint8_t z[96]; memcpy(z, m.p, 96); memset(z + 64, 0, 32); sha256_ctx_t c; sha256_init(&c); sha256_update(&c, z, 96); sha256_update(&c, m.p + 96, m.n - 96); sha256_final(&c, m.p + 64); }
        cur = m.p; curn = m.n;
        int ok = fn(m.p, m.n);
        if (ok) { accepted++; if (ncorp < 512 && (m.n != s->n || memcmp(m.p, s->p, m.n))) { corpus[ncorp++] = m; grew++; m.p = 0; } }
        free(m.p);
        if ((it & 0xfffff) == 0) fprintf(stderr, "  %s: %lld/%lld, принято %lld, корпус %u\n", tname, it, N, accepted, ncorp);
    }
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("fuzz %s: запусков %lld, принято парсером %lld, корпус %u (сидов %u), CPU %.1f с (%.0f запусков/с); коды возврата:", tname, N, accepted, ncorp, nseed, sec, N / (sec > 0 ? sec : 1));
    for (int i = 0; i < 64; i++) if (hist[i]) printf(" %d:%d", i - 32, hist[i]);
    if (fdt_short) printf("\n(fdt: пропущено входов короче 40 Б, см. находку F1: %ld)", fdt_short);
    printf("\nOK: падений нет\n");
    return 0;
}
