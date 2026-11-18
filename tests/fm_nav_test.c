// Проводник: сортировка по колонкам, путь и хлебные крошки, история назад/вперёд, выделение (Ctrl+A), буфер обмена (Ctrl+C/X/V), контекстное меню, команды.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm.h"
#include "input.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

#define NF 1024
typedef struct { char path[FM_PATH]; int dir; uint8_t* data; uint64_t size; int used; } mf_t;
static mf_t FS[NF];
static int ticks, cancel_after, reads, logs; static char lastlog[256];

static mf_t* find(const char* p) { for (int i = 0; i < NF; i++) if (FS[i].used && !strcmp(FS[i].path, p)) return &FS[i]; return 0; }
static mf_t* newf(const char* p) { for (int i = 0; i < NF; i++) if (!FS[i].used) { memset(&FS[i], 0, sizeof FS[i]); FS[i].used = 1; snprintf(FS[i].path, FM_PATH, "%s", p); return &FS[i]; } return 0; }
static void addf(const char* p, const char* text) { mf_t* m = newf(p); m->size = strlen(text); m->data = malloc(m->size + 1); memcpy(m->data, text, m->size); }
static void addd(const char* p) { newf(p)->dir = 1; }
static int m_list(const char* path, void (*emit)(void*, const char*, int, uint64_t), void* ctx) {
    mf_t* d = strcmp(path, "/") ? find(path) : 0;
    if (strcmp(path, "/") && (!d || !d->dir)) return -2;
    size_t pl = strlen(path); if (pl == 1) pl = 0;
    for (int i = 0; i < NF; i++) {
        if (!FS[i].used) continue;
        const char* p = FS[i].path;
        if (strncmp(p, path, pl) || p[pl] != '/' || strchr(p + pl + 1, '/')) continue;
        emit(ctx, p + pl + 1, FS[i].dir, FS[i].size);
    }
    return 0;
}
static int m_stat(const char* p, int* dir, uint64_t* size) { if (!strcmp(p, "/")) { *dir = 1; *size = 0; return 0; } mf_t* m = find(p); if (!m) return -2; *dir = m->dir; *size = m->size; return 0; }
static int64_t m_read(const char* p, uint64_t off, void* b, size_t n) { mf_t* m = find(p); if (!m || m->dir) return -2; reads++; if (off > m->size) return -1; if (off + n > m->size) n = m->size - off; memcpy(b, m->data + off, n); return (int64_t)n; }
static int m_write(const char* p, const void* d, size_t n) { mf_t* m = find(p); if (!m) m = newf(p); if (!m) return -6; free(m->data); m->data = malloc(n + 1); memcpy(m->data, d, n); m->size = n; return 0; }
static int m_mkdir(const char* p) { if (find(p)) return -3; addd(p); return 0; }
static int m_unlink(const char* p) {
    mf_t* m = find(p); if (!m) return -2;
    if (m->dir) { size_t l = strlen(p); for (int i = 0; i < NF; i++) if (FS[i].used && !strncmp(FS[i].path, p, l) && FS[i].path[l] == '/') return -8; }
    free(m->data); m->used = 0; return 0;
}
static void* m_alloc(size_t n) { return malloc(n); }
static void m_release(void* p) { free(p); }
static int m_tick(fm_t* f) { ticks++; (void)f; return cancel_after && ticks >= cancel_after; }
static const char* m_err(int e) { return e == -3 ? "уже есть" : e == -2 ? "нет такого" : e == -8 ? "каталог не пуст" : "сбой"; }
static void m_log(const char* l) { logs++; snprintf(lastlog, sizeof lastlog, "%s", l); }
static const fm_io_t IO = { m_list, m_stat, m_read, m_write, m_mkdir, m_unlink, m_alloc, m_release, m_tick, m_err, m_log };

static fm_t* F;
static void key(int k) { fm_key(F, &IO, k, 0); }
static void typ(const char* s) { for (; *s; s++) fm_key(F, &IO, (unsigned char)*s, (unsigned char)*s); }
static int has(int p, const char* name) { for (uint32_t i = 0; i < F->p[p].n; i++) if (!strcmp(F->p[p].e[i].name, name)) return 1; return 0; }
static int at(int p, const char* name) { for (uint32_t i = 0; i < F->p[p].n; i++) if (!strcmp(F->p[p].e[i].name, name)) return (int)i; return -1; }
static void cur_to(int p, const char* name) { int i = at(p, name); CHECK(i >= 0); F->active = p; F->p[p].cur = i; }

