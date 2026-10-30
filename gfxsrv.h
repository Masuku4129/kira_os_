#ifndef GFXSRV_H
#define GFXSRV_H
#include <stdint.h>

/* Графический сервер для программ Ring 3. Программа просит поверхность (gfx_create), рисует в неё
   напрямую (память общая с ядром), говорит gfx_present, а читает события мыши/клавиатуры через
   gfx_event. Окном и композицией занимается поток дисплея: вызовы из процесса только ставят
   запросы в очередь, поэтому состояние оконного менеджера меняет один поток. */

#define GFX_USER_VA 0x0000008080000000ULL      /* окно 4 ГБ: поверхность любого экрана */
#define GFX_USER_END 0x0000008180000000ULL
#define GFX_MIN_W 64
#define GFX_MIN_H 48
/* Наибольший размер поверхности - размер экрана (на любом мониторе программа может занять его целиком). */

typedef struct { uint32_t type, a, b, c; } gfx_event_t;   /* 16 байт */
enum { GEV_KEY_DOWN = 1, GEV_KEY_UP = 2, GEV_MOUSE_MOVE = 3, GEV_MOUSE_BUTTON = 4, GEV_CLOSE = 5 };

/* Вызовы из процесса (контекст системного вызова). */
int64_t gfx_sys_create(uint32_t pid, const char* title, uint32_t w, uint32_t h);   /* адрес пикселей в процессе или -1 */
int64_t gfx_sys_present(uint32_t pid);
int64_t gfx_sys_event(uint32_t pid, uint64_t user_ptr);                            /* 1 - событие, 0 - нет, -1 - ошибка */
void    gfx_proc_gone(uint32_t pid);                                               /* процесс завершён (IF может быть 0) */

/* Поток дисплея. */
void gfx_server_run(void);
void gfx_on_input(int win, int type, int x, int y, unsigned buttons);
void gfx_on_close(int win);

int  gfx_focus_pid(void);   /* pid приложения, чьё окно сверху, иначе 0 */

#endif
