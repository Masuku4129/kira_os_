#ifndef WM_H
#define WM_H
#include <stdint.h>
#include "gfx2d.h"

/* Оболочка рабочего стола «ОС Кира» и композитор.
   - верхняя панель: логотип, разделы (вкладки), дата и время, индикаторы ядер, памяти и здоровья;
   - левая боковая панель с неоновыми значками: запуск приложений, возврат свёрнутых;
   - окна: тёмные панели со «светофором» слева (закрыть, свернуть, на весь экран), заголовок по центру,
     неоновая подсветка окна в фокусе;
   - закреплённые панели (виджеты), полноэкранный режим без рамок;
   - фон: ночное небо с сеткой и «нейросеть» в форме мозга, узлы которой вспыхивают (brainfx).
   Модуль не знает про видеопамять: рисует в переданную поверхность и сообщает, какой прямоугольник изменился. */

#define WM_MAX_WIN   272     /* окно у каждой программы (до 256) и окна ядра */
#define WM_TOP_H     40      /* верхняя панель */
#define WM_PAD       10      /* поля окна вокруг содержимого */
#define WM_HEAD      36      /* заголовок окна */
#define WM_SIDE_W    76      /* место под боковую панель слева */
#define WM_SPATIAL_RAIL_W 88  /* ширина компактной навигации Spatial Minimal */
#define WM_GLOW      4       /* свечение окна в фокусе (за пределами рамки) */
#define WM_AUTO      (-100000) /* координата "разместить самому" */
#define WM_SIDE_MAX  10
#define WM_TAB_MAX   8
#define WM_DOCK_MAX  WM_SIDE_MAX

typedef enum {
    WM_LAYOUT_LEGACY = 0,
    WM_LAYOUT_SPATIAL_MINIMAL = 1
} wm_layout_mode_t;

#define WM_CLOSABLE  1       /* окно можно закрыть (иначе красная кнопка сворачивает) */
#define WM_PINNED    2       /* виджет: не двигается, не поднимается, без кнопок */
#define WM_RESIZABLE 4       /* размер меняет владелец содержимого (консоль, панели ядра): вызывается ops.resize */
#define WM_ZOOMABLE  8       /* содержимое растягивается/сжимается (окна приложений и 3D) */
#define WM_INPUT     16      /* окно ядра получает мышь (ops.input), как окна приложений */

/* Значок боковой панели. bottom - прижат к низу; temp - не закреплён (идущая программа): рисуется после черты;
   hint - вторая строка подсказки (например «правая кнопка - открепить»); badge - буква в углу значка
   (у программ с одинаковым значком). */
typedef struct { const char* label; const char* tag; uint32_t color; int icon; int bottom; int temp; const char* hint; char badge; } wm_side_item_t;
typedef wm_side_item_t wm_dock_item_t;
typedef struct { const char* label; const char* tag; } wm_tab_t;

typedef struct {
    void* (*alloc)(uint64_t bytes);               /* память под содержимое окна и фон (обнулять не нужно) */
    void  (*notify)(int id, int visible);         /* 0 - свёрнуто, 1 - показано, -1 - запрошено закрытие окна приложения */
    void  (*input)(int id, int type, int x, int y, unsigned buttons); /* мышь над окном: 3 - движение, 4 - кнопки; x,y - в содержимом */
    void  (*launch)(int side_index);              /* значок боковой панели нажат, а окна у него нет */
    void  (*resize)(int id, int w, int h);        /* у окна с WM_RESIZABLE изменился размер содержимого */
    void  (*tab)(int tab_index);                  /* нажат раздел в верхней панели */
    void  (*side_alt)(int side_index);            /* правая кнопка на значке боковой панели */
    void  (*launcher)(void);                     /* открыть каталог приложений (только Spatial Minimal) */
    void  (*workspace_changed)(int workspace);  /* новое рабочее пространство стало активным */
} wm_ops_t;

