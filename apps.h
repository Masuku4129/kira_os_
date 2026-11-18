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
#define APP_MIND_H (600 + TH_BAR_H)
#define APP_MON_W  600
#define APP_MON_H  (470 + TH_BAR_H)
#define APP_FILES_W 800
#define APP_FILES_H 520
#define APP_SET_W  520
#define APP_SET_H  (474 + TH_BAR_H)

void app_mind_draw(surface_t* s, const mind_view_t* v, unsigned page);
/* Карточный кэш окна «Разум»: перерисовываются только карточки с изменившимися значениями; бюджет делит работу на кадры. */
#define APP_MIND_DREW 1u   /* поверхность изменилась */
#define APP_MIND_MORE 2u   /* бюджет вышел, остались неотрисованные карточки: позвать ещё раз в следующем кадре */
unsigned app_mind_draw_step(surface_t* s, const mind_view_t* v, unsigned page, uint64_t budget_ns, uint64_t (*now_ns)(void));
void app_mind_invalidate(void);
int  app_mind_hit(int x, int y);
void app_monitor_draw(surface_t* s, const mind_view_t* v, uint32_t threads);

/* Проводник: две панели. Что под мышью: строка панели, пустое место панели, полоса прокрутки (row -1/0/1 - выше/на/ниже
   бегунка), путь, кнопки шапки, ячейка подсказки клавиш F1..F9 (row 0..8). */
enum { APP_HIT_NONE, APP_HIT_ROW, APP_HIT_PANEL, APP_HIT_SCROLL, APP_HIT_REFRESH, APP_HIT_HIDDEN, APP_HIT_KEY,
       APP_HIT_TOOL,      /* кнопка панели инструментов: row - FM_CMD_* */
       APP_HIT_HEAD,      /* шапка колонки: row - FM_SORT_* */
       APP_HIT_CRUMB,     /* часть строки пути: row - номер части (0 - корень) */
       APP_HIT_SIDE,      /* пункт боковой панели: row - номер (путь - app_files_side_path) */
       APP_HIT_MENU };    /* пункт контекстного меню: row - номер */
#define APP_MENU_W 272
typedef struct { int kind, panel, row; } app_hit_t;
/* hover - что под мышью (или 0); drop_panel - над какой панелью тащат файл (или -1); status - итог запуска программы
   (строка состояния, если нет сообщения менеджера) или 0; pinned - программа под курсором уже в быстром доступе. */
void app_files_draw(surface_t* s, const exp_view_t* e, const app_hit_t* hover, int drop_panel, const char* status, int pinned);
int  app_files_hit(const exp_view_t* e, int x, int y, app_hit_t* h);   /* возвращает h->kind */
int  app_files_side_path(const exp_view_t* e, int row, char* out, int cap);          /* путь пункта боковой панели; 0 - есть */
int  app_files_crumb_path(const exp_view_t* e, int panel, int k, char* out, int cap);/* путь до k-й части строки пути панели */
void app_files_menu_pos(const exp_view_t* e, int* x, int* y);                        /* где нарисовано контекстное меню (общая геометрия с попаданием мыши) */

/* Настройки. Коды элементов: 10+режим (0 наблюдает, 1 действует), 20+анимация (0..3, 4 - авто),
   30 - пояс минус, 31 - пояс плюс, 40+телеметрия (0/1), 50+уведомления Разума (0 выкл, 1 важные, 2 все). */
typedef struct {
    int mode, fx_user, fx, tz_min, telemetry, experience, cores, rtc_local, notify;
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
