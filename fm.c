#include "fm.h"
#include "input.h"

/* ---------------- строки ---------------- */

enum { E_CANCEL = -1000, E_NOMEM = -1001, E_BIG = -1002, E_INSIDE = -1003, E_EXIST = -1004, E_MANY = -1005, E_DEEP = -1006, E_NAME = -1007 };
#define DEPTH_MAX 8

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scpy(char* d, const char* s, int cap) { int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static int ap(char* d, int p, int cap, const char* s) { while (*s && p + 1 < cap) d[p++] = *s++; d[p] = 0; return p; }
static int apu(char* d, int p, int cap, uint64_t v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    while (n && p + 1 < cap) d[p++] = t[--n];
    d[p] = 0;
    return p;
}
static int apsize(char* d, int p, int cap, uint64_t b) {
    if (b < 1024) { p = apu(d, p, cap, b); return ap(d, p, cap, " Б"); }
    if (b < 1024 * 1024) { p = apu(d, p, cap, b / 1024); return ap(d, p, cap, " КБ"); }
    uint64_t t = b * 10 / (1024 * 1024);
    p = apu(d, p, cap, t / 10);
    if (t < 1000) { p = ap(d, p, cap, "."); p = apu(d, p, cap, t % 10); }
    return ap(d, p, cap, " МБ");
}
static void zero(void* p, size_t n) { unsigned char* b = (unsigned char*)p; for (size_t i = 0; i < n; i++) b[i] = 0; }

static int pjoin(char* out, const char* dir, const char* name) {
    int n = 0;
    while (dir[n] && n < FM_PATH - 1) { out[n] = dir[n]; n++; }
    if (dir[n]) return -1;
    if (n == 0 || out[n - 1] != '/') { if (n >= FM_PATH - 1) return -1; out[n++] = '/'; }
    for (int i = 0; name[i]; i++) { if (n >= FM_PATH - 1) return -1; out[n++] = name[i]; }
    out[n] = 0;
    return 0;
}

static int is_inside(const char* p, const char* dir) {        /* p совпадает с dir или лежит внутри */
    int i = 0; while (dir[i] && dir[i] == p[i]) i++;
    return !dir[i] && (!p[i] || p[i] == '/' || (i == 1 && dir[0] == '/'));
}

/* ---------------- путь ---------------- */

int fm_path_depth(const char* path) {
    int d = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/' && path[i + 1] && path[i + 1] != '/') d++;
    return d;
}

/* Путь до k-й части включительно: "/a/b/c", k=2 -> "/a/b"; k=0 -> "/". Лишние косые игнорируются. */
int fm_path_prefix(const char* path, int k, char* out, int cap) {
    if (path[0] != '/' || k < 0 || k > fm_path_depth(path) || cap < 2) return -1;
    int n = 0, seen = 0;
    out[n++] = '/';
    for (int i = 1; path[i] && seen < k; i++) {
        if (path[i] == '/') continue;
        if (n > 1) { if (n + 1 >= cap) return -1; out[n++] = '/'; }
        while (path[i] && path[i] != '/') { if (n + 1 >= cap) return -1; out[n++] = path[i++]; }
        seen++; i--;
    }
    out[n] = 0;
    return 0;
}

/* ---------------- служебное и тип ---------------- */

int fm_is_system(const char* dir, const char* name) {
    if (name[0] == '.') return 1;
    if (dir[0] == '/' && !dir[1]) return seq(name, "var") || seq(name, "lost+found");   /* /bin - список приложений, виден */
    return 0;
}

static int ext_is(const char* name, const char* e) {
    int n = slen(name), m = slen(e);
    if (n <= m + 1 || name[n - m - 1] != '.') return 0;
    for (int i = 0; i < m; i++) { char c = name[n - m + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != e[i]) return 0; }
    return 1;
}

int fm_kind_of(const char* dir, const char* name, int is_dir) {
    if (is_dir) return FM_KIND_DIR;
    if (seq(dir, "/bin")) return FM_KIND_PROG;
    if (ext_is(name, "wasm")) return FM_KIND_WASM;
    static const char* const img[] = { "png", "bmp", "ppm", "gif", "jpg", "jpeg" };
    static const char* const txt[] = { "txt", "md", "log", "c", "h", "py", "sh", "cfg", "ini", "json", "s", "asm" };
    for (unsigned i = 0; i < sizeof img / sizeof img[0]; i++) if (ext_is(name, img[i])) return FM_KIND_IMG;
    for (unsigned i = 0; i < sizeof txt / sizeof txt[0]; i++) if (ext_is(name, txt[i])) return FM_KIND_TEXT;
    return FM_KIND_FILE;
}

/* ---------------- список панели ---------------- */

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int g_col, g_desc;                                           /* колонка и направление текущей сортировки (рабочий поток один) */

static int name_cmp(const fm_ent_t* a, const fm_ent_t* b) {
    const unsigned char* x = (const unsigned char*)a->name; const unsigned char* y = (const unsigned char*)b->name;
    while (*x && fold(*x) == fold(*y)) { x++; y++; }
    if (*x != *y && fold(*x) != fold(*y)) return fold(*x) - fold(*y);
    x = (const unsigned char*)a->name; y = (const unsigned char*)b->name;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}

/* Каталоги всегда первыми; внутри группы - по колонке (имя, размер, тип), равные - по имени; убывание переворачивает группу целиком. */
static int ent_cmp(const fm_ent_t* a, const fm_ent_t* b) {
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    int c = 0;
    if (g_col == FM_SORT_SIZE) c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
    else if (g_col == FM_SORT_TYPE) c = (int)a->kind - (int)b->kind;
    if (c) return g_desc ? -c : c;
    c = name_cmp(a, b);
    return g_desc && g_col == FM_SORT_NAME ? -c : c;
}

typedef struct { fm_ent_t* e; uint32_t n, cap, over, hidden; int show_hidden; const char* dir; } lst_t;

static void emit(void* c, const char* name, int dir, uint64_t size) {
    lst_t* L = (lst_t*)c;
    if (!name[0] || seq(name, ".") || seq(name, "..")) return;
    if (!L->show_hidden && fm_is_system(L->dir, name)) { L->hidden++; return; }
    if (L->n >= L->cap) { L->over = 1; return; }
    fm_ent_t* e = &L->e[L->n++];
    scpy(e->name, name, FM_NAME);
    e->dir = (uint8_t)(dir != 0); e->size = size; e->mark = 0; e->pad = 0;
    e->kind = (uint8_t)fm_kind_of(L->dir, e->name, e->dir);
}

/* Сортировка вставками по индексам, затем перестановка по циклам: записи по 80 байт почти не двигаются. */
void fm_sort(fm_ent_t* e, uint32_t n, int col, int desc) {
    static uint16_t idx[FM_ENT];                                   /* вызывается только из рабочего потока */
    if (n < 2) return;
    g_col = col; g_desc = desc != 0;
    for (uint32_t i = 0; i < n; i++) idx[i] = (uint16_t)i;
    for (uint32_t i = 1; i < n; i++) {
        uint16_t t = idx[i]; uint32_t j = i;
        while (j > 0 && ent_cmp(&e[idx[j - 1]], &e[t]) > 0) { idx[j] = idx[j - 1]; j--; }
        idx[j] = t;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (idx[i] == i) continue;
        fm_ent_t tmp = e[i]; uint32_t j = i;
        while (idx[j] != i) { uint32_t k = idx[j]; e[j] = e[k]; idx[j] = (uint16_t)j; j = k; }
        e[j] = tmp; idx[j] = (uint16_t)j;
    }
}

static void clamp_view(fm_panel_t* P) {
    if (P->cur >= (int32_t)P->n) P->cur = (int32_t)P->n - 1;
    if (P->cur < 0) P->cur = 0;
    int32_t maxtop = (int32_t)P->n - FM_ROWS; if (maxtop < 0) maxtop = 0;
    if (P->cur < P->top) P->top = P->cur;
    if (P->cur >= P->top + FM_ROWS) P->top = P->cur - FM_ROWS + 1;
    if (P->top > maxtop) P->top = maxtop;
    if (P->top < 0) P->top = 0;
}

static void recount_marks(fm_panel_t* P) {
    P->nmark = 0; P->mark_bytes = 0;
    for (uint32_t i = 0; i < P->n; i++) if (P->e[i].mark) { P->nmark++; if (!P->e[i].dir) P->mark_bytes += P->e[i].size; }
}

/* Читает каталог P->path. Курсор встаёт на keep (если такое имя есть). Ошибка списка - код, панель не тронута. */
static int load_panel(fm_t* f, const fm_io_t* io, fm_panel_t* P, const char* keep) {
    static fm_ent_t buf[FM_ENT];
    int root = P->path[0] == '/' && !P->path[1];
    uint32_t n0 = root ? 0 : 1;
    lst_t L = { buf + n0, 0, FM_ENT - n0, 0, 0, f->show_hidden, P->path };
    int r = io->list(P->path, emit, &L);
    if (r < 0) return r;
    fm_sort(buf + n0, L.n, P->sort_col, P->sort_desc);
    uint32_t n = n0 + L.n;
    if (n0) {
        fm_ent_t* u = &buf[0];
        scpy(u->name, "..", FM_NAME); u->dir = 1; u->size = 0; u->mark = 0; u->kind = FM_KIND_UP; u->pad = 0;
    }
    char old[FM_NAME]; scpy(old, (P->n && P->cur < (int32_t)P->n) ? P->e[P->cur].name : "", FM_NAME);
    char marks[FM_MARKS][FM_NAME]; uint32_t nm = 0;
    for (uint32_t k = 0; k < FM_MARKS && P->mk[k][0]; k++) { scpy(marks[nm], P->mk[k], FM_NAME); nm++; }
    for (uint32_t i = 0; i < n; i++) P->e[i] = buf[i];
    P->n = n; P->trunc = L.over; P->hidden = L.hidden;
    for (uint32_t k = 0; k < FM_MARKS; k++) P->mk[k][0] = 0;
    uint32_t kept = 0;
    for (uint32_t k = 0; k < nm; k++)
        for (uint32_t i = n0; i < n; i++)
            if (seq(P->e[i].name, marks[k])) { P->e[i].mark = 1; scpy(P->mk[kept++], marks[k], FM_NAME); break; }
    recount_marks(P);
    const char* want = (keep && keep[0]) ? keep : old;                 /* курсор идёт за именем, иначе остаётся на месте */
    for (uint32_t i = 0; want[0] && i < n; i++) if (seq(P->e[i].name, want)) { P->cur = (int32_t)i; break; }
    clamp_view(P);
    return 0;
}

static void clear_marks(fm_panel_t* P) {
    for (uint32_t i = 0; i < P->n; i++) P->e[i].mark = 0;
    for (uint32_t k = 0; k < FM_MARKS; k++) P->mk[k][0] = 0;
    P->nmark = 0; P->mark_bytes = 0;
}

static void msg(fm_t* f, int err, const char* a, const char* b, const char* c) {
    int p = ap(f->msg, 0, sizeof f->msg, a);
    if (b) p = ap(f->msg, p, sizeof f->msg, b);
    if (c) ap(f->msg, p, sizeof f->msg, c);
    f->msg_err = err;
}

static const char* errtext(const fm_io_t* io, int e) {
    switch (e) {
    case E_CANCEL: return "прервано (Esc)";
    case E_NOMEM: return "не хватает памяти под файл";
    case E_BIG: return "файл слишком велик";
    case E_INSIDE: return "нельзя копировать каталог в самого себя";
    case E_EXIST: return "такое имя уже есть";
    case E_MANY: return "в каталоге больше 512 объектов - сначала разбейте его";
    case E_DEEP: return "слишком глубокая вложенность";
    case E_NAME: return "недопустимое имя";
    default: return io->errstr ? io->errstr(e) : "ошибка";
    }
}

static int g_nohist;                                               /* 1 - переход по истории: стеки не трогать */

static void hist_push(char (*st)[FM_PATH], uint32_t* n, const char* path) {
    if (*n && seq(st[*n - 1], path)) return;
    if (*n >= FM_HIST) { for (uint32_t i = 1; i < FM_HIST; i++) scpy(st[i - 1], st[i], FM_PATH); (*n)--; }
    scpy(st[(*n)++], path, FM_PATH);
}

static int goto_keep(fm_t* f, const fm_io_t* io, int pi, const char* path, const char* keep) {
    fm_panel_t* P = &f->p[pi];
    char np[FM_PATH]; int n = 0;
    if (path[0] != '/') return -7;
    while (path[n] && n < FM_PATH - 1) { np[n] = path[n]; n++; }
    if (path[n]) return -11;
    while (n > 1 && np[n - 1] == '/') n--;
    np[n] = 0;
    char oldp[FM_PATH]; scpy(oldp, P->path, FM_PATH);
    int same = seq(oldp, np);
    scpy(P->path, np, FM_PATH);
    if (!same) { clear_marks(P); P->top = 0; P->cur = 0; P->n = 0; }
    int r = load_panel(f, io, P, keep);
    if (r < 0 && !same) {                                          /* не вышло - остаёмся, где были */
        scpy(P->path, oldp, FM_PATH);
        load_panel(f, io, P, 0);
    } else if (r >= 0 && !same && !g_nohist) { hist_push(P->back, &P->nback, oldp); P->nfwd = 0; }
    return r;
}

int fm_goto(fm_t* f, const fm_io_t* io, int panel, const char* path) {
    int r = goto_keep(f, io, panel, path, 0);
    f->version++;
    return r;
}

void fm_reload(fm_t* f, const fm_io_t* io) {
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0);
    f->version++;
}