void wm_init(surface_t* screen, const wm_ops_t* ops); /* legacy-compatible entry point */
void wm_init_mode(surface_t* screen, const wm_ops_t* ops, wm_layout_mode_t mode);
void wm_set_layout_mode(wm_layout_mode_t mode);
wm_layout_mode_t wm_get_layout_mode(void);
/* Spatial Minimal: временный обзор всех неприкреплённых окон, включая свёрнутые. */
void wm_window_overview_toggle(void);
int wm_window_overview_is_open(void);
int wm_window_overview_count(void);
int wm_window_overview_item_center(int item, int* x, int* y);
int wm_system_panel_is_open(void);
#define WM_WORKSPACE_MAX 4
int wm_workspace_current(void);
int wm_workspace_count(void);
int wm_workspace_switch(int workspace);
int wm_workspace_center(int workspace, int* x, int* y);
/* Bounded, non-modal notifications. Posting never changes focus or window ownership. */
enum { WM_NOTIFY_INFO = 0, WM_NOTIFY_WARNING = 1, WM_NOTIFY_CRITICAL = 2 };
int wm_notification_post(const char* title, const char* message, int priority);
int wm_notification_count(void);
int wm_notification_unread_count(void);
int wm_notification_center_is_open(void);
int wm_notification_toast_is_visible(void);
void wm_notification_center_toggle(void);
void wm_notification_clear(void);
/* Spatial Minimal: centers of active previous/next Dock page controls for hit-test integration. */
int wm_dock_page_control_center(int next, int* x, int* y);
int  wm_create(const char* title, int x, int y, int content_w, int content_h, int flags); /* id или -1 */
/* Как wm_create, но память под содержимое выделяется на cap_w x cap_h: размер можно менять до этого предела. */
int  wm_create_ex(const char* title, int x, int y, int content_w, int content_h, int flags, int cap_w, int cap_h);
void wm_set_resize_limits(int id, int min_w, int min_h, int step_w, int step_h);
void wm_set_content_size(int id, int w, int h);    /* владелец подправляет размер содержимого */
int  wm_zoom_percent(int id);
void wm_content_size(int id, int* w, int* h);
/* Окно приложения: содержимое лежит в чужой памяти mem (0x00RRGGBB, шаг = content_w);
   кнопка закрытия не скрывает окно, а шлёт notify(id,-1). */
int  wm_create_user(const char* title, int x, int y, int content_w, int content_h, uint32_t* mem);
void wm_set_frame_seq(int id, const volatile uint32_t* seq);
void wm_destroy(int id);
surface_t* wm_content(int id);
void wm_set_title(int id, const char* title);
void wm_set_subtitle(int id, const char* text);    /* справа в заголовке, приглушённо */
void wm_set_tag(int id, const char* tag);          /* по метке окно связывается со значком и разделом */
int  wm_find(const char* tag);
void wm_side_set(const wm_side_item_t* items, int n);
/* Метки окон программ Ring 3 (видимых и свёрнутых), не больше max; для списка «идущие» в боковой панели. */
int  wm_user_tags(char (*out)[16], int max);
#define wm_dock_set wm_side_set
void wm_tabs_set(const wm_tab_t* tabs, int n);
void wm_set_visible(int id, int visible);          /* 0 - свернуть "на фон" (приложение продолжает работать) */
int  wm_visible(int id);
void wm_minimize(int id);
void wm_minimize_all(void);                         /* показать рабочий стол (раздел «Главная») */
void wm_set_fullscreen(int id, int on);
int  wm_fullscreen(int id);
void wm_raise(int id);
int  wm_top(void);                                 /* верхнее видимое не закреплённое окно или -1 */
void wm_invalidate(int id);
void wm_invalidate_rows(int id, int y0, int y1);
void wm_invalidate_all(void);
void wm_set_culling(int on);
/* Верхняя панель: дата («ПЯТНИЦА, 30 ОКТЯБРЯ 2026») и время; ядра (занятость, %), память (%), здоровье Разума. */
void wm_set_clock(const char* date, const char* hm);
void wm_set_meters(const uint8_t* busy, int ncores, int mem_pct, int health);
/* Фон: уровень анимации 0..3 и шаг анимации (каждый цикл дисплея). */
void wm_set_fx(int level);
void wm_anim(uint32_t now_ms, unsigned activity);
void wm_mouse(int dx, int dy, unsigned buttons);   /* относительное движение, бит0 = левая кнопка */
void wm_cursor(int* x, int* y);
int  wm_window_pos(int id, int* x, int* y);        /* левый верхний угол панели */
int  wm_content_origin(int id, int* x, int* y, int* scale);
/* Геометрия для проверок и подсказок: центр значка боковой панели, кнопки окна (1 закрыть, 2 свернуть,
   3 на весь экран), раздела верхней панели. 0 - есть. */
int  wm_side_center(int i, int* x, int* y);
int  wm_button_center(int id, int which, int* x, int* y);
int  wm_tab_center(int i, int* x, int* y);
/* Перерисовывает накопленный damage в экран. 1 - нарисовано, *x,*y,*w,*h - что показать. */
int  wm_compose(int* x, int* y, int* w, int* h);
/* Параллельная сборка: изменённые места экрана (плитки 64x64) режутся на полосы, полосы собираются на нескольких
   ядрах (через runner) и каждая сразу выводится present. Вернёт число полос; *pixels - сколько точек собрано. */
typedef void (*wm_runner_t)(void (*fn)(void* ctx, uint32_t i), void* ctx, uint32_t n);
void wm_set_runner(wm_runner_t r);
uint32_t wm_compose_par(void (*present)(int x, int y, int w, int h), uint64_t* pixels);

#endif
