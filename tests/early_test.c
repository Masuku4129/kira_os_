// Хост-тест раннего вывода: перенос Multiboot2-структуры (mbinfo), ранняя консоль на буфере кадра
// и в текстовом режиме (earlycon), дамп регистров (crashdump). bootmap подменён заглушкой,
// видеопамять - обычные буферы в памяти теста.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "earlycon.h"
#include "mbinfo.h"
#include "crashdump.h"
#include "klog.h"
#include "font.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- заглушки ядра ------------------------------------------------------
static char said[1 << 16];
void uart_print(const char* s) { strncat(said, s, sizeof said - strlen(said) - 1); }
void uart_print_dec(uint64_t v) { char b[32]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); uart_print(b); }
void uart_print_hex(uint64_t v) { char b[32]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); uart_print(b); }

static int bm_init_rc;
static uint8_t* bm_host;
static uint64_t bm_phys_base;
static unsigned bm_map_calls, bm_release_calls;
static uint64_t bm_last_phys, bm_last_len;
static int bm_last_uc;
// Как настоящий bootmap.c: пока окно не установлено вызовом bootmap_init, отображать нечего.
static int bm_ready;
static unsigned bm_init_calls;
int bootmap_init(void) { bm_init_calls++; if (bm_init_rc == 0) bm_ready = 1; return bm_init_rc; }
void* bootmap_map(uint64_t phys, uint64_t len, int uc) {
    bm_map_calls++; bm_last_phys = phys; bm_last_len = len; bm_last_uc = uc;
    if (!bm_ready) return 0;
    return bm_host ? bm_host + (phys - bm_phys_base) : 0;
}
void bootmap_release_all(void) { bm_release_calls++; }

// ---- вспомогательное ----------------------------------------------------
#define BGR { 16, 8, 8, 8, 0, 8 }
#define RGB { 0, 8, 8, 8, 16, 8 }

static uint8_t* fbmem;
static uint32_t FBW, FBH, FBP;

static uint32_t px(uint32_t x, uint32_t y) { return *(uint32_t*)(fbmem + (uint64_t)y * FBP + (uint64_t)x * 4); }

static int cell_is(uint32_t col, uint32_t row, uint32_t cp, uint32_t fg, uint32_t bg, uint32_t scale) {
    const uint8_t* g = font_glyph(cp);
    for (uint32_t y = 0; y < FONT_H; y++)
        for (uint32_t x = 0; x < FONT_W; x++) {
            uint32_t want = (g[y] & (0x80u >> x)) ? fg : bg;
            for (uint32_t sy = 0; sy < scale; sy++)
                for (uint32_t sx = 0; sx < scale; sx++)
                    if (px(col * FONT_W * scale + x * scale + sx, row * FONT_H * scale + y * scale + sy) != want) return 0;
        }
    return 1;
}

static int row_is_all(uint32_t row, uint32_t scale, uint32_t color) {
    for (uint32_t y = row * FONT_H * scale; y < (row + 1) * FONT_H * scale; y++)
        for (uint32_t x = 0; x < FBW; x++) if (px(x, y) != color) return 0;
    return 1;
}

static void puts_ec(const char* s) { while (*s) earlycon_putc(*s++); }

static void rgb_setup(uint32_t w, uint32_t h, const earlycon_pixfmt_t* f) {
    free(fbmem);
    FBW = w; FBH = h; FBP = w * 4 + 64;
    fbmem = malloc((size_t)FBP * h);
    memset(fbmem, 0x5A, (size_t)FBP * h);        // мусор: attach обязан очистить экран
    CHECK(earlycon_attach_rgb(fbmem, w, h, FBP, f) == 0);
}

static uint8_t* add_tag(uint8_t* p, uint32_t type, uint32_t size, const void* body, uint32_t body_len) {
    memset(p, 0, (size + 7) & ~7u);
    *(uint32_t*)p = type; *(uint32_t*)(p + 4) = size;
    if (body_len) memcpy(p + 8, body, body_len);
    return p + ((size + 7) & ~7u);
}

