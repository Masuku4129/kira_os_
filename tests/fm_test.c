// Двухпанельный менеджер: сортировка, служебные файлы, навигация, отметки, копирование и перенос (файлы и деревья),
// каталог, переименование, удаление с подтверждением, отмена, порции, ошибки, перенос мышью.
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

int main(void) {
    F = calloc(1, sizeof *F);
    addd("/bin"); addd("/var"); addd("/data"); addd("/Alpha"); addf("/zeta.txt", "zz"); addf("/beta.png", "PNGDATA"); addf("/.hidden", "h");
    addf("/bin/plasma", "x"); addf("/data/a.txt", "AAAA"); addf("/data/b.txt", "BBBBBB"); addd("/data/sub"); addf("/data/sub/deep.txt", "deep");
    fm_init(F, &IO, "/", "/data");
    CHECK(!strcmp(F->p[1].path, "/data"));
    /* служебное скрыто, порядок: каталоги (без учёта регистра), затем файлы */
    CHECK(!has(0, "bin") && !has(0, "var") && !has(0, ".hidden") && F->p[0].hidden == 3);
    CHECK(F->p[0].n == 4 && !strcmp(F->p[0].e[0].name, "Alpha") && !strcmp(F->p[0].e[1].name, "data"));
    CHECK(!strcmp(F->p[0].e[2].name, "beta.png") && F->p[0].e[2].kind == FM_KIND_IMG && !strcmp(F->p[0].e[3].name, "zeta.txt") && F->p[0].e[3].kind == FM_KIND_TEXT);
    CHECK(!strcmp(F->p[1].e[0].name, "..") && F->p[1].e[0].kind == FM_KIND_UP && !strcmp(F->p[1].e[1].name, "sub"));
    key(KEY_F9); CHECK(F->show_hidden && has(0, "bin") && has(0, ".hidden")); key(KEY_F9); CHECK(!has(0, "bin"));
    /* навигация: Tab, Enter, Backspace возвращает курсор на каталог */
    CHECK(F->active == 0); key(KEY_TAB); CHECK(F->active == 1); key(KEY_TAB);
    cur_to(0, "data"); key(KEY_ENTER); CHECK(!strcmp(F->p[0].path, "/data") && F->p[0].cur == 0);
    cur_to(0, "sub"); key(KEY_ENTER); CHECK(!strcmp(F->p[0].path, "/data/sub"));
    key(KEY_BACKSPACE); CHECK(!strcmp(F->p[0].path, "/data") && !strcmp(F->p[0].e[F->p[0].cur].name, "sub"));
    key(KEY_BACKSPACE); CHECK(!strcmp(F->p[0].path, "/") && !strcmp(F->p[0].e[F->p[0].cur].name, "data"));
    CHECK(fm_goto(F, &IO, 0, "/nope") < 0 && !strcmp(F->p[0].path, "/"));              /* неудачный переход: путь прежний */
    /* файл: Enter - просьба открыть, F3 - просмотр, F4 - только программы /bin */
    uint32_t seq0 = F->act_seq; cur_to(0, "zeta.txt"); key(KEY_ENTER);
    CHECK(F->act_seq == seq0 + 1 && F->act_kind == FM_ACT_OPEN && !strcmp(F->act_path, "/zeta.txt"));
    key(KEY_F3); CHECK(F->act_kind == FM_ACT_VIEW && F->act_seq == seq0 + 2);
    key(KEY_F4); CHECK(F->act_seq == seq0 + 2 && F->msg_err);
    fm_goto(F, &IO, 0, "/bin"); cur_to(0, "plasma"); CHECK(F->p[0].e[F->p[0].cur].kind == FM_KIND_PROG); key(KEY_F4);
    CHECK(F->act_kind == FM_ACT_PIN && !strcmp(F->act_path, "/bin/plasma"));
    /* копирование F5: курсор слева, цель справа (/data) */
    fm_goto(F, &IO, 0, "/"); fm_goto(F, &IO, 1, "/data"); F->active = 0;
    cur_to(0, "zeta.txt"); key(KEY_F5);
    CHECK(find("/data/zeta.txt") && find("/data/zeta.txt")->size == 2 && find("/zeta.txt") && has(1, "zeta.txt") && !F->msg_err);
    CHECK(strstr(lastlog, "copy /zeta.txt -> /data/zeta.txt: 2 байт"));
    key(KEY_F5); CHECK(F->msg_err && find("/data/zeta.txt")->size == 2);               /* имя занято: ошибка, файл цел */
    /* отметки: Insert отмечает и двигает вниз, повтор снимает, отметки переживают обновление, Esc снимает все */
    cur_to(0, "beta.png"); key(KEY_INSERT); CHECK(F->p[0].nmark == 1 && F->p[0].mark_bytes == 7 && F->p[0].e[at(0, "beta.png")].mark);
    key(KEY_INSERT); CHECK(F->p[0].nmark == 2);
    cur_to(0, "zeta.txt"); key(KEY_INSERT); CHECK(F->p[0].nmark == 1);
    fm_reload(F, &IO); CHECK(F->p[0].nmark == 1 && F->p[0].e[at(0, "beta.png")].mark);
    key(KEY_ESC); CHECK(F->p[0].nmark == 0 && !F->p[0].e[at(0, "beta.png")].mark);
    cur_to(0, "beta.png"); key(KEY_INSERT); key(KEY_F6);
    CHECK(find("/data/beta.png") && !find("/beta.png") && !has(0, "beta.png") && has(1, "beta.png") && F->p[0].nmark == 0);
    CHECK(strstr(lastlog, "move /beta.png -> /data/beta.png"));
    /* каталог целиком (дерево) в чужой каталог; в самого себя нельзя */
    fm_goto(F, &IO, 0, "/"); fm_goto(F, &IO, 1, "/Alpha"); cur_to(0, "data"); key(KEY_F5);
    CHECK(find("/Alpha/data/sub/deep.txt") && find("/Alpha/data/a.txt") && !F->msg_err);
    fm_goto(F, &IO, 1, "/data"); cur_to(0, "data"); key(KEY_F5); CHECK(F->msg_err && !find("/data/data/data"));
    /* F7: новый каталог, курсор на нём; плохие имена отвергаются */
    fm_goto(F, &IO, 0, "/"); F->active = 0; key(KEY_F7); CHECK(F->mode == FM_MODE_INPUT);
    typ("new"); key(KEY_BACKSPACE); typ("w"); CHECK(!strcmp(F->input, "new")); key(KEY_ENTER);
    CHECK(F->mode == FM_MODE_NORMAL && find("/new") && find("/new")->dir && !strcmp(F->p[0].e[F->p[0].cur].name, "new"));
    key(KEY_F7); typ("a/b"); CHECK(!strcmp(F->input, "ab")); key(KEY_ESC); CHECK(F->mode == FM_MODE_NORMAL && !find("/ab"));
    key(KEY_F7); key(KEY_ENTER); CHECK(F->msg_err);                                       /* пустое имя */
    key(KEY_F7); fm_key(F, &IO, 0, 0x43F); fm_key(F, &IO, 0, 0x440); CHECK(!strcmp(F->input, "пр")); key(KEY_BACKSPACE); CHECK(!strcmp(F->input, "п")); key(KEY_ESC);
    /* F2: переименование файла и каталога с содержимым */
    cur_to(0, "zeta.txt"); key(KEY_F2); CHECK(F->mode == FM_MODE_INPUT && !strcmp(F->input, "zeta.txt"));
    for (int i = 0; i < 3; i++) { key(KEY_BACKSPACE); }
    typ("doc"); key(KEY_ENTER);
    CHECK(find("/zeta.doc") && !find("/zeta.txt") && !strcmp(F->p[0].e[F->p[0].cur].name, "zeta.doc"));
    cur_to(0, "Alpha"); key(KEY_F2); for (int i = 0; i < 5; i++) key(KEY_BACKSPACE); typ("Omega"); key(KEY_ENTER);
    CHECK(find("/Omega/data/sub/deep.txt") && !find("/Alpha") && !find("/Alpha/data"));
    cur_to(0, "Omega"); key(KEY_F2); for (int i = 0; i < 5; i++) key(KEY_BACKSPACE); typ("data"); key(KEY_ENTER); CHECK(F->msg_err && find("/Omega"));   /* имя занято */
    /* F8: подтверждение; Esc отменяет; Enter удаляет рекурсивно */
    cur_to(0, "Omega"); key(KEY_F8); CHECK(F->mode == FM_MODE_CONFIRM && strstr(F->dlg_text, "Omega")); key(KEY_ESC);
    CHECK(F->mode == FM_MODE_NORMAL && find("/Omega/data/a.txt"));
    key(KEY_F8); key(KEY_ENTER); CHECK(!find("/Omega") && !find("/Omega/data/sub/deep.txt") && !has(0, "Omega") && !F->msg_err);
    CHECK(strstr(lastlog, "delete /Omega"));
    /* ".." не удаляется */
    fm_goto(F, &IO, 0, "/data"); F->p[0].cur = 0; key(KEY_F8); CHECK(F->mode == FM_MODE_NORMAL && F->msg_err);
    /* большой файл: порциями, ход виден; отмена на середине */
    {
        size_t big = FM_CHUNK * 5 + 123; mf_t* m = newf("/data/big.bin"); m->size = big; m->data = malloc(big);
        for (size_t i = 0; i < big; i++) m->data[i] = (uint8_t)(i * 7);
        fm_goto(F, &IO, 0, "/data"); fm_goto(F, &IO, 1, "/new"); cur_to(0, "big.bin"); ticks = reads = 0;
        key(KEY_F5); CHECK(reads == 6 && ticks >= 6 && find("/new/big.bin") && !memcmp(find("/new/big.bin")->data, m->data, big));
        CHECK(F->prog_total == big && F->prog_done == big);
        m_unlink("/new/big.bin"); fm_reload(F, &IO); cur_to(0, "big.bin"); ticks = 0; cancel_after = 3;
        key(KEY_F5); cancel_after = 0; CHECK(!find("/new/big.bin") && F->msg_err && strstr(F->msg, "Esc"));
    }
    /* перенос мышью на другую панель - копия; двойной щелчок входит в каталог */
    fm_goto(F, &IO, 0, "/data"); fm_goto(F, &IO, 1, "/new"); fm_reload(F, &IO);
    fm_mouse(F, &IO, FM_MOUSE_CLICK, 0, at(0, "a.txt")); CHECK(F->active == 0 && F->p[0].cur == at(0, "a.txt"));
    fm_mouse(F, &IO, FM_MOUSE_DROP, 0, at(0, "b.txt")); CHECK(find("/new/b.txt") && find("/data/b.txt") && !find("/new/a.txt"));
    fm_mouse(F, &IO, FM_MOUSE_DOUBLE, 0, at(0, "sub")); CHECK(!strcmp(F->p[0].path, "/data/sub"));
    fm_mouse(F, &IO, FM_MOUSE_CLICK, 1, 0); CHECK(F->active == 1);
    /* прокрутка */
    for (int i = 0; i < 60; i++) { char p[40]; snprintf(p, sizeof p, "/new/f%03d", i); addf(p, "x"); }
    fm_goto(F, &IO, 1, "/new"); F->active = 1; key(KEY_END); CHECK(F->p[1].cur == (int)F->p[1].n - 1 && F->p[1].top == (int)F->p[1].n - FM_ROWS);
    key(KEY_HOME); CHECK(F->p[1].cur == 0 && F->p[1].top == 0); key(KEY_PGDN); CHECK(F->p[1].cur == FM_ROWS - 1);
    fm_page(F, 1, 1); CHECK(F->p[1].top == FM_ROWS - 1 && F->p[1].cur >= F->p[1].top);
    /* каталог шире предела панели: усечение отмечено, а не потеряно молча */
    addd("/huge"); for (int i = 0; i < 400; i++) { char p[40]; snprintf(p, sizeof p, "/huge/h%03d", i); addf(p, "x"); }
    fm_goto(F, &IO, 0, "/huge"); CHECK(F->p[0].n == 401 && !F->p[0].trunc);
    for (int i = 400; i < 700; i++) { char p[40]; snprintf(p, sizeof p, "/huge/h%03d", i); addf(p, "x"); }
    fm_goto(F, &IO, 0, "/huge"); CHECK(F->p[0].n == 701 && !F->p[0].trunc);
    /* каталог из более чем FM_TREE объектов не копируется молча частично */
    fm_goto(F, &IO, 0, "/"); fm_goto(F, &IO, 1, "/new"); cur_to(0, "huge"); key(KEY_F5); CHECK(F->msg_err && !find("/new/huge/h000"));
    printf("fm: sorting, hidden, navigation, copy/move/mkdir/rename/delete, trees, cancel, chunks, mouse passed\n");
    return 0;
}
