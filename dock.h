#ifndef DOCK_H
#define DOCK_H
#include <stdint.h>

/* Быстрый доступ (левая панель): программы из /bin, которые пользователь закрепил сам. Список хранится в /var/dock -
   по имени в строке. Чистый модуль: разбор, запись, добавление, удаление, значок и цвет по имени. */
#define DOCK_MAX  10
#define DOCK_NAME 16
#define DOCK_PATH "/var/dock"

typedef struct { int n; char name[DOCK_MAX][DOCK_NAME]; } dock_t;

void     dock_default(dock_t* d);                               /* plasma, paint, cube3d */
int      dock_valid_name(const char* s);                        /* как у программ: a-z 0-9 - _ */
int      dock_parse(dock_t* d, const char* text, unsigned len); /* число принятых имён; мусор и повторы пропускаются */
unsigned dock_format(const dock_t* d, char* out, unsigned cap); /* текст для файла */
int      dock_find(const dock_t* d, const char* name);          /* индекс или -1 */
int      dock_add(dock_t* d, const char* name);                 /* 0 - добавлено, 1 - уже было, -1 - нельзя */
int      dock_remove(dock_t* d, const char* name);              /* 0 - убрано, -1 - не было */
int      dock_icon(const char* name);                           /* ICON_* по имени */
const char* dock_title(const char* name);                       /* понятное русское название программы или 0 (тогда показывается имя) */

/* Единая таблица программ: системное имя -> русское название, описание, значок, цвет. Пользователь видит имена в доке
   (подсказки), в заголовках окон, в списке /bin Проводника и в таблице программ Монитора - везде через эти функции.
   Терминал и консоль остаются с системными именами. Неизвестная программа: общий значок (ICON_TERMINAL) и своё имя. */
typedef struct { const char* sys; const char* title; const char* desc; int icon; uint32_t color; } app_info_t;
const app_info_t* app_info(const char* name);                   /* строка таблицы или 0 */
const char* app_label(const char* name);                        /* русское название; для неизвестной - само имя */
const char* app_desc(const char* name);                         /* описание или "" */
int      app_info_count(void);                                  /* строк в таблице (для проверок) */
const app_info_t* app_info_at(int i);
uint32_t dock_color(const char* name);                          /* неоновый цвет по имени */

#endif
