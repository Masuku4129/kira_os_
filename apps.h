#ifndef APPS_H
#define APPS_H
#include <stdint.h>
#include "gfx2d.h"
#include "mind_sys.h"
#include "explorer.h"

/* Приложения оболочки, которые рисует само ядро (поток дисплея): Разум, Монитор, Проводник, Настройки.
   Только рисование и разбор щелчков по готовым снимкам данных: ни дисков, ни замков, ни ожиданий. */

#include "theme.h"
/* Размеры содержимого окон. Внизу каждого окна - полоса состояния TH_BAR_H (у Проводника ещё и подсказка клавиш). */
#define APP_MIND_W 600
#define APP_MIND_H (540 + TH_BAR_H)
#define APP_MON_W  600
#define APP_MON_H  (470 + TH_BAR_H)
#define APP_FILES_W 800
#define APP_FILES_H 520
#define APP_SET_W  520
#define APP_SET_H  (410 + TH_BAR_H)

void app_mind_draw(surface_t* s, const mind_view_t* v);
void app_monitor_draw(surface_t* s, const mind_view_t* v, uint32_t threads);

/* Проводник: две панели. Что под мышью: строка панели, пустое место панели, полоса прокрутки (row -1/0/1 - выше/на/ниже
   бегунка), путь, кнопки шапки, ячейка подсказки клавиш F1..F9 (row 0..8). */
enum { APP_HIT_NONE, APP_HIT_ROW, APP_HIT_PANEL, APP_HIT_SCROLL, APP_HIT_REFRESH, APP_HIT_HIDDEN, APP_HIT_KEY };
typedef struct { int kind, panel, row; } app_hit_t;
/* hover - что под мышью (или 0); drop_panel - над какой панелью тащат файл (или -1); status - итог запуска программы
   (строка состояния, если нет сообщения менеджера) или 0; pinned - программа под курсором уже в быстром доступе. */
void app_files_draw(surface_t* s, const exp_view_t* e, const app_hit_t* hover, int drop_panel, const char* status, int pinned);
int  app_files_hit(const exp_view_t* e, int x, int y, app_hit_t* h);   /* возвращает h->kind */

/* Настройки. Коды элементов: 10+режим (0 наблюдает, 1 действует), 20+анимация (0..3, 4 - авто),
   30 - пояс минус, 31 - пояс плюс, 40+телеметрия (0/1). */
typedef struct {
    int mode, fx_user, fx, tz_min, telemetry, experience, cores, rtc_local;
    const char* irqc;     /* контроллер прерываний (platform_irq_name) */
    uint32_t ram_mb;
    int scr_w, scr_h;
} app_settings_t;
void app_settings_draw(surface_t* s, const app_settings_t* st, int hover);
int  app_settings_hit(int x, int y);

/* Общие помощники форматирования (без libc). */
int  app_fmt_u(char* out, uint64_t v);
void app_fmt_hms(char* out, uint32_t s);
void app_fmt_kb(char* out, uint64_t kb);

#endif