void fm_set_hidden(fm_t* f, const fm_io_t* io, int on) {
    f->show_hidden = on != 0;
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0);
    msg(f, 0, on ? "Служебные файлы показаны" : "Служебные файлы скрыты", 0, 0);
    f->version++;
}

void fm_init(fm_t* f, const fm_io_t* io, const char* left, const char* right) {
    zero(f, sizeof *f);
    scpy(f->p[0].path, "/", FM_PATH); scpy(f->p[1].path, "/", FM_PATH);
    load_panel(f, io, &f->p[0], 0); load_panel(f, io, &f->p[1], 0);
    if (left && left[0]) goto_keep(f, io, 0, left, 0);
    if (right && right[0]) goto_keep(f, io, 1, right, 0);
    for (int i = 0; i < 2; i++) f->p[i].nback = f->p[i].nfwd = 0;       /* начальная установка путей - не переход пользователя */
    f->version++;
}

/* ---------------- операции над деревом ---------------- */

typedef struct {
    fm_t* f; const fm_io_t* io;
    uint64_t total, done; uint32_t files, files_total;
    int cancel;
} op_t;

static int o_tick(op_t* o, const char* name) {
    o->f->prog_done = o->done; o->f->prog_total = o->total;
    o->f->prog_files = o->files; o->f->prog_files_total = o->files_total;
    if (name) scpy(o->f->prog_name, name, FM_NAME);
    if (o->io->tick && o->io->tick(o->f)) o->cancel = 1;
    return o->cancel;
}