typedef struct {
    uint64_t addr; uint32_t pitch, width, height; uint8_t bpp, fb_type; uint16_t reserved;
    uint8_t rp, rs, gp, gs, bp, bs;
} __attribute__((packed)) fbbody_t;

static uint32_t build_info(uint8_t* buf, const fbbody_t* fb, uint32_t fb_tag_size) {
    uint8_t* p = buf + 8;
    p = add_tag(p, 1, 9, "x", 1);
    if (fb) p = add_tag(p, 8, fb_tag_size, fb, fb_tag_size - 8);
    p = add_tag(p, 0, 8, 0, 0);
    uint32_t total = (uint32_t)(p - buf);
    *(uint32_t*)buf = total; *(uint32_t*)(buf + 4) = 0;
    return total;
}

// ---- crashdump ----------------------------------------------------------
static char dump[8192];
static void dump_out(const char* s) { strncat(dump, s, sizeof dump - strlen(dump) - 1); }

static void test_crashdump(void) {
    uint64_t f[CRASH_FRAME_QWORDS];
    for (unsigned i = 0; i < CRASH_FRAME_QWORDS; i++) f[i] = 0x1111000000000000ULL + i;
    f[14] = 0xAAAAAAAAAAAAAAAAULL;    // rax
    f[17] = 0x123456789ABCDEF0ULL;    // rip
    f[20] = 0x0000000000200000ULL;    // rsp
    crash_regs_t cr = { 0x80050033ULL, 0xDEAD0000ULL, 0x12D000ULL, 0x6A0ULL };
    uint64_t st[8] = { 1, 2, 3, 4, 5, 6, 7, 0xFEEDFACECAFEBEEFULL };
    dump[0] = 0;
    crashdump_print(f, &cr, st, dump_out);
    CHECK(strstr(dump, "RAX=AAAAAAAAAAAAAAAA"));
    CHECK(strstr(dump, "RIP=123456789ABCDEF0"));
    CHECK(strstr(dump, "RSP=0000000000200000"));
    CHECK(strstr(dump, "R15=1111000000000000"));      // индекс 0 - r15
    CHECK(strstr(dump, "R8 =1111000000000007"));      // индекс 7 - r8
    CHECK(strstr(dump, "CR2=00000000DEAD0000"));
    CHECK(strstr(dump, "CR0=0000000080050033"));
    CHECK(strstr(dump, "S7=FEEDFACECAFEBEEF"));
    CHECK(strstr(dump, "CS=1111000000000012"));       // индекс 18
    CHECK(strstr(dump, "SS=1111000000000015"));       // индекс 21
    // каждая строка начинается с метки и целая
    unsigned lines = 0;
    for (const char* p = dump; *p; ) { CHECK(strncmp(p, "[KIRA FATAL] ", 13) == 0); const char* nl = strchr(p, '\n'); CHECK(nl != 0); if (!nl) break; p = nl + 1; lines++; }
    CHECK(lines == 8);
    dump[0] = 0;
    crashdump_print(f, 0, 0, dump_out);
    CHECK(strstr(dump, "Stack at RSP is not readable"));
    CHECK(!strstr(dump, "CR0="));
}

