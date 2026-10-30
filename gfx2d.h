#ifndef GFX2D_H
#define GFX2D_H

#include <stdint.h>

// Поверхность - прямоугольник 32-битных пикселей 0x00RRGGBB в памяти. Рисование идёт
// в поверхность, а не прямо в видеопамять: так окна, спрайты и буфер кадра - одно
// и то же, а чтение/запись видеопамяти (медленной, write-combining) не нужны.
typedef struct {
    uint32_t* px;
    int w, h;
    int pitch;          // шаг строки в ПИКСЕЛЯХ (не в байтах)
} surface_t;

static inline uint32_t gfx_rgb(unsigned r, unsigned g, unsigned b) {
    return ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
}

// Все функции обрезают рисование по границам поверхности.
void gfx_fill(surface_t* s, uint32_t color);
void gfx_pixel(surface_t* s, int x, int y, uint32_t color);
void gfx_rect(surface_t* s, int x, int y, int w, int h, uint32_t color);        // заливка
void gfx_frame(surface_t* s, int x, int y, int w, int h, uint32_t color);       // контур 1 px
void gfx_hline(surface_t* s, int x, int y, int w, uint32_t color);
void gfx_line(surface_t* s, int x0, int y0, int x1, int y1, uint32_t color);    // Брезенхем

// Копирование прямоугольника src -> dst (без наложения).
void gfx_blit(surface_t* dst, int dx, int dy, const surface_t* src, int sx, int sy, int w, int h);
// Наложение с альфой: пиксель источника ARGB (старший байт - непрозрачность 0..255).
void gfx_blit_alpha(surface_t* dst, int dx, int dy, const surface_t* src, int sx, int sy, int w, int h);

// Один символ 8x16 по коду Unicode. bg < 0 - прозрачный фон (иначе цвет 0x00RRGGBB).
void gfx_glyph(surface_t* s, int x, int y, uint32_t codepoint, uint32_t fg, int64_t bg);
// Строка UTF-8; '\n' переводит строку. Возвращает ширину самой длинной строки в пикселях.
int gfx_text(surface_t* s, int x, int y, const char* utf8, uint32_t fg, int64_t bg);
int gfx_text_width(const char* utf8);

// Декодер UTF-8: читает один символ, сдвигает *p. Некорректные байты дают U+FFFD.
uint32_t utf8_next(const char** p);

// Быстрое копирование/заливка выровненных по 8 байт блоков (rep movsq/stosq).
void gfx_copy64(void* dst, const void* src, uint64_t qwords);
void gfx_fill64(void* dst, uint64_t pattern, uint64_t qwords);

#endif