static const char* base_of(const char* p) { const char* b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

/* Все объекты каталога (без фильтра служебных). over - не поместилось в FM_TREE. */
static int list_all(const fm_io_t* io, const char* path, fm_ent_t* out, uint32_t* n, int* over) {
    lst_t L = { out, 0, FM_TREE, 0, 0, 1, "" };
    int r = io->list(path, emit, &L);
    *n = L.n; *over = (int)L.over;
    return r;
}

static int scan_item(op_t* o, const char* path, int dir, uint64_t size, int depth) {
    if (!dir) { o->total += size; o->files_total++; return 0; }
    if (depth >= DEPTH_MAX) return E_DEEP;
    fm_ent_t* e = (fm_ent_t*)o->io->alloc(sizeof(fm_ent_t) * FM_TREE);
    if (!e) return E_NOMEM;
    uint32_t n; int over;
    int r = list_all(o->io, path, e, &n, &over);
    if (r >= 0 && over) r = E_MANY;
    for (uint32_t i = 0; r >= 0 && i < n; i++) {
        char c[FM_PATH];
        if (pjoin(c, path, e[i].name) < 0) { r = E_NAME; break; }
        r = scan_item(o, c, e[i].dir, e[i].size, depth + 1);
    }
    o->io->release(e);
    return r;
}

static int copy_file(op_t* o, const char* src, const char* dst, uint64_t size) {
    const fm_io_t* io = o->io;
    uint8_t* buf = (uint8_t*)io->alloc(size ? (size_t)size : 1);
    if (!buf || (uint64_t)(size_t)size != size) { if (buf) io->release(buf); return E_NOMEM; }
    uint64_t off = 0;
    o_tick(o, base_of(src));
    while (off < size) {
        size_t n = (size - off) > FM_CHUNK ? FM_CHUNK : (size_t)(size - off);
        int64_t got = io->read(src, off, buf + off, n);
        if (got != (int64_t)n) { io->release(buf); return got < 0 ? (int)got : -1; }
        off += n; o->done += n;
        if (o_tick(o, base_of(src))) { io->release(buf); return E_CANCEL; }
    }
    int r = io->write(dst, buf, (size_t)size);
    io->release(buf);
    if (r < 0) return r;
    o->files++;
    o_tick(o, base_of(src));
    return 0;
}

static int copy_item(op_t* o, const char* src, const char* dst, int dir, uint64_t size, int depth) {
    const fm_io_t* io = o->io;
    int d2; uint64_t s2;
    if (io->stat(dst, &d2, &s2) == 0) return E_EXIST;
    if (!dir) return copy_file(o, src, dst, size);
    if (depth >= DEPTH_MAX) return E_DEEP;
    if (is_inside(dst, src)) return E_INSIDE;
    int r = io->mkdir(dst);
    if (r < 0) return r;
    fm_ent_t* e = (fm_ent_t*)io->alloc(sizeof(fm_ent_t) * FM_TREE);
    if (!e) return E_NOMEM;
    uint32_t n; int over;
    r = list_all(io, src, e, &n, &over);
    if (r >= 0 && over) r = E_MANY;
    for (uint32_t i = 0; r >= 0 && i < n; i++) {
        char s[FM_PATH], d[FM_PATH];
        if (pjoin(s, src, e[i].name) < 0 || pjoin(d, dst, e[i].name) < 0) { r = E_NAME; break; }
        r = copy_item(o, s, d, e[i].dir, e[i].size, depth + 1);
    }
    io->release(e);
    return r;
}

static int delete_item(op_t* o, const char* path, int dir, int depth) {
    const fm_io_t* io = o->io;
    if (dir) {
        if (depth >= DEPTH_MAX) return E_DEEP;
        fm_ent_t* e = (fm_ent_t*)io->alloc(sizeof(fm_ent_t) * FM_TREE);
        if (!e) return E_NOMEM;
        int r = 0;
        for (int pass = 0; pass < 64 && r >= 0; pass++) {            /* каталог больше FM_TREE: убираем порциями */
            uint32_t n; int over;
            r = list_all(io, path, e, &n, &over);
            if (r < 0 || n == 0) break;
            for (uint32_t i = 0; i < n && r >= 0; i++) {
                char c[FM_PATH];
                if (pjoin(c, path, e[i].name) < 0) { r = E_NAME; break; }
                r = delete_item(o, c, e[i].dir, depth + 1);
            }
            if (!over) break;
        }
        io->release(e);
        if (r < 0) return r;
    }
    int r = io->unlink(path);
    if (r < 0) return r;
    o->files++;
    if (o_tick(o, base_of(path))) return E_CANCEL;
    return 0;
}

/* ---------------- действия над отмеченным ---------------- */

typedef struct { char name[FM_NAME]; int dir; uint64_t size; } tgt_t;

static int targets(const fm_panel_t* P, int only_cur, tgt_t* t, int max) {
    int n = 0;
    if (!only_cur && P->nmark) {
        for (uint32_t i = 0; i < P->n && n < max; i++)
            if (P->e[i].mark) { scpy(t[n].name, P->e[i].name, FM_NAME); t[n].dir = P->e[i].dir; t[n].size = P->e[i].size; n++; }
    } else if (P->n && P->e[P->cur].kind != FM_KIND_UP) {
        const fm_ent_t* e = &P->e[P->cur];
        scpy(t[0].name, e->name, FM_NAME); t[0].dir = e->dir; t[0].size = e->size; n = 1;
    }
    return n;
}

static void log_line(const fm_io_t* io, const char* a, const char* b, const char* c, const char* d, uint64_t bytes) {
    if (!io->log) return;
    char l[FM_PATH * 2 + 64]; int p = ap(l, 0, sizeof l, "[KIRA FILES] ");
    p = ap(l, p, sizeof l, a); p = ap(l, p, sizeof l, " "); p = ap(l, p, sizeof l, b);
    if (c) { p = ap(l, p, sizeof l, " -> "); p = ap(l, p, sizeof l, c); }
    if (d) { p = ap(l, p, sizeof l, ": "); p = apu(l, p, sizeof l, bytes); p = ap(l, p, sizeof l, " байт"); }
    ap(l, p, sizeof l, "\n");
    io->log(l);
}

static void begin_busy(fm_t* f, int op, const char* title) {
    f->mode = FM_MODE_BUSY; f->op = op;
    scpy(f->dlg_title, title, sizeof f->dlg_title);
    f->prog_done = f->prog_total = 0; f->prog_files = f->prog_files_total = 0; f->prog_name[0] = 0;
}

static void end_busy(fm_t* f) { f->mode = FM_MODE_NORMAL; f->op = FM_OP_NONE; }

static void finish_msg(fm_t* f, const fm_io_t* io, const char* verb, int r, const char* culprit, const op_t* o) {
    char t[FM_NAME * 2];
    if (r >= 0) {
        int p = ap(f->msg, 0, sizeof f->msg, verb);
        p = ap(f->msg, p, sizeof f->msg, ": "); p = apu(f->msg, p, sizeof f->msg, o->files);
        p = ap(f->msg, p, sizeof f->msg, o->files == 1 ? " файл, " : " файлов, ");
        apsize(f->msg, p, sizeof f->msg, o->done);
        f->msg_err = 0;
        return;
    }
    int p = ap(t, 0, sizeof t, base_of(culprit)); (void)p;
    msg(f, 1, r == E_CANCEL ? "Прервано на " : "Ошибка: ", t, 0);
    int q = slen(f->msg);
    q = ap(f->msg, q, sizeof f->msg, " - ");
    ap(f->msg, q, sizeof f->msg, errtext(io, r));
}

/* Ядро копирования и переноса: имена t[0..n) из каталога sdir в каталог ddir. */
static void transfer_core(fm_t* f, const fm_io_t* io, const char* sdir, const tgt_t* t, int n, const char* ddir, int move) {
    op_t o = { f, io, 0, 0, 0, 0, 0 };
    begin_busy(f, move ? FM_OP_MOVE : FM_OP_COPY, move ? "Перенос" : "Копирование");
    int r = 0; char culprit[FM_PATH]; culprit[0] = 0;
    for (int i = 0; i < n && r >= 0; i++) {
        char s[FM_PATH];
        if (pjoin(s, sdir, t[i].name) < 0) { r = E_NAME; scpy(culprit, t[i].name, FM_PATH); break; }
        r = scan_item(&o, s, t[i].dir, t[i].size, 0);
        if (r < 0) scpy(culprit, s, FM_PATH);
    }
    for (int i = 0; i < n && r >= 0; i++) {
        char s[FM_PATH], d[FM_PATH];
        if (pjoin(s, sdir, t[i].name) < 0 || pjoin(d, ddir, t[i].name) < 0) { r = E_NAME; scpy(culprit, t[i].name, FM_PATH); break; }
        uint64_t before = o.done;
        r = copy_item(&o, s, d, t[i].dir, t[i].size, 0);
        if (r >= 0 && move) r = delete_item(&o, s, t[i].dir, 0);
        if (r < 0) { scpy(culprit, s, FM_PATH); break; }
        log_line(io, move ? "move" : "copy", s, d, d, o.done - before);
    }
    end_busy(f);
    finish_msg(f, io, move ? "Перенесено" : "Скопировано", r, culprit, &o);
    if (r < 0) log_line(io, move ? "move failed:" : "copy failed:", culprit, 0, 0, 0);
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0);
}

