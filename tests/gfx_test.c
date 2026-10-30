// Хост-тест графической части: 2D-примитивы, UTF-8, шрифт (кириллица), альфа-смешение,
// часы (TSC->ns), журнал ядра (кольцо), текстовая консоль на экране (перенос, скролл,
// UTF-8 через границы порций), Картинки сохраняются в /tmp/kira_gfx/*.ppm.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gfx2d.h"
#include "font.h"
#include "clock.h"
#include "klog.h"
#include "fbcon.h"
#include "fbpix.h"


static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void save_ppm(const char* path, const surface_t* s) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        uint32_t p = s->px[(long)y * s->pitch + x];
        unsigned char c[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static void klog_str(const char* s) { while (*s) klog_putc(*s++); }

int main(void) {
    mkdir("/tmp/kira_gfx", 0777);
    setvbuf(stdout, 0, _IONBF, 0);

    // ---- UTF-8 ----
    {
        const char* p = "A\xD0\x96\xE2\x82\xAC\xF0\x9F\x98\x80";   // A Ж € 😀
        CHECK(utf8_next(&p) == 'A');
        CHECK(utf8_next(&p) == 0x416);
        CHECK(utf8_next(&p) == 0x20AC);
        CHECK(utf8_next(&p) == 0x1F600);
        CHECK(utf8_next(&p) == 0);
        const char* bad = "\xC0\xAF";                              // "переразмеренная" запись '/': запрещена
        CHECK(utf8_next(&bad) == 0xFFFD);
        const char* trunc = "\xE2\x82";                             // обрыв
        CHECK(utf8_next(&trunc) == 0xFFFD);
        const char* lone = "\x80z";
        CHECK(utf8_next(&lone) == 0xFFFD && utf8_next(&lone) == 'z');
        const char* sur = "\xED\xA0\x80";                           // суррогат U+D800
        CHECK(utf8_next(&sur) == 0xFFFD);
    }

    // ---- примитивы и обрезка ----
    {
        static uint32_t px[100 * 60];
        surface_t s = { px, 100, 60, 100 };
        gfx_fill(&s, 0x112233);
        CHECK(px[0] == 0x112233 && px[100 * 60 - 1] == 0x112233);
        gfx_rect(&s, -10, -10, 20, 20, 0xFF0000);                   // выходит за край: обрезается
        CHECK(px[0] == 0xFF0000 && px[9 * 100 + 9] == 0xFF0000 && px[10 * 100 + 10] == 0x112233 && px[9 * 100 + 10] == 0x112233);
        gfx_rect(&s, 90, 50, 100, 100, 0x00FF00);
        CHECK(px[59 * 100 + 99] == 0x00FF00 && px[49 * 100 + 95] == 0x112233);
        gfx_pixel(&s, -1, 5, 1); gfx_pixel(&s, 5, 60, 1); gfx_pixel(&s, 100, 5, 1);  // не должно падать/писать
        gfx_line(&s, 10, 30, 60, 30, 0xABCDEF);
        CHECK(px[30 * 100 + 10] == 0xABCDEF && px[30 * 100 + 60] == 0xABCDEF && px[30 * 100 + 35] == 0xABCDEF);
        gfx_line(&s, 20, 40, 40, 40 + 15, 0x5555);                  // диагональ
        CHECK(px[40 * 100 + 20] == 0x5555 && px[55 * 100 + 40] == 0x5555);
        gfx_line(&s, -50, -50, 150, 150, 0x7777);                   // далеко за границами
        gfx_frame(&s, 70, 5, 20, 10, 0x1234);
        CHECK(px[5 * 100 + 70] == 0x1234 && px[14 * 100 + 89] == 0x1234 && px[8 * 100 + 80] != 0x1234);
        // нулевые/отрицательные размеры
        gfx_rect(&s, 5, 5, 0, 10, 1); gfx_rect(&s, 5, 5, -3, 10, 1); gfx_frame(&s, 5, 5, 0, 0, 1);
    }

    // ---- blit (обрезка с обеих сторон) и альфа ----
    {
        static uint32_t a[16 * 16], b[8 * 8];
        surface_t dst = { a, 16, 16, 16 }, src = { b, 8, 8, 8 };
        for (int i = 0; i < 64; i++) b[i] = 0x01000000u * 0 + 0x00FF0000 + (uint32_t)i;
        gfx_fill(&dst, 0);
        gfx_blit(&dst, 12, 12, &src, 0, 0, 8, 8);                   // правый нижний угол: обрезан до 4x4
        CHECK(a[12 * 16 + 12] == b[0] && a[15 * 16 + 15] == b[3 * 8 + 3] && a[11 * 16 + 12] == 0);
        gfx_blit(&dst, -3, -3, &src, 0, 0, 8, 8);                   // левый верхний: видна часть 5x5 из (3,3)
        CHECK(a[0] == b[3 * 8 + 3] && a[4 * 16 + 4] == b[7 * 8 + 7] && a[5 * 16 + 5] == 0);
        gfx_blit(&dst, 0, 0, &src, 100, 100, 8, 8);                 // источник вне границ: ничего
        gfx_fill(&dst, 0x00FFFFFF);
        for (int i = 0; i < 64; i++) b[i] = 0xFF000000u | 0x112233;       // непрозрачный
        gfx_blit_alpha(&dst, 0, 0, &src, 0, 0, 8, 8);
        CHECK(a[0] == 0x112233);
        for (int i = 0; i < 64; i++) b[i] = 0x00000000u | 0x112233;       // прозрачный: dst не меняется
        gfx_fill(&dst, 0x445566);
        gfx_blit_alpha(&dst, 0, 0, &src, 0, 0, 8, 8);
        CHECK(a[0] == 0x445566);
        for (int i = 0; i < 64; i++) b[i] = 0x80000000u | 0xFFFFFF;       // 50% белого на (0,0,0)
        gfx_fill(&dst, 0);
        gfx_blit_alpha(&dst, 0, 0, &src, 0, 0, 8, 8);
        unsigned r = (a[0] >> 16) & 255; CHECK(r == 128);                  // 255*128/255 = 128
        // инвариант: альфа-смешение между двумя цветами лежит между ними
        for (unsigned al = 0; al < 256; al += 5) {
            for (int i = 0; i < 64; i++) b[i] = (al << 24) | 0x00C080F0;
            gfx_fill(&dst, 0x00102030);
            gfx_blit_alpha(&dst, 0, 0, &src, 0, 0, 8, 8);
            unsigned rr = (a[0] >> 16) & 255, lo = 0x10, hi = 0xC0;
            CHECK(rr >= lo && rr <= hi);
        }
    }

    // ---- шрифт ----
    {
        const uint8_t* ga = font_glyph('A');
        int bits = 0; for (int i = 0; i < 16; i++) bits += __builtin_popcount(ga[i]);
        CHECK(bits > 10);
        CHECK(font_glyph(0x416) != font_glyph('A'));                  // Ж есть
        const uint8_t* gm = font_glyph(0x10FFFF);                     // неизвестный: рамка, не NULL
        CHECK(gm != 0 && gm[2] == 0x7E);
        CHECK(font_glyph(0x2588)[5] == 0xFF);                         // █
        CHECK(memcmp(font_glyph(0x20), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
        // все ожидаемые русские буквы имеют непустой глиф
        int empty = 0;
        for (uint32_t cp = 0x410; cp <= 0x44F; cp++) { int any = 0; for (int i = 0; i < 16; i++) any |= font_glyph(cp)[i]; if (!any) empty++; }
        CHECK(empty == 0);
        CHECK(gfx_text_width("Привет") == 6 * 8 && gfx_text_width("ab\ncde") == 3 * 8);
    }

    // ---- часы ----
    {
        CHECK(clock_tsc_to_ns(3000000, 3000000) == 1000000);                                   // 1 мс при 3 ГГц
        CHECK(clock_tsc_to_ns(3000000000ULL, 3000000) == 1000000000ULL);                       // 1 с
        CHECK(clock_tsc_to_ns(3000000000ULL * 86400 * 365, 3000000) == 1000000000ULL * 86400 * 365); // год без переполнения
        CHECK(clock_khz_from_cpuid15(2, 166, 24000000) == 1992000);                            // кристалл 24 МГц x 166/2
        CHECK(clock_khz_from_cpuid15(0, 166, 24000000) == 0 && clock_khz_from_cpuid15(2, 166, 0) == 0);   // лист без частоты
        CHECK(clock_tsc_to_ns(1, 3000000) == 0);
        CHECK(clock_tsc_to_ns(3, 3000000) == 1);
        CHECK(clock_tsc_to_ns(123456, 0) == 0);
        uint64_t prev = 0; int mono = 1;
        for (uint64_t c = 0; c < 10000000; c += 997) { uint64_t n = clock_tsc_to_ns(c, 2494322); if (n < prev) mono = 0; prev = n; }
        CHECK(mono);
    }

    // ---- перевод пикселей для видеопамяти 24/16/15 бит ----
    {
        uint32_t src[3] = { 0x00FF8040, 0x00000000, 0x00FFFFFF };
        uint8_t d[16];
        fbpix_fmt_t f32 = { 4, 16, 8, 8, 8, 0, 8 }, f24 = { 3, 16, 8, 8, 8, 0, 8 }, f16 = { 2, 11, 5, 5, 6, 0, 5 }, f15 = { 2, 10, 5, 5, 5, 0, 5 };
        fbpix_fmt_t bgr = { 4, 0, 8, 8, 8, 16, 8 }, bad = { 2, 11, 5, 5, 6, 12, 5 };
        CHECK(fbpix_valid(&f32, 32) == 0 && fbpix_is_native(&f32) && !fbpix_is_native(&f24) && !fbpix_is_native(&bgr));
        CHECK(fbpix_valid(&f24, 24) == 0 && fbpix_valid(&f16, 16) == 0 && fbpix_valid(&f15, 15) == 0 && fbpix_valid(&bad, 16) != 0);
        fbpix_row(d, src, 3, &f24);
        CHECK(d[0] == 0x40 && d[1] == 0x80 && d[2] == 0xFF && d[3] == 0 && d[6] == 0xFF && d[7] == 0xFF && d[8] == 0xFF);
        fbpix_row(d, src, 1, &f16);
        CHECK((d[0] | d[1] << 8) == ((0x1F << 11) | (0x20 << 5) | 0x08));
        fbpix_row(d, src, 1, &f15);
        CHECK((d[0] | d[1] << 8) == ((0x1F << 10) | (0x10 << 5) | 0x08));
        fbpix_row(d, src, 1, &bgr);
        CHECK(d[0] == 0xFF && d[1] == 0x80 && d[2] == 0x40);
    }

    // ---- текстовая консоль ----
    {
        enum { W = 1024, H = 768 };
        static uint32_t px[W * H];
        surface_t scr = { px, W, H, W };
        fbcon_init(&scr);
        int cols, rows; fbcon_get_size(&cols, &rows);
        CHECK(cols == 128 && rows == 48);
        int y0, y1;
        klog_str("Kira OS: привет, мир!\n");
        CHECK(fbcon_update(&y0, &y1) > 0);
        CHECK(y0 == 0 && y1 == 16);                                    // изменилась ровно одна строка
        // глиф 'K' в клетке (0,0) нарисован цветом текста
        int any = 0; for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) if (px[y * W + x] == FBCON_FG) any = 1;
        CHECK(any);
        // кириллическая 'п' в клетке 9 (после "Kira OS: ") не пустая
        int cyr = 0; for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) if (px[y * W + 9 * 8 + x] == FBCON_FG) cyr = 1;
        CHECK(cyr);
        // UTF-8 через границу порций: 'Ж' (2 байта) режем между вызовами update
        klog_putc('\xD0');
        fbcon_update(&y0, &y1);
        klog_putc('\x96'); klog_putc('\n');
        fbcon_update(&y0, &y1);
        int zh = 0; for (int y = 16; y < 32; y++) for (int x = 0; x < 8; x++) if (px[y * W + x] == FBCON_FG) zh = 1;
        CHECK(zh);
        // длинная строка переносится
        for (int i = 0; i < 130; i++) klog_putc('#');
        klog_putc('\n');
        fbcon_update(&y0, &y1);
        // скролл: печатаем больше строк, чем помещается
        for (int i = 0; i < 70; i++) { char l[32]; snprintf(l, sizeof l, "line %02d\n", i); klog_str(l); }
        fbcon_update(&y0, &y1);
        CHECK(y0 == 0 && y1 == rows * 16);                             // скролл помечает весь текст
        // Нижняя непустая строка "line 69": ищем цветные пиксели в строке rows-2 (последняя - пустая после \n)
        int found = 0; for (int y = (rows - 2) * 16; y < (rows - 1) * 16; y++) for (int x = 0; x < 64; x++) if (px[y * W + x] == FBCON_FG) found = 1;
        CHECK(found);
        // верх экрана уже не содержит "Kira OS" (ушёл за край): первая строка = line 22 (70 - 47 + 1... любая "line")
        // полная перерисовка даёт тот же результат, что и инкрементальный вывод
        static uint32_t snap[W * H];
        memcpy(snap, px, sizeof snap);
        fbcon_repaint();
        int diff = 0; for (int i = 0; i < W * H; i++) if (snap[i] != px[i]) diff++;
        printf("repaint vs incremental differences: %d px\n", diff);
        CHECK(diff == 0);
        save_ppm("/tmp/kira_gfx/console.ppm", &scr);
    }

    // ---- журнал ----
    {
        uint64_t base = klog_total(), cur = base; char buf[64]; unsigned lost = 9;
        klog_str("hello");
        unsigned n = klog_read(&cur, buf, 64, &lost);
        CHECK(n == 5 && !memcmp(buf, "hello", 5) && lost == 0);
        CHECK(klog_read(&cur, buf, 64, &lost) == 0);
        for (int i = 0; i < KLOG_SIZE + 100; i++) klog_putc((char)('a' + i % 26));   // читатель отстал на круг
        n = klog_read(&cur, buf, 64, &lost);
        CHECK(lost == 1 && n == 64);
        CHECK(klog_total() == base + 5 + KLOG_SIZE + 100);
        // курсор должен был перескочить к самым старым сохранившимся байтам
        uint64_t oldest = klog_total() - KLOG_SIZE;
        CHECK(buf[0] == (char)('a' + (oldest - base - 5) % 26));
    }

    printf("%s\n", fails ? "GFX TESTS FAILED" : "ALL GFX TESTS PASSED");
    return fails != 0;
}
