#ifndef EXPLORER_H
#define EXPLORER_H
#include <stdint.h>
#include "fm.h"

/* Проводник: две панели (логика - fm.c). Диск трогает только рабочий поток «explorer» (он может ждать диск и
   копировать большие файлы порциями); оболочка лишь кладёт в очередь клавиши и щелчки и забирает готовый снимок,
   не ожидая. */

#define EXP_VOLS 64

typedef struct {
    uint32_t version;
    fm_t     fm;                      /* панели, курсоры, диалоги, ход операции */
    int32_t  mounted;                 /* 0 - диска с KiraFS нет */
    uint32_t total_kb, free_kb, files;
    /* подключённые тома (флешки): каталоги /usbN в корне */
    uint32_t nvol, rovol;
    struct { char name[8]; char label[20]; uint32_t total_kb; uint8_t read_only; } vol[EXP_VOLS];
    /* быстрый доступ: содержимое /var/dock, прочитанное при запуске потока */
    uint8_t  dock_loaded;             /* 1 - чтение было (файла может не быть: dock_len = -1) */
    int32_t  dock_len;
    char     dock_text[256];
} exp_view_t;

void     explorer_init(void);                       /* после файловой системы; запускает поток */
void     explorer_key(int key, int ch);             /* клавиша: KEY_* из input.h и символ */
void     explorer_mouse(int kind, int panel, int row);   /* FM_MOUSE_*: строка row панели panel */
void     explorer_page(int panel, int dir);         /* страница вверх (-1) / вниз (1) по щелчку на полосе прокрутки */
void     explorer_goto(int panel, const char* path);/* панель panel (-1 - активная) открывает каталог */
void     explorer_refresh(void);
/* Записать файл (целиком, атомарно) рабочим потоком: 0 - принято в очередь. Данные копируются. */
int      explorer_write(const char* path, const void* data, unsigned len);
uint32_t explorer_version(void);
void     explorer_view(exp_view_t* out);

#endif