static void run_transfer(fm_t* f, const fm_io_t* io, int move, int only_cur) {
    fm_panel_t* S = &f->p[f->active]; fm_panel_t* D = &f->p[!f->active];
    static tgt_t t[FM_MARKS];
    int n = targets(S, only_cur, t, FM_MARKS);
    if (!n) { msg(f, 1, "Нечего ", move ? "переносить" : "копировать", 0); return; }
    if (seq(S->path, D->path)) { msg(f, 1, "Обе панели показывают один каталог: откройте другой во второй панели", 0, 0); return; }
    char sdir[FM_PATH], ddir[FM_PATH]; scpy(sdir, S->path, FM_PATH); scpy(ddir, D->path, FM_PATH);
    transfer_core(f, io, sdir, t, n, ddir, move);
    clear_marks(&f->p[f->active]);
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0);
}

/* Буфер обмена: Ctrl+C / Ctrl+X запоминают имена (отмеченные или под курсором), Ctrl+V вставляет в каталог активной панели. */
static void clip_take(fm_t* f, int cut) {
    fm_panel_t* P = &f->p[f->active];
    static tgt_t t[FM_MARKS];
    int n = targets(P, 0, t, FM_MARKS);
    if (!n) { msg(f, 1, "Нечего ", cut ? "вырезать" : "копировать", 0); return; }
    scpy(f->clip_dir, P->path, FM_PATH);
    for (int i = 0; i < n; i++) scpy(f->clip[i], t[i].name, FM_NAME);
    f->nclip = (uint32_t)n; f->clip_cut = cut != 0;
    char c[24]; int p = apu(c, 0, sizeof c, (uint64_t)n); (void)p;
    msg(f, 0, cut ? "Вырезано в буфер: " : "Скопировано в буфер: ", c, 0);
}

static void clip_paste(fm_t* f, const fm_io_t* io) {
    fm_panel_t* P = &f->p[f->active];
    if (!f->nclip) { msg(f, 1, "Буфер обмена пуст", 0, 0); return; }
    if (seq(f->clip_dir, P->path)) { msg(f, 1, "Вставка в тот же каталог: откройте другой", 0, 0); return; }
    static tgt_t t[FM_MARKS];
    int n = 0;
    for (uint32_t i = 0; i < f->nclip && n < FM_MARKS; i++) {
        char full[FM_PATH]; int dir = 0; uint64_t size = 0;
        if (pjoin(full, f->clip_dir, f->clip[i]) < 0 || io->stat(full, &dir, &size) != 0) continue;   /* исчезло - пропускаем */
        scpy(t[n].name, f->clip[i], FM_NAME); t[n].dir = dir; t[n].size = size; n++;
    }
    if (!n) { msg(f, 1, "Скопированного уже нет на месте", 0, 0); f->nclip = 0; return; }
    int cut = f->clip_cut;
    char sdir[FM_PATH], ddir[FM_PATH]; scpy(sdir, f->clip_dir, FM_PATH); scpy(ddir, P->path, FM_PATH);
    transfer_core(f, io, sdir, t, n, ddir, cut);
    if (cut) f->nclip = 0;                                          /* вырезанное вставляется один раз */
}

