#ifndef BRAINFX_H
#define BRAINFX_H
#include <stdint.h>
#include "gfx2d.h"

/* Фон рабочего стола «нейросеть в форме мозга»: построение, отрисовка в буфер фона и живые вспышки узлов.
   Частота вспышек зависит от уровня анимации (решает Разум) и от его активности (решения, необычности). */
#define BRAIN_MAX_NODES 160

int  brainfx_build(int cx, int cy, int half_w, int half_h);          /* узлы и связи; число узлов */
void brainfx_render(surface_t* bg, int cx, int cy, int half_w, int half_h);
/* Шаг анимации: отмечает изменившиеся области через damage. fx 0..3, activity - событий Разума за секунду. */
void brainfx_anim(uint32_t now_ms, int fx, unsigned activity, void (*damage)(int x0, int y0, int x1, int y1));
void brainfx_draw(surface_t* clip, int ox, int oy, uint32_t now_ms);  /* вспышки поверх фона в перерисовываемой области */
int  brainfx_nodes(void);

#endif
