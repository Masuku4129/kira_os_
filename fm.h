#ifndef FM_H
#define FM_H
#include <stdint.h>
#include <stddef.h>

/* Двухпанельный файловый менеджер (логика): две панели со своими путями, курсор, отметки, F2/F5-F9, перенос мышью.
   Чистая часть без ядра: файловую систему и память получает через fm_io_t, поэтому проверяется на хосте.
   Состояние fm_t одновременно и есть то, что рисует оболочка (снимок целиком копируется потоку дисплея). */

#define FM_ENT    1024       /* объектов каталога в панели (дальше - отметка усечения) */
#define FM_NAME   60
#define FM_PATH   128
#define FM_MARKS  64         /* отметок в панели */
#define FM_ROWS   18         /* строк списка на экране */
#define FM_TREE   512        /* объектов одного каталога при копировании и удалении дерева */
#define FM_HIST   8          /* глубина истории переходов (назад/вперёд) на панель */
#define FM_CHUNK  65536      /* порция чтения при копировании */

enum { FM_KIND_FILE, FM_KIND_DIR, FM_KIND_UP, FM_KIND_PROG, FM_KIND_IMG, FM_KIND_TEXT, FM_KIND_WASM };

typedef struct { char name[FM_NAME]; uint64_t size; uint8_t dir, mark, kind, pad; } fm_ent_t;

typedef struct {
    char     path[FM_PATH];
    uint32_t n, trunc, hidden;                 /* объектов в списке, список усечён, скрыто служебных */
    int32_t  cur, top;
    uint32_t nmark; uint64_t mark_bytes;       /* отмечено объектов и их размер (файлы) */
    char     mk[FM_MARKS][FM_NAME];            /* имена отмеченных: переживают обновление списка */
    int32_t  sort_col, sort_desc;              /* сортировка: FM_SORT_*, 1 - по убыванию (каталоги всегда первыми) */
    uint32_t nback, nfwd;                      /* история переходов: назад (последний - ближайший) и вперёд */
    char     back[FM_HIST][FM_PATH], fwd[FM_HIST][FM_PATH];
    fm_ent_t e[FM_ENT];                        /* e[0] - ".." (если не корень), затем каталоги, затем файлы */
} fm_panel_t;

enum { FM_MODE_NORMAL, FM_MODE_INPUT, FM_MODE_CONFIRM, FM_MODE_BUSY, FM_MODE_HELP, FM_MODE_MENU };
enum { FM_SORT_NAME, FM_SORT_SIZE, FM_SORT_TYPE, FM_SORT_COLS };
enum { FM_OP_NONE, FM_OP_MKDIR, FM_OP_RENAME, FM_OP_DELETE, FM_OP_COPY, FM_OP_MOVE };
enum { FM_ACT_NONE, FM_ACT_OPEN, FM_ACT_VIEW, FM_ACT_PIN };      /* просьба к оболочке: открыть файл, просмотр, закрепить */
enum { FM_MOUSE_CLICK, FM_MOUSE_DOUBLE, FM_MOUSE_DROP,           /* DROP: файл отпущен над другой панелью */
       FM_MOUSE_RCLICK,                                         /* правая кнопка: выбор строки и контекстное меню */
       FM_MOUSE_SORT,                                           /* щелчок по шапке колонки: row - FM_SORT_* */
       FM_MOUSE_CMD,                                            /* кнопка панели инструментов: row - FM_CMD_* */
       FM_MOUSE_MENUPICK };                                     /* пункт контекстного меню: row - номер пункта */
/* Команды: общие для клавиш, панели инструментов и контекстного меню. */
enum { FM_CMD_NONE, FM_CMD_BACK, FM_CMD_FWD, FM_CMD_UP, FM_CMD_REFRESH, FM_CMD_MKDIR, FM_CMD_COPY, FM_CMD_CUT, FM_CMD_PASTE,
       FM_CMD_DELETE, FM_CMD_RENAME, FM_CMD_OPEN, FM_CMD_VIEW, FM_CMD_SELALL, FM_CMD_HIDDEN, FM_CMD_PIN,
       FM_CMD_XCOPY, FM_CMD_XMOVE };                            /* в каталог другой панели (F5/F6) */