static int valid_name(const char* nm) {
    int n = slen(nm);
    if (n == 0 || n >= FM_NAME) return 0;
    if (seq(nm, ".") || seq(nm, "..")) return 0;
    for (int i = 0; i < n; i++) if (nm[i] == '/' || (unsigned char)nm[i] < 0x20) return 0;
    return 1;
}

static void run_mkdir(fm_t* f, const fm_io_t* io) {
    fm_panel_t* P = &f->p[f->active];
    char path[FM_PATH];
    if (!valid_name(f->input) || pjoin(path, P->path, f->input) < 0) { msg(f, 1, "Недопустимое имя каталога", 0, 0); return; }
    int r = io->mkdir(path);
    if (r < 0) { msg(f, 1, "Ошибка: ", errtext(io, r), 0); log_line(io, "mkdir failed:", path, 0, 0, 0); return; }
    log_line(io, "mkdir", path, 0, 0, 0);
    msg(f, 0, "Создан каталог ", f->input, 0);
    char keep[FM_NAME]; scpy(keep, f->input, FM_NAME);
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], i == f->active ? keep : 0);
}

static void run_rename(fm_t* f, const fm_io_t* io) {
    fm_panel_t* P = &f->p[f->active];
    if (!P->n || P->e[P->cur].kind == FM_KIND_UP) return;
    fm_ent_t e = P->e[P->cur];
    char s[FM_PATH], d[FM_PATH];
    if (!valid_name(f->input) || pjoin(s, P->path, e.name) < 0 || pjoin(d, P->path, f->input) < 0) { msg(f, 1, "Недопустимое имя", 0, 0); return; }
    if (seq(e.name, f->input)) return;
    /* в слоях файловых систем нет переименования: копия под новым именем, затем удаление старого (после успешной копии) */
    op_t o = { f, io, 0, 0, 0, 0, 0 };
    begin_busy(f, FM_OP_RENAME, "Переименование");
    int r = scan_item(&o, s, e.dir, e.size, 0);
    if (r >= 0) r = copy_item(&o, s, d, e.dir, e.size, 0);
    if (r >= 0) r = delete_item(&o, s, e.dir, 0);
    end_busy(f);
    if (r < 0) { finish_msg(f, io, "", r, s, &o); log_line(io, "rename failed:", s, 0, 0, 0); }
    else { msg(f, 0, "Переименовано: ", e.name, 0); int q = slen(f->msg); q = ap(f->msg, q, sizeof f->msg, " -> "); ap(f->msg, q, sizeof f->msg, f->input); log_line(io, "rename", s, d, d, o.done); }
    char keep[FM_NAME]; scpy(keep, r >= 0 ? f->input : e.name, FM_NAME);
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], i == f->active ? keep : 0);
}

static void run_delete(fm_t* f, const fm_io_t* io) {
    fm_panel_t* P = &f->p[f->active];
    static tgt_t t[FM_MARKS];
    int n = targets(P, 0, t, FM_MARKS);
    if (!n) return;
    op_t o = { f, io, 0, 0, 0, 0, 0 };
    begin_busy(f, FM_OP_DELETE, "Удаление");
    int r = 0; char culprit[FM_PATH]; culprit[0] = 0;
    for (int i = 0; i < n && r >= 0; i++) {
        char s[FM_PATH];
        if (pjoin(s, P->path, t[i].name) < 0) { r = E_NAME; scpy(culprit, t[i].name, FM_PATH); break; }
        r = delete_item(&o, s, t[i].dir, 0);
        if (r < 0) { scpy(culprit, s, FM_PATH); break; }
        log_line(io, "delete", s, 0, 0, 0);
    }
    end_busy(f);
    if (r < 0) { finish_msg(f, io, "", r, culprit, &o); log_line(io, "delete failed:", culprit, 0, 0, 0); }
    else {
        int p = ap(f->msg, 0, sizeof f->msg, "Удалено объектов: "); apu(f->msg, p, sizeof f->msg, o.files);
        f->msg_err = 0;
    }
    clear_marks(P);
    for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0);
}

/* ---------------- клавиши ---------------- */

static void toggle_mark(fm_t* f, fm_panel_t* P, int32_t i) {
    if (i < 0 || i >= (int32_t)P->n || P->e[i].kind == FM_KIND_UP) return;
    fm_ent_t* e = &P->e[i];
    if (e->mark) {
        e->mark = 0;
        for (uint32_t k = 0; k < FM_MARKS; k++)
            if (seq(P->mk[k], e->name)) {                              /* пустое место закрываем последним именем */
                uint32_t last = k; while (last + 1 < FM_MARKS && P->mk[last + 1][0]) last++;
                scpy(P->mk[k], P->mk[last], FM_NAME); P->mk[last][0] = 0;
                break;
            }
    } else {
        if (P->nmark >= FM_MARKS) { msg(f, 1, "Отметить можно не больше 64 объектов за раз", 0, 0); return; }
        uint32_t k = 0; while (k < FM_MARKS && P->mk[k][0]) k++;
        e->mark = 1; scpy(P->mk[k], e->name, FM_NAME);
    }
    recount_marks(P);
}

static void go_up(fm_t* f, const fm_io_t* io, int pi) {
    fm_panel_t* P = &f->p[pi];
    if (P->path[0] == '/' && !P->path[1]) return;
    char par[FM_PATH], child[FM_NAME];
    scpy(par, P->path, FM_PATH);
    int n = slen(par); while (n > 1 && par[n - 1] == '/') n--;
    par[n] = 0;
    int cut = n; while (cut > 0 && par[cut - 1] != '/') cut--;
    scpy(child, par + cut, FM_NAME);
    par[cut > 1 ? cut - 1 : 1] = 0;
    goto_keep(f, io, pi, par[0] ? par : "/", child);
}

static void enter_cur(fm_t* f, const fm_io_t* io, int view) {
    fm_panel_t* P = &f->p[f->active];
    if (!P->n) return;
    fm_ent_t* e = &P->e[P->cur];
    if (e->kind == FM_KIND_UP) { go_up(f, io, f->active); return; }
    char full[FM_PATH];
    if (pjoin(full, P->path, e->name) < 0) { msg(f, 1, "Слишком длинный путь", 0, 0); return; }
    if (e->dir) {
        if (view) { msg(f, 0, "Это каталог: Enter - войти", 0, 0); return; }
        int r = goto_keep(f, io, f->active, full, 0);
        if (r < 0) msg(f, 1, "Не открыть каталог: ", errtext(io, r), 0);
        return;
    }
    f->act_kind = view ? FM_ACT_VIEW : FM_ACT_OPEN;
    scpy(f->act_path, full, sizeof f->act_path);
    f->act_seq++;
}

static void ask_input(fm_t* f, int op, const char* title, const char* text, const char* init) {
    f->mode = FM_MODE_INPUT; f->op = op;
    scpy(f->dlg_title, title, sizeof f->dlg_title);
    scpy(f->dlg_text, text, sizeof f->dlg_text);
    scpy(f->input, init, FM_NAME);
    f->input_len = slen(f->input);
}

