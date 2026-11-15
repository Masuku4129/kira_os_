#ifndef KIRA_GFXCTL_H
#define KIRA_GFXCTL_H
/* Управляющий слой графического сервера на портах: порт «kira.gfx», сообщения CBOR v1 (ksvc.h). Клиент сначала шлёт hello с личным портом.
   Нужно право CAP_GFX. Пиксели и показ кадра остаются в gfx_create/gfx_present (поверхность - общая память, копировать нечего);
   по сообщениям идут вопросы и события:
     info                  -> res {"w","h"} размер экрана
     win                   -> res {"w","h","focus"} окно процесса; code -2, если окна нет
     events {"max"?}       -> res [[тип,a,b,c], ...] до max (по умолчанию 8, не больше 16) событий окна; code = их число */
void gfxctl_init(void);
void gfxctl_poll(void);
#endif