static void keym(int k, int mods) { fm_key_m(F, &IO, k, 0, mods); }
static int idx_of(int p, const char* name) { return at(p, name); }

int main(void) {
    /* путь */
    char b[FM_PATH];
    CHECK(fm_path_depth("/") == 0 && fm_path_depth("/a") == 1 && fm_path_depth("/a/b/c") == 3 && fm_path_depth("//a//b/") == 2);
    CHECK(fm_path_prefix("/a/b/c", 0, b, sizeof b) == 0 && !strcmp(b, "/"));
    CHECK(fm_path_prefix("/a/b/c", 2, b, sizeof b) == 0 && !strcmp(b, "/a/b"));
    CHECK(fm_path_prefix("/a/b/c", 3, b, sizeof b) == 0 && !strcmp(b, "/a/b/c"));
    CHECK(fm_path_prefix("/a/b/c", 4, b, sizeof b) != 0 && fm_path_prefix("a/b", 1, b, sizeof b) != 0);
    CHECK(fm_path_prefix("/a/b/c", 3, b, 5) != 0);                        /* мал буфер - отказ, а не обрезка */

    /* сортировка чистой функцией */
    fm_ent_t e[5]; memset(e, 0, sizeof e);
    strcpy(e[0].name, "b.txt"); e[0].size = 30; e[0].kind = FM_KIND_TEXT;
    strcpy(e[1].name, "A.png"); e[1].size = 10; e[1].kind = FM_KIND_IMG;
    strcpy(e[2].name, "zdir"); e[2].dir = 1; e[2].kind = FM_KIND_DIR;
    strcpy(e[3].name, "c.bin"); e[3].size = 20; e[3].kind = FM_KIND_FILE;
    strcpy(e[4].name, "adir"); e[4].dir = 1; e[4].kind = FM_KIND_DIR;
    fm_sort(e, 5, FM_SORT_NAME, 0);
    CHECK(!strcmp(e[0].name, "adir") && !strcmp(e[1].name, "zdir") && !strcmp(e[2].name, "A.png") && !strcmp(e[3].name, "b.txt") && !strcmp(e[4].name, "c.bin"));
    fm_sort(e, 5, FM_SORT_SIZE, 0);
    CHECK(e[0].dir && e[1].dir && !strcmp(e[2].name, "A.png") && !strcmp(e[3].name, "c.bin") && !strcmp(e[4].name, "b.txt"));
    fm_sort(e, 5, FM_SORT_SIZE, 1);
    CHECK(e[0].dir && e[1].dir && !strcmp(e[2].name, "b.txt") && !strcmp(e[4].name, "A.png"));   /* каталоги всё равно первыми */
    fm_sort(e, 5, FM_SORT_NAME, 1);
    CHECK(!strcmp(e[0].name, "zdir") && !strcmp(e[2].name, "c.bin") && !strcmp(e[4].name, "A.png"));
    fm_sort(e, 5, FM_SORT_TYPE, 0);
    CHECK(e[0].dir && e[1].dir && e[2].kind <= e[3].kind && e[3].kind <= e[4].kind);

    /* панели */
    F = calloc(1, sizeof *F);
    addd("/bin"); addd("/data"); addd("/dst"); addf("/data/b.txt", "BBBBBB"); addf("/data/a.txt", "A"); addf("/data/c.txt", "CCC"); addd("/data/sub"); addf("/data/sub/deep.txt", "deep");
    fm_init(F, &IO, "/data", "/dst");
    /* сортировка щелчком по шапке: тот же столбец меняет направление */
    fm_mouse(F, &IO, FM_MOUSE_SORT, 0, FM_SORT_SIZE);
    CHECK(F->p[0].sort_col == FM_SORT_SIZE && !F->p[0].sort_desc);
    CHECK(!strcmp(F->p[0].e[F->p[0].n - 1].name, "b.txt") && !strcmp(F->p[0].e[2].name, "a.txt"));       /* [0] - "..", [1] - каталог sub */
    fm_mouse(F, &IO, FM_MOUSE_SORT, 0, FM_SORT_SIZE);
    CHECK(F->p[0].sort_desc && !strcmp(F->p[0].e[F->p[0].n - 1].name, "a.txt"));
    CHECK(F->p[1].sort_col == FM_SORT_NAME);                                                            /* панели сортируются независимо */
    fm_mouse(F, &IO, FM_MOUSE_SORT, 0, FM_SORT_NAME);
    CHECK(!F->p[0].sort_desc && !strcmp(F->p[0].e[2].name, "a.txt"));

    /* история: вход в sub, назад в /data, вперёд в sub; новый переход обрезает «вперёд» */
    F->active = 0;
    cur_to(0, "sub"); key(KEY_ENTER);
    CHECK(!strcmp(F->p[0].path, "/data/sub") && F->p[0].nback == 1);
    keym(KEY_LEFT, 4);
    CHECK(!strcmp(F->p[0].path, "/data") && F->p[0].nfwd == 1 && F->p[0].nback == 0);
    CHECK(!strcmp(F->p[0].e[F->p[0].cur].name, "sub"));                                                 /* курсор на каталоге, из которого вышли */
    keym(KEY_RIGHT, 4);
    CHECK(!strcmp(F->p[0].path, "/data/sub") && F->p[0].nback == 1 && F->p[0].nfwd == 0);
    keym(KEY_UP, 4);                                                                                    /* Alt+↑ - вверх */
    CHECK(!strcmp(F->p[0].path, "/data"));
    keym(KEY_RIGHT, 4);
    CHECK(!strcmp(F->p[0].path, "/data") && F->msg_err);                                                /* вперёд некуда - сообщение, путь прежний */
    fm_cmd(F, &IO, FM_CMD_BACK);
    CHECK(!strcmp(F->p[0].path, "/data/sub"));                                                          /* Alt+↑ тоже записан в историю */
    fm_cmd(F, &IO, FM_CMD_BACK);
    CHECK(!strcmp(F->p[0].path, "/data"));
    for (int i = 0; i < 12; i++) { fm_goto(F, &IO, 0, i & 1 ? "/data" : "/bin"); }
    CHECK(F->p[0].nback <= FM_HIST);

    /* выделение Ctrl+A: все, кроме «..» */
    fm_goto(F, &IO, 0, "/data"); F->active = 0;
    keym('a', 2);
    CHECK(F->p[0].nmark == 4 && F->p[0].mark_bytes == 10);                                              /* sub, a, b, c; размер файлов 1+6+3 */
    CHECK(!F->p[0].e[0].mark || F->p[0].e[0].kind != FM_KIND_UP);
    key(KEY_ESC);
    CHECK(F->p[0].nmark == 0);

    /* буфер обмена: Ctrl+C из /data, Ctrl+V в /dst; исходный остаётся; вставка в тот же каталог - отказ */
    cur_to(0, "a.txt"); keym('c', 2);
    CHECK(F->nclip == 1 && !F->clip_cut && !strcmp(F->clip_dir, "/data"));
    keym('v', 2);
    CHECK(F->msg_err);                                                                                  /* в тот же каталог */
    key(KEY_TAB); keym('v', 2);
    CHECK(find("/dst/a.txt") && find("/data/a.txt") && F->nclip == 1 && !F->msg_err);
    /* вырезать: перенос, буфер сбрасывается */
    key(KEY_TAB); cur_to(0, "c.txt"); keym('x', 2);
    CHECK(F->nclip == 1 && F->clip_cut);
    key(KEY_TAB); keym('v', 2);
    CHECK(find("/dst/c.txt") && !find("/data/c.txt") && F->nclip == 0);
    keym('v', 2);
    CHECK(F->msg_err);                                                                                  /* буфер пуст */
    /* исчезнувшее из буфера пропускается честно */
    key(KEY_TAB); cur_to(0, "b.txt"); keym('c', 2); m_unlink("/data/b.txt"); fm_reload(F, &IO);
    key(KEY_TAB); keym('v', 2);
    CHECK(F->msg_err && !find("/dst/b.txt"));

    /* контекстное меню: Shift+F10, навигация, недоступные пункты, выполнение */
    fm_goto(F, &IO, 0, "/data"); F->active = 0; cur_to(0, "a.txt");
    int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
    keym('c', 2);                                                                                       /* в буфере a.txt */
    int n = fm_menu_build(F, id, lb, &off);
    CHECK(n >= 15 && n <= FM_MENU_MAX && id[0] == FM_CMD_OPEN && lb[2][0] == 0);
    int paste_i = -1; for (int i = 0; i < n; i++) if (id[i] == FM_CMD_PASTE) paste_i = i;
    CHECK(paste_i >= 0 && !((off >> paste_i) & 1));                                                     /* буфер не пуст - Вставить доступно */
    F->nclip = 0;
    n = fm_menu_build(F, id, lb, &off);
    CHECK((off >> paste_i) & 1);                                                                        /* пустой буфер - Вставить недоступно */
    keym(KEY_F10, 1);
    CHECK(F->mode == FM_MODE_MENU && F->menu_x == -1 && F->menu_sel == 0);
    key(KEY_DOWN); key(KEY_DOWN);                                                                       /* пропускает разделитель */
    CHECK(id[F->menu_sel] == FM_CMD_COPY);
    key(KEY_ESC);
    CHECK(F->mode == FM_MODE_NORMAL);
    fm_mouse_at(F, &IO, FM_MOUSE_RCLICK, 0, idx_of(0, "sub"), 100, 50);                                 /* правая кнопка выбирает строку */
    CHECK(F->mode == FM_MODE_MENU && F->menu_x == 100 && F->menu_y == 50 && !strcmp(F->p[0].e[F->p[0].cur].name, "sub"));
    fm_mouse(F, &IO, FM_MOUSE_CLICK, 0, 0);                                                             /* щелчок мимо - закрыть, без действия */
    CHECK(F->mode == FM_MODE_NORMAL && !strcmp(F->p[0].path, "/data"));
    fm_mouse_at(F, &IO, FM_MOUSE_RCLICK, 0, idx_of(0, "sub"), 5, 5);
    int del_i = -1; n = fm_menu_build(F, id, lb, &off); for (int i = 0; i < n; i++) if (id[i] == FM_CMD_DELETE) del_i = i;
    fm_mouse(F, &IO, FM_MOUSE_MENUPICK, 0, del_i);                                                      /* «Удалить» из меню - с подтверждением */
    CHECK(F->mode == FM_MODE_CONFIRM && find("/data/sub"));
    key(KEY_ESC);
    CHECK(F->mode == FM_MODE_NORMAL && find("/data/sub/deep.txt"));
    fm_mouse_at(F, &IO, FM_MOUSE_RCLICK, 0, idx_of(0, "sub"), 5, 5);
    fm_mouse(F, &IO, FM_MOUSE_MENUPICK, 0, del_i); key(KEY_ENTER);
    CHECK(!find("/data/sub") && !find("/data/sub/deep.txt"));
    /* на «..» копирование, вырезание, удаление и переименование недоступны */
    fm_goto(F, &IO, 0, "/data/.."); F->active = 0;
    fm_goto(F, &IO, 0, "/dst"); F->p[0].cur = 0;
    n = fm_menu_build(F, id, lb, &off);
    for (int i = 0; i < n; i++) if (id[i] == FM_CMD_COPY || id[i] == FM_CMD_CUT || id[i] == FM_CMD_DELETE || id[i] == FM_CMD_RENAME) CHECK((off >> i) & 1);

    /* команды панели инструментов: новая папка, переименование, обновить */
    fm_mouse(F, &IO, FM_MOUSE_CMD, -1, FM_CMD_MKDIR);
    CHECK(F->mode == FM_MODE_INPUT);
    typ("newdir"); key(KEY_ENTER);
    CHECK(find("/dst/newdir") && F->mode == FM_MODE_NORMAL);
    cur_to(0, "newdir"); fm_mouse(F, &IO, FM_MOUSE_CMD, -1, FM_CMD_RENAME);
    CHECK(F->mode == FM_MODE_INPUT && !strcmp(F->input, "newdir"));
    for (int i = 0; i < 6; i++) key(KEY_BACKSPACE);
    typ("renamed"); key(KEY_ENTER);
    CHECK(find("/dst/renamed") && !find("/dst/newdir"));
    fm_mouse(F, &IO, FM_MOUSE_CMD, -1, FM_CMD_REFRESH);
    CHECK(!F->msg_err && has(0, "renamed"));
    printf("fm_nav: path, sort columns, history, select all, clipboard, context menu, toolbar commands passed\n");
    return 0;
}