static void ask_delete(fm_t* f) {
    fm_panel_t* P = &f->p[f->active];
    static tgt_t t[FM_MARKS];
    int n = targets(P, 0, t, FM_MARKS);
    if (!n) { msg(f, 1, "Нечего удалять", 0, 0); return; }
    f->mode = FM_MODE_CONFIRM; f->op = FM_OP_DELETE;
    scpy(f->dlg_title, "Удаление", sizeof f->dlg_title);
    int p;
    if (n == 1) {
        p = ap(f->dlg_text, 0, sizeof f->dlg_text, t[0].dir ? "Удалить каталог со всем содержимым «" : "Удалить файл «");
        p = ap(f->dlg_text, p, sizeof f->dlg_text, t[0].name); ap(f->dlg_text, p, sizeof f->dlg_text, "»?");
    } else {
        p = ap(f->dlg_text, 0, sizeof f->dlg_text, "Удалить отмеченное: ");
        p = apu(f->dlg_text, p, sizeof f->dlg_text, (uint64_t)n); ap(f->dlg_text, p, sizeof f->dlg_text, " объектов?");
    }
}

static void input_key(fm_t* f, const fm_io_t* io, int key, int ch) {
    if (key == KEY_ESC) { f->mode = FM_MODE_NORMAL; f->op = FM_OP_NONE; return; }
    if (key == KEY_ENTER) {
        int op = f->op;
        f->mode = FM_MODE_NORMAL; f->op = FM_OP_NONE;
        if (op == FM_OP_MKDIR) run_mkdir(f, io);
        else if (op == FM_OP_RENAME) run_rename(f, io);
        return;
    }
    if (key == KEY_BACKSPACE) {
        while (f->input_len > 0 && ((unsigned char)f->input[f->input_len - 1] & 0xC0) == 0x80) f->input_len--;
        if (f->input_len > 0) f->input_len--;
        f->input[f->input_len] = 0;
        return;
    }
    if (ch >= 0x20 && ch < 0x7F && ch != '/') {
        if (f->input_len + 1 < FM_NAME) { f->input[f->input_len++] = (char)ch; f->input[f->input_len] = 0; }
    } else if (ch >= 0x80 && ch < 0x800 && f->input_len + 2 < FM_NAME) {                 /* кириллица: два байта UTF-8 */
        f->input[f->input_len++] = (char)(0xC0 | (ch >> 6));
        f->input[f->input_len++] = (char)(0x80 | (ch & 0x3F));
        f->input[f->input_len] = 0;
    }
}

static void move_cur(fm_t* f, int32_t to) {
    fm_panel_t* P = &f->p[f->active];
    P->cur = to; clamp_view(P);
}

/* ---------------- команды (клавиши, панель инструментов, контекстное меню) ---------------- */

static void cmd_rename(fm_t* f) {
    fm_panel_t* P = &f->p[f->active];
    if (P->n && P->e[P->cur].kind != FM_KIND_UP) ask_input(f, FM_OP_RENAME, "Переименовать", "Новое имя:", P->e[P->cur].name);
    else msg(f, 1, "Выберите файл или каталог для переименования", 0, 0);
}

static void cmd_pin(fm_t* f) {
    fm_panel_t* P = &f->p[f->active];
    char full[FM_PATH];
    if (P->n && !P->e[P->cur].dir && P->e[P->cur].kind == FM_KIND_PROG && pjoin(full, P->path, P->e[P->cur].name) == 0) {
        f->act_kind = FM_ACT_PIN; scpy(f->act_path, full, sizeof f->act_path); f->act_seq++;
    } else msg(f, 1, "Закрепить можно программу из /bin", 0, 0);
}

static void select_all(fm_t* f) {
    fm_panel_t* P = &f->p[f->active];
    uint32_t added = 0;
    for (uint32_t i = 0; i < P->n; i++) {
        if (P->e[i].kind == FM_KIND_UP || P->e[i].mark) continue;
        if (P->nmark >= FM_MARKS) { msg(f, 1, "Выделено 64 объекта - больше за раз отметить нельзя", 0, 0); break; }
        uint32_t k = 0; while (k < FM_MARKS && P->mk[k][0]) k++;
        P->e[i].mark = 1; scpy(P->mk[k], P->e[i].name, FM_NAME);
        recount_marks(P); added++;
    }
    recount_marks(P);
    (void)added;
}

static void hist_go(fm_t* f, const fm_io_t* io, int fwd) {
    fm_panel_t* P = &f->p[f->active];
    uint32_t* n = fwd ? &P->nfwd : &P->nback;
    if (!*n) { msg(f, 1, fwd ? "Вперёд идти некуда" : "Назад идти некуда", 0, 0); return; }
    char target[FM_PATH], here[FM_PATH];
    scpy(target, (fwd ? P->fwd : P->back)[*n - 1], FM_PATH);
    scpy(here, P->path, FM_PATH);
    (*n)--;
    g_nohist = 1;
    const char* keep = is_inside(here, target) && !seq(here, target) ? base_of(here) : 0;   /* вышли из подкаталога - курсор на нём */
    int r = goto_keep(f, io, f->active, target, keep);
    g_nohist = 0;
    if (r < 0) { msg(f, 1, "Не открыть каталог: ", errtext(io, r), 0); return; }
    if (fwd) hist_push(P->back, &P->nback, here); else hist_push(P->fwd, &P->nfwd, here);
}

void fm_cmd(fm_t* f, const fm_io_t* io, int cmd) {
    fm_panel_t* P = &f->p[f->active];
    switch (cmd) {
    case FM_CMD_BACK: hist_go(f, io, 0); break;
    case FM_CMD_FWD: hist_go(f, io, 1); break;
    case FM_CMD_UP: go_up(f, io, f->active); break;
    case FM_CMD_REFRESH: for (int i = 0; i < 2; i++) load_panel(f, io, &f->p[i], 0); msg(f, 0, "Список обновлён", 0, 0); break;
    case FM_CMD_MKDIR: ask_input(f, FM_OP_MKDIR, "Новый каталог", "Имя каталога:", ""); break;
    case FM_CMD_COPY: clip_take(f, 0); break;
    case FM_CMD_CUT: clip_take(f, 1); break;
    case FM_CMD_PASTE: clip_paste(f, io); break;
    case FM_CMD_DELETE: ask_delete(f); break;
    case FM_CMD_RENAME: cmd_rename(f); break;
    case FM_CMD_OPEN: enter_cur(f, io, 0); break;
    case FM_CMD_VIEW: enter_cur(f, io, 1); break;
    case FM_CMD_SELALL: select_all(f); break;
    case FM_CMD_HIDDEN: fm_set_hidden(f, io, !f->show_hidden); break;
    case FM_CMD_PIN: cmd_pin(f); break;
    case FM_CMD_XCOPY: run_transfer(f, io, 0, 0); break;
    case FM_CMD_XMOVE: run_transfer(f, io, 1, 0); break;
    default: break;
    }
    (void)P;
}