// ---- mbinfo -------------------------------------------------------------
static void test_mbinfo(void) {
    uint8_t* info = aligned_alloc(8, 4096);
    fbbody_t fb = { 0xFD000000ULL, 4096, 1024, 768, 32, 1, 0, 16, 8, 8, 8, 0, 8 };
    uint32_t total = build_info(info, &fb, 8 + sizeof fb);
    const uint8_t* t = mbinfo_find_tag((unsigned long)(uintptr_t)info, 8);
    CHECK(t && *(const uint32_t*)t == 8);
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)info, 99) == 0);
    CHECK(mbinfo_find_tag(0, 8) == 0);
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)info + 4, 8) == 0);            // не выровнено
    uint8_t* bad = aligned_alloc(8, 4096);
    memcpy(bad, info, total);
    *(uint32_t*)(bad + 8 + 16 + 4) = 4;                                            // размер тега < 8
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);
    memcpy(bad, info, total);
    *(uint32_t*)(bad + 8 + 16 + 4) = 5000;                                         // тег вылезает за структуру
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);
    *(uint32_t*)bad = 12;                                                          // общий размер слишком мал
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 1) == 0);

    // Строгая структура: выравнивание, end tag обязателен и должен быть последним.
    memcpy(bad, info, total);
    *(uint32_t*)bad = total - 1;
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);                  // total_size не кратен 8
    memcpy(bad, info, total);
    *(uint32_t*)(bad + total - 8) = 99;
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);                  // отсутствует end tag
    memcpy(bad, info, total);
    *(uint32_t*)(bad + total - 4) = 16;
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);                  // end tag имеет неверный размер
    memcpy(bad, info, total);
    *(uint32_t*)(bad + 12) = 0xFFFFFFF8u;
    CHECK(mbinfo_find_tag((unsigned long)(uintptr_t)bad, 8) == 0);                  // округление/размер тега не переполняют обход

    // relocate: неверный magic / адрес - без изменений и без обращений к окну
    bm_map_calls = 0; said[0] = 0; bm_init_rc = 0;
    CHECK(mbinfo_relocate(0x1234, 0x180000000UL) == 0x180000000UL);
    CHECK(mbinfo_relocate(0x36D76289UL, 0) == 0);
    CHECK(mbinfo_relocate(0x36D76289UL, 0x180000004UL) == 0x180000004UL);
    CHECK(mbinfo_relocate(0x36D76289UL, UINTPTR_MAX - 7) == UINTPTR_MAX - 7);      // сложение физического адреса не переполняется
    CHECK(bm_map_calls == 0);

    // структура внутри identity-карты: возвращается тот же адрес, окно не трогается
    uint8_t* low = mmap((void*)0x10000000UL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(low == (uint8_t*)0x10000000UL);
    if (low != MAP_FAILED) {
        build_info(low, &fb, 8 + sizeof fb);
        CHECK(mbinfo_relocate(0x36D76289UL, 0x10000000UL) == 0x10000000UL);
        CHECK(bm_map_calls == 0);
        *(uint32_t*)low = 8;                                                        // повреждённый размер внутри карты
        CHECK(mbinfo_relocate(0x36D76289UL, 0x10000000UL) == 0x10000000UL);
        CHECK(bm_map_calls == 0);
    }

    // выше 1 ГБ: копия во внутренний буфер, содержимое совпадает, окно запрошено как обычная память
    uint64_t hi = 0x180000000ULL;
    bm_host = info; bm_phys_base = hi; bm_map_calls = 0; said[0] = 0;
    unsigned long r = mbinfo_relocate(0x36D76289UL, (unsigned long)hi);
    CHECK(r != 0 && r != (unsigned long)hi);
    CHECK(r && memcmp((void*)r, info, total) == 0);
    CHECK(bm_map_calls == 2 && bm_last_phys == hi && bm_last_len == total && bm_last_uc == 0);
    CHECK(strstr(said, "above the 1 GB identity map: copied"));
    CHECK(mbinfo_find_tag(r, 8) != 0);                                              // копия разбирается как обычная структура

    // огромный размер в заголовке - отказ, адрес остаётся прежним
    *(uint32_t*)bad = 70000; memcpy(bad + 4, info + 4, 4);
    bm_host = bad; said[0] = 0;
    CHECK(mbinfo_relocate(0x36D76289UL, (unsigned long)hi) == (unsigned long)hi);
    CHECK(strstr(said, "invalid size"));
    // окно не ставится
    bm_host = info; bm_init_rc = -1; said[0] = 0;
    CHECK(mbinfo_relocate(0x36D76289UL, (unsigned long)hi) == (unsigned long)hi);
    CHECK(strstr(said, "cannot be installed"));
    bm_init_rc = 0;
    // отображение не удалось
    bm_host = 0; said[0] = 0;
    CHECK(mbinfo_relocate(0x36D76289UL, (unsigned long)hi) == (unsigned long)hi);
    CHECK(strstr(said, "Cannot map"));

    // структура начинается ниже 1 ГБ, но пересекает границу: тоже через окно
    uint8_t* edge = mmap((void*)0x3FFFF000UL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (edge != MAP_FAILED) {
        uint8_t* at = edge + 0xFF0;                                                 // 0x3FFFFFF0
        *(uint32_t*)at = 64; *(uint32_t*)(at + 4) = 0;
        bm_host = at; bm_phys_base = 0x3FFFFFF0ULL; bm_map_calls = 0;
        // содержимое за границей страницы теста недоступно, поэтому проверяем только путь: окно запрошено
        uint8_t* big = aligned_alloc(8, 4096); memcpy(big, info, total); *(uint32_t*)big = 64;
        bm_host = big;
        unsigned long rr = mbinfo_relocate(0x36D76289UL, 0x3FFFFFF0UL);
        CHECK(bm_map_calls == 2 && rr != 0x3FFFFFF0UL);
        free(big);
    }
    free(info); free(bad);
}

// ---- earlycon -----------------------------------------------------------
static void test_rgb(void) {
    earlycon_pixfmt_t bgr = BGR, rgb = RGB;
    rgb_setup(1024, 768, &bgr);
    uint32_t cols, rows; earlycon_geometry(&cols, &rows);
    CHECK(cols == 128 && rows == 48);
    CHECK(row_is_all(0, 1, 0) && row_is_all(47, 1, 0));                              // экран очищен
    const uint32_t FG = 0x00E0E0E0, ALERT = 0x00FF6060;

    puts_ec("AB");
    CHECK(cell_is(0, 0, 'A', FG, 0, 1) && cell_is(1, 0, 'B', FG, 0, 1));
    uint32_t cx, cy; earlycon_cursor(&cx, &cy);
    CHECK(cx == 2 && cy == 0);

    puts_ec("\nC\rD");                                                               // \r возвращает в начало строки
    CHECK(cell_is(0, 1, 'D', FG, 0, 1));
    puts_ec("\t");                                                                   // до ближайшей кратной 8 колонки
    earlycon_cursor(&cx, &cy); CHECK(cx == 8 && cy == 1);
    puts_ec("\bX");
    CHECK(cell_is(7, 1, 'X', FG, 0, 1));
    puts_ec("\001\177Y");                                                            // управляющие коды не рисуются
    CHECK(cell_is(8, 1, 'Y', FG, 0, 1));

    // UTF-8: кириллица рисуется глифом, оборванные и лишние байты не ломают строку
    puts_ec("\n\xD0\x96");
    CHECK(cell_is(0, 2, 0x416, FG, 0, 1));
    puts_ec("\x96");                                                                 // «хвост» без головы
    earlycon_cursor(&cx, &cy); CHECK(cx == 1);
    puts_ec("\xD0Z");                                                                // голова без хвоста, потом ASCII
    CHECK(cell_is(1, 2, 'Z', FG, 0, 1));

    // перенос длинной строки
    puts_ec("\n");
    for (uint32_t i = 0; i < cols + 3; i++) earlycon_putc('m');
    earlycon_cursor(&cx, &cy); CHECK(cy == 4 && cx == 3);
    CHECK(cell_is(cols - 1, 3, 'm', FG, 0, 1));

    // красный после тревоги
    earlycon_alert();
    puts_ec("\nE");
    earlycon_cursor(&cx, &cy);
    CHECK(cell_is(0, cy, 'E', ALERT, 0, 1));

    // очистка экрана
    puts_ec("\f");
    CHECK(row_is_all(0, 1, 0) && row_is_all(1, 1, 0));
    earlycon_cursor(&cx, &cy); CHECK(cx == 0 && cy == 0);

    // кольцо: после rows строк самая старая затирается, под последней строкой черта
    rgb_setup(1024, 768, &bgr);
    char ln[16];
    for (int i = 0; i < 50; i++) { snprintf(ln, sizeof ln, "L%d\n", i); puts_ec(ln); }
    earlycon_cursor(&cx, &cy); CHECK(cx == 0 && cy == 2);
    CHECK(cell_is(0, 0, 'L', FG, 0, 1) && cell_is(1, 0, '4', FG, 0, 1) && cell_is(2, 0, '8', FG, 0, 1));
    CHECK(cell_is(2, 1, '9', FG, 0, 1));
    CHECK(row_is_all(2, 1, 0));                                                      // текущая строка чистая
    CHECK(px(5, 3 * 16) == FG && px(1000, 3 * 16 + 1) == FG && px(5, 3 * 16 + 5) == 0);   // черта в верхних двух пикселях строки 3
    CHECK(cell_is(1, 4, '4', FG, 0, 1));                                             // дальше - старое

    // порядок каналов: RGB вместо BGR
    rgb_setup(1024, 768, &rgb);
    puts_ec("A");
    CHECK(cell_is(0, 0, 'A', 0x00E0E0E0, 0, 1));
    earlycon_alert(); puts_ec("A");
    CHECK(cell_is(1, 0, 'A', 0x006060FFu, 0, 1));                                    // красный в младших байтах

    // крупный экран: знакоместо увеличивается целым числом
    rgb_setup(2560, 1440, &bgr);
    earlycon_geometry(&cols, &rows);
    CHECK(cols == 160 && rows == 45);
    puts_ec("Q");
    CHECK(cell_is(0, 0, 'Q', FG, 0, 2));

    // отказы подключения
    CHECK(earlycon_attach_rgb(0, 1024, 768, 4096, &bgr) == -1);
    CHECK(earlycon_attach_rgb(fbmem, 100, 768, 4096, &bgr) == -1);                   // слишком узкий
    CHECK(earlycon_attach_rgb(fbmem, 1024, 768, 1000, &bgr) == -1);                  // pitch меньше строки
    earlycon_pixfmt_t zero = { 0, 0, 0, 0, 0, 0 };
    CHECK(earlycon_attach_rgb(fbmem, 1024, 768, 4096, &zero) == -1);
}

static void test_text(void) {
    uint16_t* t = calloc(80 * 25, 2);
    CHECK(earlycon_attach_text(t, 80, 25) == 0);
    CHECK(t[0] == 0x0720);                                                           // очищено пробелами с серым атрибутом
    puts_ec("Hi\n\xD0\x96!");
    CHECK(t[0] == 0x0748 && t[1] == 0x0769);
    CHECK(t[80] == 0x073F);                                                          // не-ASCII в текстовом режиме - '?'
    CHECK(t[81] == 0x0721);
    earlycon_alert(); puts_ec("\nR");
    CHECK(t[160] == 0x0C52);
    // кольцо
    CHECK(earlycon_attach_text(t, 80, 25) == 0);
    for (int i = 0; i < 27; i++) puts_ec("x\n");
    uint32_t cx, cy; earlycon_cursor(&cx, &cy);
    CHECK(cy == 2);
    CHECK(t[3 * 80] == 0x072D && t[3 * 80 + 79] == 0x072D);                          // черта '-' под текущей строкой
    CHECK(t[2 * 80] == 0x0720);
    CHECK(t[4 * 80] == 0x0778);
    CHECK(earlycon_attach_text(0, 80, 25) == -1 && earlycon_attach_text(t, 10, 25) == -1);
    free(t);
}

static void test_init(void) {
    uint8_t* info = aligned_alloc(8, 4096);
    FBW = 1024; FBH = 768; FBP = 4096;
    free(fbmem); fbmem = calloc(1, (size_t)FBP * FBH);
    bm_host = fbmem; bm_phys_base = 0xFD000000ULL;

    // журнал, накопленный до включения экрана, показывается после включения
    klog_putc('b'); klog_putc('o'); klog_putc('o'); klog_putc('t'); klog_putc('\n');

    fbbody_t fb = { 0xFD000000ULL, 4096, 1024, 768, 32, 1, 0, 16, 8, 8, 8, 0, 8 };
    build_info(info, &fb, 8 + sizeof fb);
    said[0] = 0; bm_map_calls = 0;
    // Загрузка, при которой Multiboot2-структура лежит ниже 1 ГБ: mbinfo_relocate окно не трогает,
    // ранняя консоль обязана поставить его сама (так было пропущено в v15: на экране было пусто).
    bm_ready = 0; bm_init_calls = 0; bm_init_rc = 0;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == 0);
    CHECK(bm_init_calls >= 1);
    CHECK(earlycon_active());
    CHECK(bm_last_phys == 0xFD000000ULL && bm_last_len == 4096ULL * 768 && bm_last_uc == 1);   // видеопамять - некэшируемая
    CHECK(cell_is(0, 0, 'b', 0x00E0E0E0, 0, 1) && cell_is(3, 0, 't', 0x00E0E0E0, 0, 1));

    // стоп: рисование прекращается, окно снято
    bm_release_calls = 0;
    earlycon_stop();
    CHECK(!earlycon_active() && bm_release_calls == 1);
    uint8_t* snap = malloc((size_t)FBP * FBH); memcpy(snap, fbmem, (size_t)FBP * FBH);
    puts_ec("zzz");
    CHECK(memcmp(snap, fbmem, (size_t)FBP * FBH) == 0);
    free(snap);

    // повторный запуск (экранный драйвер не поднялся): журнал рисуется заново с начала
    klog_putc('x'); klog_putc('\n');
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == 0);
    CHECK(cell_is(0, 0, 'b', 0x00E0E0E0, 0, 1));

    // тег EGA-текст (тип 2): страница видеопамяти приходит из загрузчика
    uint16_t* tbuf = calloc(80 * 25, 2);
    fbbody_t eg = { 0xB8000ULL, 160, 80, 25, 16, 2, 0, 0, 0, 0, 0, 0, 0 };
    build_info(info, &eg, 8 + 24);
    bm_host = (uint8_t*)tbuf; bm_phys_base = 0xB8000ULL;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == 0);
    CHECK(tbuf[0] == 0x0762);                                                       // 'b'
    uint32_t cols, rows; earlycon_geometry(&cols, &rows); CHECK(cols == 80 && rows == 25);
    free(tbuf);

    // 24 бита на пиксель, индексная палитра, обрезанный тег, нет памяти - понятные причины и отказ
    fbbody_t b24 = { 0xFD000000ULL, 3072, 1024, 768, 24, 1, 0, 16, 8, 8, 8, 0, 8 };
    build_info(info, &b24, 8 + sizeof b24); said[0] = 0; bm_host = fbmem; bm_phys_base = 0xFD000000ULL;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == -3 && strstr(said, "Unsupported bootloader framebuffer"));
    fbbody_t idx = { 0xFD000000ULL, 1024, 1024, 768, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
    build_info(info, &idx, 8 + sizeof idx); said[0] = 0;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == -3 && strstr(said, "type 0"));
    build_info(info, &fb, 16); said[0] = 0;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == -3 && strstr(said, "Truncated"));
    build_info(info, &fb, 8 + sizeof fb); said[0] = 0; bm_host = 0;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == -2 && strstr(said, "Cannot map video memory"));
    // окно установить нельзя (PML4[511] занята): честное сообщение и отказ, без обращения к видеопамяти
    { uint8_t* keep = bm_host; (void)keep; }
    bm_ready = 0; bm_init_rc = -1; said[0] = 0; bm_map_calls = 0;
    CHECK(earlycon_init((unsigned long)(uintptr_t)info) == -2 && strstr(said, "early window") && bm_map_calls == 0);
    bm_init_rc = 0;

    // тега нет: текстовый VGA по адресу 0xB8000 (в тесте - реальная страница по этому адресу)
    uint16_t* vga = mmap((void*)0xB8000UL, 4096 * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (vga == (uint16_t*)0xB8000UL) {
        build_info(info, 0, 0);
        CHECK(earlycon_init((unsigned long)(uintptr_t)info) == 0);
        CHECK(vga[0] == 0x0762);
        munmap(vga, 4096 * 2);
    } else printf("note: cannot map 0xB8000 here, VGA-without-tag path not run\n");
    free(info);
}

int main(void) {
    test_crashdump();
    test_mbinfo();
    test_rgb();
    test_text();
    test_init();
    if (fails) { printf("early: %d FAILED\n", fails); return 1; }
    printf("early: all checks passed\n");
    return 0;
}