#define FM_MENU_MAX 20

typedef struct {
    uint32_t   version;
    fm_panel_t p[2];
    int32_t    active, show_hidden;
    int32_t    mode, op;
    char       dlg_title[48], dlg_text[FM_PATH + 16];
    char       input[FM_NAME]; int32_t input_len;
    char       msg[128]; int32_t msg_err;       /* итог последней операции или ошибка: полоса состояния */
    uint64_t   prog_done, prog_total; uint32_t prog_files, prog_files_total;
    char       prog_name[FM_NAME];
    uint32_t   act_seq; int32_t act_kind; char act_path[FM_PATH];
    char       clip_dir[FM_PATH]; char clip[FM_MARKS][FM_NAME]; uint32_t nclip; int32_t clip_cut;   /* буфер обмена: имена в каталоге clip_dir */
    int32_t    menu_sel, menu_x, menu_y;                        /* контекстное меню: пункт, место щелчка (-1 - от курсора) */
} fm_t;

typedef struct {
    int      (*list)(const char* path, void (*emit)(void* ctx, const char* name, int dir, uint64_t size), void* ctx);
    int      (*stat)(const char* path, int* dir, uint64_t* size);        /* 0 - есть */
    int64_t  (*read)(const char* path, uint64_t off, void* buf, size_t len);
    int      (*write)(const char* path, const void* data, size_t len);   /* создаёт или целиком заменяет */
    int      (*mkdir)(const char* path);
    int      (*unlink)(const char* path);                                /* файл или пустой каталог */
    void*    (*alloc)(size_t n);
    void     (*release)(void* p);
    int      (*tick)(fm_t* f);                                           /* между порциями: показать ход; 1 - отмена (Esc) */
    const char* (*errstr)(int err);
    void     (*log)(const char* line);
} fm_io_t;

void fm_init(fm_t* f, const fm_io_t* io, const char* left, const char* right);
void fm_reload(fm_t* f, const fm_io_t* io);                              /* перечитать обе панели (курсор и отметки сохраняются) */
int  fm_goto(fm_t* f, const fm_io_t* io, int panel, const char* path);   /* 0 - перешли; иначе ошибка (путь остаётся прежним) */
void fm_key(fm_t* f, const fm_io_t* io, int key, int ch);                /* key - KEY_* из input.h, ch - символ */
void fm_key_m(fm_t* f, const fm_io_t* io, int key, int ch, int mods);   /* то же с модификаторами INPUT_MOD_* (Ctrl+C/X/V/A, Alt+←/→, Shift+F10) */
void fm_mouse(fm_t* f, const fm_io_t* io, int kind, int panel, int row);
void fm_mouse_at(fm_t* f, const fm_io_t* io, int kind, int panel, int row, int x, int y);   /* x,y - место щелчка (меню) */
void fm_cmd(fm_t* f, const fm_io_t* io, int cmd);                        /* выполнить команду FM_CMD_* над активной панелью */
/* Пункты контекстного меню по состоянию: id - FM_CMD_*, label - подпись ("" - разделитель), off - маска недоступных. Возвращает число. */
int  fm_menu_build(const fm_t* f, int* id, const char** label, unsigned* off);
/* Чистая логика пути и списка (проверяется на хосте). */
int  fm_path_depth(const char* path);                                    /* число частей пути: "/" - 0, "/a/b" - 2 */
int  fm_path_prefix(const char* path, int k, char* out, int cap);        /* путь до k-й части включительно (k=0 - "/"); 0 - успех */
void fm_sort(fm_ent_t* e, uint32_t n, int col, int desc);                /* каталоги первыми, внутри - по колонке */
void fm_page(fm_t* f, int panel, int dir);                               /* щелчок по полосе прокрутки: страница вверх/вниз */
void fm_set_hidden(fm_t* f, const fm_io_t* io, int on);
int  fm_is_system(const char* dir, const char* name);                    /* служебное: скрыто по умолчанию */
int  fm_kind_of(const char* dir, const char* name, int is_dir);

#endif