/* ---------------- контекстное меню ---------------- */

int fm_menu_build(const fm_t* f, int* id, const char** label, unsigned* off) {
    const fm_panel_t* P = &f->p[f->active];
    int has = P->n > 0 && P->cur < (int32_t)P->n, real = has && P->e[P->cur].kind != FM_KIND_UP;
    int file = real && !P->e[P->cur].dir, prog = real && P->e[P->cur].kind == FM_KIND_PROG;
    int other = !seq(f->p[0].path, f->p[1].path);
    struct { int id; const char* label; int ok; } it[] = {
        { FM_CMD_OPEN, "Открыть", has }, { FM_CMD_VIEW, "Просмотр", file }, { FM_CMD_NONE, "", 1 },
        { FM_CMD_COPY, "Копировать       Ctrl+C", real }, { FM_CMD_CUT, "Вырезать         Ctrl+X", real },
        { FM_CMD_PASTE, "Вставить         Ctrl+V", f->nclip > 0 }, { FM_CMD_NONE, "", 1 },
        { FM_CMD_RENAME, "Переименовать   F2", real }, { FM_CMD_DELETE, "Удалить          Del", real }, { FM_CMD_NONE, "", 1 },
        { FM_CMD_XCOPY, "Копия в другую панель  F5", real && other }, { FM_CMD_XMOVE, "Перенос в другую панель  F6", real && other },
        { FM_CMD_NONE, "", 1 }, { FM_CMD_MKDIR, "Новая папка      F7", 1 }, { FM_CMD_SELALL, "Выделить всё     Ctrl+A", P->n > 0 },
        { FM_CMD_REFRESH, "Обновить", 1 },
        { FM_CMD_NONE, "", 1 }, { FM_CMD_PIN, "Закрепить в панели  F4", prog },
    };
    int n = (int)(sizeof it / sizeof it[0]);
    if (!prog) n -= 2;                                              /* закрепление - только для программ /bin */
    *off = 0;
    for (int i = 0; i < n && i < FM_MENU_MAX; i++) { id[i] = it[i].id; label[i] = it[i].label; if (!it[i].ok) *off |= 1u << i; }
    return n;
}

static int menu_first(const fm_t* f, int from, int dir) {          /* ближайший доступный пункт от from в сторону dir; -1 - нет */
    int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
    int n = fm_menu_build(f, id, lb, &off);
    for (int i = from; i >= 0 && i < n; i += dir) if (id[i] != FM_CMD_NONE && !((off >> i) & 1)) return i;
    return -1;
}

static void menu_open(fm_t* f, int x, int y) {
    f->mode = FM_MODE_MENU; f->menu_x = x; f->menu_y = y;
    int s = menu_first(f, 0, 1);
    f->menu_sel = s < 0 ? 0 : s;
}

static void menu_exec(fm_t* f, const fm_io_t* io, int item) {
    int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
    int n = fm_menu_build(f, id, lb, &off);
    f->mode = FM_MODE_NORMAL;
    if (item < 0 || item >= n || id[item] == FM_CMD_NONE || ((off >> item) & 1)) return;
    fm_cmd(f, io, id[item]);
}

static void menu_key(fm_t* f, const fm_io_t* io, int key) {
    if (key == KEY_ESC) { f->mode = FM_MODE_NORMAL; return; }
    if (key == KEY_ENTER) { menu_exec(f, io, f->menu_sel); return; }
    if (key == KEY_UP) { int k = menu_first(f, f->menu_sel - 1, -1); if (k >= 0) f->menu_sel = k; }
    else if (key == KEY_DOWN) { int k = menu_first(f, f->menu_sel + 1, 1); if (k >= 0) f->menu_sel = k; }
    else if (key == KEY_HOME) { int k = menu_first(f, 0, 1); if (k >= 0) f->menu_sel = k; }
    else if (key == KEY_END) { int k = menu_first(f, FM_MENU_MAX - 1, -1); if (k >= 0) f->menu_sel = k; }
}

/* Состояние одной строкой для стенда и отладки: по нему сценарии проверяют навигацию без снимков экрана. */
void fm_state_line(const fm_t* f, char* out, int cap) {
    static const char* const mode[] = { "normal", "input", "confirm", "busy", "help", "menu" };
    static const char* const sortn[] = { "name", "size", "type" };
    int p = ap(out, 0, cap, "[KIRA FILES] state: active="); p = ap(out, p, cap, f->active ? "R" : "L");
    p = ap(out, p, cap, " mode="); p = ap(out, p, cap, mode[f->mode >= 0 && f->mode <= FM_MODE_MENU ? f->mode : 0]);
    for (int i = 0; i < 2; i++) {
        const fm_panel_t* P = &f->p[i];
        p = ap(out, p, cap, i ? " R{path=" : " L{path="); p = ap(out, p, cap, P->path);
        p = ap(out, p, cap, " cur="); p = ap(out, p, cap, P->n && P->cur < (int32_t)P->n ? P->e[P->cur].name : "-");
        p = ap(out, p, cap, " n="); p = apu(out, p, cap, P->n);
        p = ap(out, p, cap, " marks="); p = apu(out, p, cap, P->nmark);
        p = ap(out, p, cap, " sort="); p = ap(out, p, cap, sortn[P->sort_col >= 0 && P->sort_col < FM_SORT_COLS ? P->sort_col : 0]);
        p = ap(out, p, cap, P->sort_desc ? ":desc" : ":asc");
        p = ap(out, p, cap, " back="); p = apu(out, p, cap, P->nback);
        p = ap(out, p, cap, " fwd="); p = apu(out, p, cap, P->nfwd); p = ap(out, p, cap, "}");
    }
    p = ap(out, p, cap, " clip="); p = apu(out, p, cap, f->nclip); if (f->nclip) p = ap(out, p, cap, f->clip_cut ? "(cut)" : "(copy)");
    p = ap(out, p, cap, " menu="); if (f->mode == FM_MODE_MENU) p = apu(out, p, cap, (uint64_t)f->menu_sel); else p = ap(out, p, cap, "-");
    p = ap(out, p, cap, " hidden="); p = ap(out, p, cap, f->show_hidden ? "1" : "0");
    if (f->mode == FM_MODE_CONFIRM || f->mode == FM_MODE_INPUT) { p = ap(out, p, cap, " dialog=\""); p = ap(out, p, cap, f->dlg_title); p = ap(out, p, cap, "\""); }
    if (f->mode == FM_MODE_INPUT) { p = ap(out, p, cap, " input=\""); p = ap(out, p, cap, f->input); p = ap(out, p, cap, "\""); }
    if (f->msg[0]) { p = ap(out, p, cap, f->msg_err ? " error=\"" : " msg=\""); p = ap(out, p, cap, f->msg); p = ap(out, p, cap, "\""); }
    ap(out, p, cap, "\n");
}

