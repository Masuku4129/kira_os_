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
uint32_t dock_color(const char* name);                          /* неоновый цвет по имени */

#endif
