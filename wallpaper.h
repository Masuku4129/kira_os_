/* Обои рабочего стола из картинки. Формат файла KWP1: "KWP1", ширина и высота (u32, little-endian), затем RGB по 3 байта
   на точку, строки сверху вниз. Конвертер любой картинки в KWP1 - tools/mkwallpaper.py. Масштабирование - целочисленное
   (ядро собрано без SSE), "заполнить с обрезкой": картинка накрывает экран целиком, лишнее по краям отбрасывается. */
#ifndef KIRA_WALLPAPER_H
#define KIRA_WALLPAPER_H
#include <stdint.h>
#include <stddef.h>

#define WALLPAPER_MAX_DIM 4096

/* Проверяет заголовок и размер. 0 - файл пригоден: *w, *h - размеры, *rgb - начало точек внутри buf. */
int wallpaper_parse(const uint8_t* buf, size_t len, int* w, int* h, const uint8_t** rgb);

/* Рисует картинку на поверхность w x h (px - 0x00RRGGBB, строка w точек). */
void wallpaper_scale(uint32_t* px, int w, int h, const uint8_t* rgb, int iw, int ih);
#endif