/* ---------------- клавиши ---------------- */

void fm_key(fm_t* f, const fm_io_t* io, int key, int ch) { fm_key_m(f, io, key, ch, 0); }

void fm_key_m(fm_t* f, const fm_io_t* io, int key, int ch, int mods) {
    if (f->mode == FM_MODE_BUSY) return;
    if (f->mode == FM_MODE_HELP) { f->mode = FM_MODE_NORMAL; f->version++; return; }
    if (f->mode == FM_MODE_INPUT) { input_key(f, io, key, ch); f->version++; return; }
    if (f->mode == FM_MODE_MENU) { menu_key(f, io, key); f->version++; return; }
    if (f->mode == FM_MODE_CONFIRM) {
        if (key == KEY_ENTER || ch == 'y' || ch == 'Y' || ch == 0x434 || ch == 0x414) {
            f->mode = FM_MODE_NORMAL;
            if (f->op == FM_OP_DELETE) run_delete(f, io);
        } else if (key == KEY_ESC || ch == 'n' || ch == 'N' || ch == 0x43D || ch == 0x41D) { f->mode = FM_MODE_NORMAL; f->op = FM_OP_NONE; }
        f->version++;
        return;
    }
    fm_panel_t* P = &f->p[f->active];
    f->msg[0] = 0; f->msg_err = 0;
    if (mods & 2) {                                                 /* Ctrl: буфер обмена и выделение */
        int c = key == 'c' ? FM_CMD_COPY : key == 'x' ? FM_CMD_CUT : key == 'v' ? FM_CMD_PASTE : key == 'a' ? FM_CMD_SELALL : FM_CMD_NONE;
        if (c != FM_CMD_NONE) { fm_cmd(f, io, c); f->version++; return; }
    }
    if (mods & 4) {                                                 /* Alt: история и вверх */
        int c = key == KEY_LEFT ? FM_CMD_BACK : key == KEY_RIGHT ? FM_CMD_FWD : key == KEY_UP ? FM_CMD_UP : FM_CMD_NONE;
        if (c != FM_CMD_NONE) { fm_cmd(f, io, c); f->version++; return; }
    }
    if ((mods & 1) && key == KEY_F10) { menu_open(f, -1, -1); f->version++; return; }   /* Shift+F10 - контекстное меню */
    int32_t cur = P->cur, n = (int32_t)P->n;
    switch (key) {
    case KEY_TAB: case KEY_LEFT: case KEY_RIGHT: f->active = !f->active; break;
    case KEY_UP: if (cur > 0) move_cur(f, cur - 1); break;
    case KEY_DOWN: if (cur + 1 < n) move_cur(f, cur + 1); break;
    case KEY_PGUP: move_cur(f, cur - (FM_ROWS - 1) < 0 ? 0 : cur - (FM_ROWS - 1)); break;
    case KEY_PGDN: move_cur(f, cur + (FM_ROWS - 1) >= n ? n - 1 : cur + (FM_ROWS - 1)); break;
    case KEY_HOME: move_cur(f, 0); break;
    case KEY_END: move_cur(f, n - 1); break;
    case KEY_ENTER: enter_cur(f, io, 0); break;
    case KEY_BACKSPACE: go_up(f, io, f->active); break;
    case KEY_ESC: clear_marks(P); break;
    case KEY_INSERT: case ' ': toggle_mark(f, P, cur); if (cur + 1 < n) move_cur(f, cur + 1); break;
    case KEY_F1: f->mode = FM_MODE_HELP; break;
    case KEY_F2: cmd_rename(f); break;
    case KEY_F3: enter_cur(f, io, 1); break;
    case KEY_F4: cmd_pin(f); break;
    case KEY_F5: run_transfer(f, io, 0, 0); break;
    case KEY_F6: run_transfer(f, io, 1, 0); break;
    case KEY_F7: ask_input(f, FM_OP_MKDIR, "Новый каталог", "Имя каталога:", ""); break;
    case KEY_F8: case KEY_DELETE: ask_delete(f); break;
    case KEY_F9: fm_set_hidden(f, io, !f->show_hidden); break;
    default: break;
    }
    f->version++;
}

void fm_mouse(fm_t* f, const fm_io_t* io, int kind, int panel, int row) { fm_mouse_at(f, io, kind, panel, row, -1, -1); }

static const char* sort_name(int col) { return col == FM_SORT_SIZE ? "по размеру" : col == FM_SORT_TYPE ? "по типу" : "по имени"; }

void fm_mouse_at(fm_t* f, const fm_io_t* io, int kind, int panel, int row, int x, int y) {
    if (f->mode == FM_MODE_MENU) {
        if (kind == FM_MOUSE_MENUPICK) { menu_exec(f, io, row); f->version++; return; }
        f->mode = FM_MODE_NORMAL;                                   /* щелчок мимо меню закрывает его */
        if (kind != FM_MOUSE_RCLICK) { f->version++; return; }
    }
    if (f->mode != FM_MODE_NORMAL) return;
    f->msg[0] = 0; f->msg_err = 0;
    if (kind == FM_MOUSE_CMD) { fm_cmd(f, io, row); f->version++; return; }
    if (panel < 0 || panel > 1) return;
    f->active = panel;
    fm_panel_t* P = &f->p[panel];
    if (kind == FM_MOUSE_SORT) {
        if (row < 0 || row >= FM_SORT_COLS) return;
        if (P->sort_col == row) P->sort_desc = !P->sort_desc; else { P->sort_col = row; P->sort_desc = 0; }
        load_panel(f, io, P, 0);
        msg(f, 0, "Сортировка ", sort_name(P->sort_col), P->sort_desc ? ", по убыванию" : ", по возрастанию");
        f->version++;
        return;
    }
    if (row >= 0 && row < (int)P->n) { P->cur = row; clamp_view(P); }
    if (kind == FM_MOUSE_DOUBLE && row >= 0 && row < (int)P->n) enter_cur(f, io, 0);
    else if (kind == FM_MOUSE_DROP && row >= 0 && row < (int)P->n) run_transfer(f, io, 0, !P->e[row].mark);
    else if (kind == FM_MOUSE_RCLICK) menu_open(f, x, y);
    f->version++;
}

void fm_page(fm_t* f, int panel, int dir) {
    if (f->mode != FM_MODE_NORMAL || panel < 0 || panel > 1) return;
    fm_panel_t* P = &f->p[panel];
    int32_t maxtop = (int32_t)P->n - FM_ROWS; if (maxtop < 0) maxtop = 0;
    P->top += dir * (FM_ROWS - 1);
    if (P->top < 0) P->top = 0;
    if (P->top > maxtop) P->top = maxtop;
    if (P->cur < P->top) P->cur = P->top;
    if (P->cur >= P->top + FM_ROWS) P->cur = P->top + FM_ROWS - 1;
    if (P->cur >= (int32_t)P->n) P->cur = (int32_t)P->n - 1;
    if (P->cur < 0) P->cur = 0;
    f->version++;
}
