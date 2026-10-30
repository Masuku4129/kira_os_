#ifndef APPS_H
#define APPS_H
#include <stdint.h>
#include "gfx2d.h"
#include "mind_sys.h"
#include "explorer.h"

/* Приложения оболочки, которые рисует само ядро (поток дисплея): Разум, Монитор, Проводник, Настройки.
   Только рисование и разбор щелчков по готовым снимкам данных: ни дисков, ни замков, ни ожиданий. */

#define APP_MIND_W 600
#define APP_MIND_H 540
#define APP_MON_W  600
#define APP_MON_H  470
#define APP_FILES_W 600
#define APP_FILES_H 470
#define APP_SET_W  520
#define APP_SET_H  410

void app_mind_draw(surface_t* s, const mind_view_t* v);
void app_monitor_draw(surface_t* s, const mind_view_t* v, uint32_t threads);

/* Проводник. hit: >=0 - строка дерева; APP_FILES_* - кнопки. */
enum { APP_FILES_NONE = -1, APP_FILES_REFRESH = -2, APP_FILES_UP = -3, APP_FILES_DOWN = -4, APP_FILES_RUN = -5, APP_FILES_PIN = -6 };
/* status - итог последнего запуска (строка внизу просмотра) или 0; pinned - программа уже в быстром доступе. */
void app_files_draw(surface_t* s, const exp_view_t* e, int hover, int scroll, const char* status, int pinned);
const char* app_files_program(const exp_view_t* e);   /* выбран файл прямо в /bin: имя программы, иначе 0 */
int  app_files_hit(const exp_view_t* e, int scroll, int x, int y);
int  app_files_visible_rows(void);

/* Настройки. Коды элементов: 10+режим (0 наблюдает, 1 действует), 20+анимация (0..3, 4 - авто),
   30 - пояс минус, 31 - пояс плюс, 40+телеметрия (0/1). */
typedef struct {
    int mode, fx_user, fx, tz_min, telemetry, experience, apic, cores, rtc_local;
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
