#ifndef FBCON_H
#define FBCON_H

#include "gfx2d.h"

#define FBCON_FG 0x00C8D4E4u   /* обычный текст */
#define FBCON_BG 0x00070D19u   /* фон терминала (совпадает с полем окон) */

// Текстовая консоль на экране: показывает журнал ядра (то же, что идёт в UART).
// Сетка символов хранится отдельно от пикселей, поэтому экран можно перерисовать целиком
// после того, как поверх консоли побывали окна. Символ \f (0x0C) в журнале очищает экран.
/* До fbcon_init: сетка знаков под наибольший размер окна (экран), память - от alloc. */
void fbcon_reserve(int max_w_px, int max_h_px, void* (*alloc)(uint64_t bytes));
void fbcon_init(surface_t* screen);
// Выводит накопленное в журнале в поверхность. Возвращает число обработанных байт;
// *dirty_y0/*dirty_y1 - изменившиеся строки пикселей [y0, y1) (если байтов не было - не трогаются).
unsigned fbcon_update(int* dirty_y0, int* dirty_y1);
void fbcon_resize(void);                   // поверхность изменила размер: пересчитать сетку и перерисовать
void fbcon_repaint(void);                  // полная перерисовка из сетки символов
int fbcon_text_height(void);               // высота занятой текстом области в пикселях
void fbcon_get_size(int* cols, int* rows);

#endif
