#ifndef EXPLORER_H
#define EXPLORER_H
#include <stdint.h>

/* Проводник: дерево KiraFS и просмотр файла. Диск трогает только свой рабочий поток «explorer» (он может ждать
   диск); оболочка лишь отправляет запросы и забирает готовый снимок, не ожидая. */

#define EXP_ROWS     2048    /* строк в дереве (прокручивается) */
#define EXP_DIR_MAX  1024    /* имён одного каталога в дереве (дальше - отметка усечения) */
#define EXP_NAME     60
#define EXP_PREVIEW  1536
#define EXP_VOLS     64

typedef struct {
    uint8_t  depth, dir, open, sel;
    uint64_t size;
    char     name[EXP_NAME];
} exp_row_t;

typedef struct {
    uint32_t version;
    int32_t  mounted;                 /* 0 - диска с KiraFS нет */
    uint32_t nrows, truncated;
    exp_row_t row[EXP_ROWS];
    uint32_t total_kb, free_kb, files;
    char     sel_path[128];           /* выбранный файл ("" - нет) */
    uint64_t sel_size;
    int32_t  prev_len;                /* байт в prev; -1 - ошибка чтения */
    uint8_t  prev_bin;                /* в начале файла есть двоичные данные */
    char     prev[EXP_PREVIEW];
    /* подключённые тома (флешки): каталоги /usbN в корне */
    uint32_t nvol;
    struct { char name[8]; char label[20]; uint32_t total_kb; } vol[EXP_VOLS];
    /* быстрый доступ: содержимое /var/dock, прочитанное при запуске потока */
    uint8_t  dock_loaded;             /* 1 - чтение было (файла может не быть: dock_len = -1) */
    int32_t  dock_len;
    char     dock_text[256];
} exp_view_t;

void     explorer_init(void);                       /* после файловой системы; запускает поток */
void     explorer_click(uint32_t version, int row); /* каталог: раскрыть/свернуть; файл: показать */
void     explorer_refresh(void);
void     explorer_expand(const char* path);         /* раскрыть каталог (если ещё не раскрыт) */
/* Записать файл (целиком, атомарно) рабочим потоком: 0 - принято в очередь. Данные копируются. */
int      explorer_write(const char* path, const void* data, unsigned len);
uint32_t explorer_version(void);
void     explorer_view(exp_view_t* out);

/* Чистая часть (проверяется на хосте): построение дерева через функцию обхода каталога. */
typedef int (*exp_list_fn)(const char* path, void (*emit)(void* ctx, const char* name, int dir, uint64_t size), void* ctx);
uint32_t exp_build(exp_list_fn list, char open[][128], int nopen, exp_row_t* rows, char (*paths)[128], uint32_t max, uint32_t* truncated);

#endif
