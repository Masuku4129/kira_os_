#ifndef GFXUI_H
#define GFXUI_H
#include "gfx2d.h"

/* Примитивы современного интерфейса: сглаженные скруглённые прямоугольники, круги, кольца,
   дуги, прозрачность. Только целочисленная арифметика (в ядре нет FPU вне 3D).
   Покрытие пикселя считается в 1/16 пикселя по расстоянию от центра пикселя до границы фигуры.
   Все функции обрезают рисование по границам поверхности. alpha: 0..255. */

void ui_blend(surface_t* s, int x, int y, uint32_t color, unsigned alpha);
void ui_round_rect(surface_t* s, int x, int y, int w, int h, int r, uint32_t color, unsigned alpha);
void ui_round_frame(surface_t* s, int x, int y, int w, int h, int r, uint32_t color, unsigned alpha);  /* контур 1 px */
void ui_disc(surface_t* s, int cx, int cy, int r, uint32_t color, unsigned alpha);                      /* (cx,cy) - центр, r - радиус в пикселях */
void ui_ring(surface_t* s, int cx, int cy, int r_in, int r_out, uint32_t color, unsigned alpha);
/* Дуга кольца: угол в десятых долях градуса, 0 - вверх, по часовой стрелке; sweep 0..3600. */
void ui_arc(surface_t* s, int cx, int cy, int r_in, int r_out, int start10, int sweep10, uint32_t color);
int  ui_angle10(int dx, int dy);                          /* угол точки (dx,dy; y вниз) в 0..3599 */
void ui_text_bold(surface_t* s, int x, int y, const char* utf8, uint32_t color);   /* жирнее на 1 px */
int  ui_text_scaled(surface_t* s, int x, int y, const char* utf8, int scale, uint32_t color);  /* крупный текст, возвращает ширину */
uint32_t ui_mix(uint32_t a, uint32_t b, unsigned t256);   /* a*(256-t)+b*t */
/* Отрезок со сглаживанием: концы и полутолщина в 1/16 пикселя. */
void ui_seg(surface_t* s, int x0_16, int y0_16, int x1_16, int y1_16, int half_w16, uint32_t color, unsigned alpha);
/* Сглаженный крупный текст: scale16 = 16 - как 8x16, 24 - в полтора раза, 32 - вдвое. Возвращает ширину. */
int  ui_text_smooth(surface_t* s, int x, int y, const char* utf8, int scale16, uint32_t color);
int  ui_text_smooth_width(const char* utf8, int scale16);
void ui_vgrad(surface_t* s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void ui_glow(surface_t* s, int x, int y, int w, int h, int r, int size, uint32_t color, unsigned alpha);


/* ---- единая тема приложений: карточки, кнопки, полоса состояния, подсказка клавиш ---- */
typedef struct { const char* key; const char* label; uint32_t color; } ui_key_t;   /* ячейка подсказки: клавиша и действие */
void ui_text_fit(surface_t* s, int x, int y, const char* utf8, int maxw, uint32_t color);   /* не шире maxw, иначе «…» */
void ui_card(surface_t* s, int x, int y, int w, int h, const char* title, uint32_t accent);  /* title может быть 0 */
void ui_button(surface_t* s, int x, int y, int w, int h, const char* label, uint32_t color, int hover, int on);
/* Полоса состояния внизу окна на всю ширину: слева итог/подсказка, справа сведения. */
void ui_statusbar(surface_t* s, int y, const char* left, uint32_t left_color, const char* right);
/* Полоса подсказки клавиш как в mc: n одинаковых ячеек на всю ширину; hover - номер ячейки под мышью или -1. */
void ui_keybar(surface_t* s, int y, const ui_key_t* k, int n, int hover);
int  ui_keybar_hit(int width, int n, int x);              /* номер ячейки по x или -1 */

#endif
