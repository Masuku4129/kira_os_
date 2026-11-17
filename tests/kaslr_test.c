// Хост-тест чистой части KASLR (kaslr.c) и политики kptr (kptr.c: kptr_format): выбор позиции (выравнивание, диапазон,
// занятое, энтропия), сбор диапазонов из BootInfo, таблица перемещений, слова командной строки, равномерность выбора.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kaslr.h"
#include "kptr.h"

void uart_print(const char* s) { (void)s; }
void uart_print_hex(uint64_t v) { (void)v; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define MB (1ULL << 20)

// ---- выбор позиции ----------------------------------------------------------------------------------------------
static void test_pick(void) {
    kaslr_range_t usable[] = { { 0x100000, 256 * MB } };            // 1 МБ..256 МБ, как QEMU с 256 МБ
    kaslr_range_t avoid[] = { { 0x100000, 0x100000 + 6 * MB },       // образ в старом месте
                              { 100 * MB, 110 * MB } };              // модуль (initrd)
    kaslr_env_t e = { usable, 1, avoid, 2, 8 * MB, KASLR_FLOOR, KASLR_CEIL };
    uint64_t n = kaslr_slots(&e);
    CHECK(n > 50);
    // каждая позиция: выровнена, внутри [floor, ram), не пересекает занятое
    uint64_t seen_lo = ~0ULL, seen_hi = 0;
    for (uint64_t k = 0; k < n; k++) {
        uint64_t base = 0, slots = 0;
        CHECK(kaslr_pick(&e, k, &base, &slots) == 0);
        CHECK(slots == n);
        CHECK((base & (KASLR_ALIGN - 1)) == 0);
        CHECK(base >= KASLR_FLOOR && base + 8 * MB <= 256 * MB);
        CHECK(!(base < 110 * MB && 100 * MB < base + 8 * MB));
        CHECK(!(base < 0x100000 + 6 * MB));
        if (base < seen_lo) seen_lo = base;
        if (base > seen_hi) seen_hi = base;
    }
    CHECK(seen_lo == KASLR_FLOOR);                                   // крайние позиции достижимы
    CHECK(seen_hi == 256 * MB - 8 * MB);
    // разные энтропии -> разные позиции (не константа)
    uint64_t b1, b2, b3;
    kaslr_pick(&e, 1, &b1, 0); kaslr_pick(&e, 2, &b2, 0); kaslr_pick(&e, 3, &b3, 0);
    CHECK(b1 != b2 && b2 != b3);
    // энтропия по модулю числа позиций: n позиций - все разные
    uint64_t b0, bn;
    kaslr_pick(&e, 0, &b0, 0); kaslr_pick(&e, n, &bn, 0);
    CHECK(b0 == bn);
}

static void test_no_room(void) {
    kaslr_range_t usable[] = { { 0x100000, 20 * MB } };
    kaslr_range_t avoid[] = { { 0x100000, 0x100000 + 6 * MB } };
    kaslr_env_t e = { usable, 1, avoid, 1, 8 * MB, KASLR_FLOOR, KASLR_CEIL };     // от 16 МБ до 20 МБ: 4 МБ < 8 МБ
    uint64_t base, slots;
    CHECK(kaslr_slots(&e) == 0);
    CHECK(kaslr_pick(&e, 5, &base, &slots) == -1 && slots == 0);
    CHECK(kaslr_slots(0) == 0);
    kaslr_env_t z = { usable, 1, avoid, 1, 0, KASLR_FLOOR, KASLR_CEIL };
    CHECK(kaslr_slots(&z) == 0);
}

static void test_ceiling_and_floor(void) {
    kaslr_range_t usable[] = { { 0x100000, 8ULL << 30 } };           // 8 ГБ ОЗУ: позиции только в первом гигабайте (identity-карта)
    kaslr_env_t e = { usable, 1, 0, 0, 4 * MB, KASLR_FLOOR, KASLR_CEIL };
    uint64_t n = kaslr_slots(&e), base;
    CHECK(n == (1024 - 16 - 4) / 2 + 1);                              // базы 16, 18, ..., 1020 МБ
    for (uint64_t k = 0; k < n; k++) {
        CHECK(kaslr_pick(&e, k, &base, 0) == 0);
        CHECK(base >= 16 * MB && base + 4 * MB <= (1ULL << 30));
    }
    kaslr_range_t two[] = { { 0x100000, 0x9F000 + 0x100000 }, { 64 * MB, 80 * MB } };   // две свободные области
    kaslr_env_t e2 = { two, 2, 0, 0, 2 * MB, KASLR_FLOOR, KASLR_CEIL };
    n = kaslr_slots(&e2);
    CHECK(n == 8);                                                    // область ниже floor отброшена; 64..78 МБ
    for (uint64_t k = 0; k < n; k++) {
        kaslr_pick(&e2, k, &base, 0);
        CHECK(base >= 64 * MB && base + 2 * MB <= 80 * MB);
    }
}

// Энтропия: выбор должен быть близок к равномерному, а kaslr_mix не должна давать нуль и одинаковые значения для разных входов.
static void test_entropy(void) {
    kaslr_range_t usable[] = { { 0x100000, 512 * MB } };
    kaslr_env_t e = { usable, 1, 0, 0, 8 * MB, KASLR_FLOOR, KASLR_CEIL };
    uint64_t n = kaslr_slots(&e);
    unsigned* hist = calloc(n, sizeof(unsigned));
    uint64_t acc = 0, zero = 0;
    const unsigned draws = 200000;
    for (unsigned i = 0; i < draws; i++) {
        acc = kaslr_mix(acc, i);
        if (acc == 0) zero++;
        uint64_t base;
        kaslr_pick(&e, acc, &base, 0);
        hist[(base - KASLR_FLOOR) / KASLR_ALIGN]++;
    }
    CHECK(zero == 0);
    double exp_ = (double)draws / (double)n;
    unsigned bad = 0;
    for (uint64_t k = 0; k < n; k++) if (hist[k] < exp_ * 0.6 || hist[k] > exp_ * 1.4) bad++;
    CHECK(bad == 0);
    free(hist);
    CHECK(kaslr_mix(0, 0) != 0);
    CHECK(kaslr_mix(1, 2) != kaslr_mix(2, 1));
    CHECK(kaslr_mix(0, 1) != kaslr_mix(0, 2));
}

// ---- BootInfo -> диапазоны ---------------------------------------------------------------------------------------
static uint8_t blob[4096] __attribute__((aligned(16)));

static uint32_t build_kbi(int nmods, int with_cmd, const char* cmd) {
    memset(blob, 0, sizeof blob);
    kbi_header_t* h = (kbi_header_t*)blob;
    kbi_mmap_entry_t* mm = (kbi_mmap_entry_t*)(blob + sizeof(*h));
    mm[0] = (kbi_mmap_entry_t){ 0, 0x9F000, KBI_MEM_USABLE, 0 };
    mm[1] = (kbi_mmap_entry_t){ 0xF0000, 0x10000, KBI_MEM_RESERVED, 0 };
    mm[2] = (kbi_mmap_entry_t){ 0x100000, 255 * MB, KBI_MEM_USABLE, 0 };
    mm[3] = (kbi_mmap_entry_t){ 0xB0000000ULL, 0x1000, KBI_MEM_RESERVED, 0 };
    uint32_t off = sizeof(*h) + 4 * sizeof(kbi_mmap_entry_t);
    h->magic = KIRA_BOOTINFO_MAGIC; h->version = KIRA_BOOTINFO_VERSION; h->header_size = sizeof(*h);
    h->arch = KBI_ARCH_X86_64; h->boot_mode = KBI_BOOT_BIOS;
    h->mmap_off = sizeof(*h); h->mmap_count = 4; h->mmap_entry_size = sizeof(kbi_mmap_entry_t);
    if (nmods) {
        h->modules_off = off; h->modules_count = nmods; h->module_entry_size = sizeof(kbi_module_t);
        kbi_module_t* m = (kbi_module_t*)(blob + off);
        off += nmods * sizeof(kbi_module_t);
        for (int i = 0; i < nmods; i++) {
            m[i].base = 100 * MB + (uint64_t)i * 4 * MB; m[i].length = 3 * MB;
            m[i].name_off = off; m[i].name_len = 5; memcpy(blob + off, "mod0", 5); blob[off + 3] = (uint8_t)('0' + i);
            off += 8;
        }
    }
    if (with_cmd) {
        h->flags |= KBI_F_CMDLINE; h->cmdline_off = off; h->cmdline_len = (uint32_t)strlen(cmd) + 1;
        memcpy(blob + off, cmd, h->cmdline_len);
        off += (h->cmdline_len + 7) & ~7u;
    }
    h->kernel_phys_base = 0x100000; h->kernel_size = 6 * MB;
    h->total_size = off;
    return off;
}

static void test_collect(void) {
    kaslr_mem_t* m = malloc(sizeof *m);
    uint32_t sz = build_kbi(2, 1, "quiet kira.debug");
    CHECK(kaslr_collect(blob, sz, 0x9000, 0x9400, m) == 0);
    CHECK(m->n_usable == 2);                                          // только USABLE
    CHECK(m->usable[0].lo == 0 && m->usable[1].lo == 0x100000 && m->usable[1].hi == 256 * MB);
    CHECK(m->ram_top == 256 * MB);
    // занято: модули (2), сам BootInfo, образ ядра, структура Multiboot2
    CHECK(m->n_avoid >= 4 && m->n_avoid <= 5);                       // BootInfo сам тоже занят (мог слиться с соседним диапазоном)
    int has_mod = 0, has_kernel = 0, has_mbi = 0;
    for (unsigned i = 0; i < m->n_avoid; i++) {
        if (m->avoid[i].lo == 100 * MB && m->avoid[i].hi == 103 * MB) has_mod = 1;
        if (m->avoid[i].lo <= 0x100000 && m->avoid[i].hi >= 0x100000 + 6 * MB) has_kernel = 1;
        if (m->avoid[i].lo == 0x9000 && m->avoid[i].hi == 0x9400) has_mbi = 1;
    }
    CHECK(has_mod && has_kernel && has_mbi);
    for (unsigned i = 1; i < m->n_avoid; i++) CHECK(m->avoid[i - 1].hi < m->avoid[i].lo);   // отсортировано, слито
    // выбор на собранных диапазонах не задевает ни модули, ни образ
    kaslr_env_t e = { m->usable, m->n_usable, m->avoid, m->n_avoid, 10 * MB, KASLR_FLOOR, KASLR_CEIL };
    uint64_t n = kaslr_slots(&e), base;
    CHECK(n > 0);
    for (uint64_t k = 0; k < n; k++) {
        kaslr_pick(&e, k, &base, 0);
        CHECK(!(base < 103 * MB && 100 * MB < base + 10 * MB));
        CHECK(!(base < 0x100000 + 6 * MB));
        CHECK(!(base < 0x9400 && 0x9000 < base + 10 * MB));
    }
    // неверный BootInfo отвергается
    CHECK(kaslr_collect(blob, 10, 0, 0, m) == -1);
    blob[0] ^= 1;
    CHECK(kaslr_collect(blob, sz, 0, 0, m) == -1);
    blob[0] ^= 1;
    CHECK(kaslr_collect(0, 0, 0, 0, m) == -1);
    free(m);
}

static void test_cmdline(void) {
    const char* c = "quiet kira.nokaslr x2apic";
    CHECK(kaslr_cmdline_has(c, (uint32_t)strlen(c) + 1, "kira.nokaslr"));
    CHECK(kaslr_cmdline_has(c, (uint32_t)strlen(c) + 1, "quiet"));
    CHECK(kaslr_cmdline_has(c, (uint32_t)strlen(c) + 1, "x2apic"));
    CHECK(!kaslr_cmdline_has(c, (uint32_t)strlen(c) + 1, "kira.nokasl"));
    CHECK(!kaslr_cmdline_has(c, (uint32_t)strlen(c) + 1, "kira.nokaslr2"));
    CHECK(!kaslr_cmdline_has("xkira.nokaslr", 14, "kira.nokaslr"));
    CHECK(!kaslr_cmdline_has("kira.nokaslr=0", 15, "kira.nokaslr"));
    CHECK(kaslr_cmdline_has("a\tkira.nokaslr", 15, "kira.nokaslr"));
    CHECK(!kaslr_cmdline_has("", 1, "kira.nokaslr"));
    CHECK(!kaslr_cmdline_has(0, 0, "kira.nokaslr"));
    CHECK(!kaslr_cmdline_has(c, 5, "kira.nokaslr"));                  // len обрезает: за пределами слова нет
}

// ---- таблица перемещений -----------------------------------------------------------------------------------------
static void w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void test_reloc(void) {
    enum { LOADED = 256 };
    uint8_t img[LOADED], ref[LOADED];
    uint8_t tbl[sizeof(kreloc_hdr_t) + 4 * 8];
    for (int i = 0; i < LOADED; i++) img[i] = (uint8_t)(i * 7);
    // два 64-битных слова (на смещениях 16 и 33 - невыровненное) и два 32-битных (64, 100)
    uint64_t a = 0x0000000000123456ULL, b = 0x00000000001FFFF0ULL; uint32_t c = 0x00200000, d = 0x00100040;
    memcpy(img + 16, &a, 8); memcpy(img + 33, &b, 8); memcpy(img + 64, &c, 4); memcpy(img + 100, &d, 4);
    memcpy(ref, img, sizeof img);
    kreloc_hdr_t* h = (kreloc_hdr_t*)tbl;
    h->magic = KRELOC_MAGIC; h->version = KRELOC_VERSION; h->n64 = 2; h->n32 = 2; h->loaded_size = LOADED;
    uint8_t* o = tbl + sizeof *h;
    w32(o, 16); w32(o + 4, 33); w32(o + 8, 64); w32(o + 12, 100);
    const uint32_t tl = sizeof(kreloc_hdr_t) + 16;
    CHECK(kreloc_check(tbl, tl, LOADED) == 0);
    const uint64_t slide = 0x3000000;
    CHECK(kreloc_apply(img, LOADED, tbl, tl, slide) == 4);
    uint64_t ra, rb; uint32_t rc, rd;
    memcpy(&ra, img + 16, 8); memcpy(&rb, img + 33, 8); memcpy(&rc, img + 64, 4); memcpy(&rd, img + 100, 4);
    CHECK(ra == a + slide && rb == b + slide && rc == c + slide && rd == d + slide);
    // остальные байты не тронуты
    for (int i = 0; i < LOADED; i++) {
        int in = (i >= 16 && i < 24) || (i >= 33 && i < 41) || (i >= 64 && i < 68) || (i >= 100 && i < 104);
        if (!in) CHECK(img[i] == ref[i]);
    }
    // повреждённые таблицы
    uint8_t bad[sizeof tbl];
    memcpy(bad, tbl, sizeof tbl);
    CHECK(kreloc_check(bad, tl - 1, LOADED) == -1);                   // короче заявленного
    CHECK(kreloc_check(bad, tl, LOADED + 8) == -1);                   // не тот размер образа
    ((kreloc_hdr_t*)bad)->magic ^= 1; CHECK(kreloc_check(bad, tl, LOADED) == -1);
    memcpy(bad, tbl, sizeof tbl);
    w32(bad + sizeof(kreloc_hdr_t), LOADED - 4);                      // 64-битное слово вылезает за образ
    CHECK(kreloc_check(bad, tl, LOADED) == -1);
    uint8_t img2[LOADED];
    memcpy(img2, ref, LOADED);
    CHECK(kreloc_apply(img2, LOADED, bad, tl, slide) == -1);
    // 32-битное слово не должно выйти за 2 ГБ (R_X86_64_32S)
    memcpy(img2, ref, LOADED);
    CHECK(kreloc_apply(img2, LOADED, tbl, tl, 0x7FF00000ULL) == -1);
    // пустая таблица допустима
    uint8_t empty[sizeof(kreloc_hdr_t)];
    kreloc_hdr_t* eh = (kreloc_hdr_t*)empty;
    *eh = (kreloc_hdr_t){ KRELOC_MAGIC, KRELOC_VERSION, 0, 0, LOADED };
    CHECK(kreloc_check(empty, sizeof empty, LOADED) == 0 && kreloc_apply(img2, LOADED, empty, sizeof empty, slide) == 0);
}

// ---- политика kptr -----------------------------------------------------------------------------------------------
static void test_kptr(void) {
    char b[32];
    const uint64_t lo = 0x30000000, hi = 0x30600000;     // совпадает с --defsym kernel_start/kernel_end в сборке теста
    kptr_format(0x30012345, lo, hi, 0, b);  CHECK(strcmp(b, "K+0x12345") == 0);
    kptr_format(lo, lo, hi, 0, b);          CHECK(strcmp(b, "K+0x0") == 0);
    kptr_format(hi - 1, lo, hi, 0, b);      CHECK(strcmp(b, "K+0x5fffff") == 0);
    kptr_format(hi, lo, hi, 0, b);          CHECK(strcmp(b, "0x30600000") == 0);       // вне образа - как есть
    kptr_format(0x1000, lo, hi, 0, b);      CHECK(strcmp(b, "0x1000") == 0);
    kptr_format(0x30012345, lo, hi, 1, b);  CHECK(strcmp(b, "0x30012345") == 0);       // режим отладки - настоящий адрес
    kptr_format(0, lo, hi, 0, b);           CHECK(strcmp(b, "0x0") == 0);
    kptr_format(~0ULL, lo, hi, 0, b);       CHECK(strcmp(b, "0xffffffffffffffff") == 0);
    // при любой базе образа в обычном режиме одинаковое смещение даёт одинаковую строку: база из вывода не восстановить
    char b2[32];
    kptr_format(0x30012345, lo, hi, 0, b);
    kptr_format(0x40012345, 0x40000000, 0x40600000, 0, b2);
    CHECK(strcmp(b, b2) == 0 && strncmp(b2, "K+", 2) == 0);
    // kptr_print/режим отладки: по умолчанию выключен, переключается
    CHECK(kptr_debug() == 0);
    kptr_set_debug(1); CHECK(kptr_debug() == 1);
    kptr_set_debug(0); CHECK(kptr_debug() == 0);
    CHECK(kptr_in_image(lo) && kptr_in_image(hi - 1) && !kptr_in_image(hi) && !kptr_in_image(lo - 1));
}

int main(void) {
    test_pick(); test_no_room(); test_ceiling_and_floor(); test_entropy();
    test_collect(); test_cmdline(); test_reloc(); test_kptr();
    if (fails) { printf("kaslr_test: %d FAILED\n", fails); return 1; }
    printf("kaslr_test: all passed\n");
    return 0;
}
