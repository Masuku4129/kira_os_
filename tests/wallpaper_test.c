/* Обои из картинки: разбор заголовка KWP1 и масштабирование "заполнить с обрезкой". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wallpaper.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t* make(int w, int h, size_t* n) {          /* красный растёт слева направо, зелёный - сверху вниз */
    *n = 12 + (size_t)w * h * 3;
    uint8_t* b = calloc(*n, 1);
    memcpy(b, "KWP1", 4);
    b[4] = (uint8_t)w; b[5] = (uint8_t)(w >> 8); b[8] = (uint8_t)h; b[9] = (uint8_t)(h >> 8);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t* p = b + 12 + ((size_t)y * w + x) * 3;
        p[0] = (uint8_t)(x * 255 / (w - 1)); p[1] = (uint8_t)(y * 255 / (h - 1)); p[2] = 77;
    }
    return b;
}

int main(void) {
    size_t n; uint8_t* b = make(64, 36, &n);
    int w, h; const uint8_t* rgb;
    CHECK(wallpaper_parse(b, n, &w, &h, &rgb) == 0 && w == 64 && h == 36 && rgb == b + 12, "корректный файл принят");
    CHECK(wallpaper_parse(b, n - 1, &w, &h, &rgb) != 0, "обрезанный файл отвергнут");
    CHECK(wallpaper_parse(b, 8, &w, &h, &rgb) != 0, "короче заголовка отвергнут");
    CHECK(wallpaper_parse(0, 100, &w, &h, &rgb) != 0, "пустой указатель отвергнут");
    uint8_t save = b[0]; b[0] = 'X';
    CHECK(wallpaper_parse(b, n, &w, &h, &rgb) != 0, "чужая подпись отвергнута");
    b[0] = save; b[5] = 0x7F;                            /* ширина > 4096 */
    CHECK(wallpaper_parse(b, n, &w, &h, &rgb) != 0, "огромные размеры отвергнуты");
    b[5] = 0;

    /* тот же размер: картинка почти не меняется; другой размер: края и монотонность */
    uint32_t* px = calloc(64 * 36, 4);
    wallpaper_scale(px, 64, 36, b + 12, 64, 36);
    CHECK((px[0] >> 16 & 255) <= 2 && (px[63] >> 16 & 255) >= 253, "тот же размер: красный по краям");
    CHECK((px[0] & 255) == 77 && px[0] >> 24 == 0, "синий постоянен, старший байт чист");
    uint32_t* big = calloc(200 * 200, 4);                 /* квадрат из 16:9: боковые края обрезаются */
    wallpaper_scale(big, 200, 200, b + 12, 64, 36);
    int r0 = big[100 * 200] >> 16 & 255, r1 = big[100 * 200 + 199] >> 16 & 255;
    CHECK(r0 > 50 && r0 < 100 && r1 > 150 && r1 < 210, "обрезка по бокам: краёв картинки нет");
    for (int x = 1; x < 200; x++) if ((big[100 * 200 + x] >> 16 & 255) + 1 < (big[100 * 200 + x - 1] >> 16 & 255)) { CHECK(0, "красный не убывает"); break; }
    int g0 = big[0] >> 8 & 255, g1 = big[199 * 200] >> 8 & 255;
    CHECK(g0 <= 3 && g1 >= 252, "по высоте картинка целиком");
    wallpaper_scale(0, 10, 10, b + 12, 64, 36); wallpaper_scale(px, 0, 0, b + 12, 64, 36);   /* не падает */
    free(b); free(px); free(big);
    printf(fails ? "WALLPAPER FAILED\n" : "wallpaper: parse, fill-crop scaling passed\n");
    return fails != 0;
}
