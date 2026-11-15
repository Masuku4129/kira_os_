/* ksh - оболочка Киры: интерпретатор командных скриптов. Один файл на чистом C (libc), без setjmp.
   Собирается в WebAssembly (wasm32-wasip1, tools/ksh_wasm.sh) и обычным компилятором (для тестов на хосте).

   Запуск:   ksh файл.sh [арги]   ksh [-e] [-x] [-u] -c код...   ksh (интерактивно: приглашение "ksh> ", Ctrl+D - выход).
   Язык:     переменные и подстановки ($x ${x} ${x:-d} ${x:=d} ${x:+d} ${x:?m} ${#x} ${x#п} ${x##п} ${x%п} ${x%%п} $((арифметика)) $(команда) `команда`),
             кавычки '..' ".." и \, if/elif/else, while, until, for in, case, функции, local, return/break/continue, ! && || ; перевод строки,
             конвейеры, перенаправления < > >> N> 2>&1 и документ-вставка <<, группы { } и ( ), комментарии, $? $# $0 $1.. $@ $*, шаблоны * ? [..].
   Встроенные: : true false echo printf test [ cat ls rm cp mv wc head pwd cd read set unset export local shift eval source . exit return break continue expr.
   Внешних программ нет (в WASM нет fork/exec): всё, что не встроенное и не функция, - "команда не найдена" (127).
   Файлы - только плоская песочница (/data/wasm): имена [A-Za-z0-9._-] до 40 знаков, без каталогов.
   Конвейер выполняется по очереди: вывод каждой команды копится в памяти и становится вводом следующей. Последняя команда конвейера (как в ksh93)
   выполняется в самой оболочке, поэтому `echo 1 | read x` задаёт x.
   Глубина вложенности ограничена по реальному расходу стека (в WASM он 64 КБ), при превышении - сообщение и выход, без аварии. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>
#include <dirent.h>

typedef long long ll;
typedef unsigned long long ull;

/* ---------- память, строки ---------- */
static void* xm(size_t n) { void* p = malloc(n ? n : 1); if (!p) { fputs("ksh: нет памяти\n", stderr); exit(70); } return p; }
static void* xr(void* q, size_t n) { void* p = realloc(q, n ? n : 1); if (!p) { fputs("ksh: нет памяти\n", stderr); exit(70); } return p; }
static char* xndup(const char* s, size_t n) { char* p = xm(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char* xdup(const char* s) { return xndup(s, strlen(s)); }

typedef struct { char* p; size_t n, cap; } Buf;
static void b_put(Buf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) { size_t c = b->cap ? b->cap : 64; while (c < b->n + n + 1) c *= 2; b->p = xr(b->p, c); b->cap = c; }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n; b->p[b->n] = 0;
}
static void b_c(Buf* b, char c) { b_put(b, &c, 1); }
static void b_s(Buf* b, const char* s) { b_put(b, s, strlen(s)); }
static char* b_take(Buf* b) { char* p = b->p ? b->p : xdup(""); b->p = 0; b->n = b->cap = 0; return p; }
static void b_free(Buf* b) { free(b->p); b->p = 0; b->n = b->cap = 0; }

typedef struct { char** v; int n, cap; } Argv;          /* v всегда оканчивается нулём (после первого добавления) */
static void av_push(Argv* a, char* s) {
    if (a->n + 2 > a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = xr(a->v, (size_t)a->cap * sizeof(char*)); }
    a->v[a->n++] = s; a->v[a->n] = 0;
}
static void av_free(Argv* a) { for (int i = 0; i < a->n; i++) free(a->v[i]); free(a->v); a->v = 0; a->n = a->cap = 0; }

static size_t utf8_count(const char* s, size_t n) { size_t c = 0; for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++; return c; }
static size_t utf8_clen(const char* s, size_t left) {        /* длина символа UTF-8 с начала s (не больше left) */
    unsigned char c = (unsigned char)s[0]; size_t l = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return l > left ? left : l;
}

/* ---------- состояние оболочки ---------- */
enum { C_NONE, C_BREAK, C_CONT, C_RET, C_EXIT };
static int g_st;                       /* $? */
static int g_cs;                       /* статус последней подстановки команды (для команды без слов) */
static int g_ctl, g_ctln;              /* ожидающее управление: break/continue/return/exit */
static int g_opt_e, g_opt_x, g_opt_u;  /* set -e -x -u */
static int g_noerr, g_loops, g_rdepth, g_experr;
static char* g_name;                   /* $0 */
static char** g_pa; static int g_pn;   /* позиционные параметры $1.. */
static char* g_stack0;
#ifdef __wasm__
#define STACK_LIM 40000                  /* из 64 КБ стека WASM; остальное - запас libc */
#else
#define STACK_LIM 4000000                /* на хосте стек 8 МБ (и санитайзеры раздувают кадры) */
#endif
static int too_deep(void) { char c; long d = (long)(g_stack0 - &c); if (d < 0) d = -d; return d > STACK_LIM; }
/* Вложенность считаем и по числу вызовов: интерпретатор WASM допускает 4000 вложенных вызовов (userland/wasm.c, DEPTH), а на один уровень
   нашей вложенности приходится до 9 вызовов. REC_ENTER/REC_LEAVE ставятся парой в точках рекурсии. */
#ifdef __wasm__
#define REC_MAX 400
#else
#define REC_MAX 100000000
#endif
static int g_rec;
#define REC_ENTER() (++g_rec > REC_MAX || too_deep())
#define REC_LEAVE() (g_rec--)

typedef struct { char* name; char* val; int exp; } Var;
static Var* g_vars; static int g_nv, g_cv;
static int var_find(const char* n) { for (int i = 0; i < g_nv; i++) if (!strcmp(g_vars[i].name, n)) return i; return -1; }
static const char* var_get(const char* n) { int i = var_find(n); return i >= 0 ? g_vars[i].val : 0; }
static void var_set(const char* n, const char* v) {
    int i = var_find(n);
    if (i < 0) {
        if (g_nv == g_cv) { g_cv = g_cv ? g_cv * 2 : 16; g_vars = xr(g_vars, (size_t)g_cv * sizeof(Var)); }
        i = g_nv++; g_vars[i].name = xdup(n); g_vars[i].val = 0; g_vars[i].exp = 0;
    }
    char* nv = xdup(v); free(g_vars[i].val); g_vars[i].val = nv;
}
static void var_unset(const char* n) {
    int i = var_find(n); if (i < 0) return;
    free(g_vars[i].name); free(g_vars[i].val); memmove(&g_vars[i], &g_vars[i + 1], (size_t)(g_nv - i - 1) * sizeof(Var)); g_nv--;
}
static int is_name(const char* s, size_t n) {
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

typedef struct { char* name; char* old; int had, exp; } Loc;        /* сохранённое значение local */
static Loc* g_loc; static int g_nloc, g_cloc, g_locbase;
static void loc_restore_to(int base) {
    while (g_nloc > base) {
        Loc* l = &g_loc[--g_nloc];
        if (l->had) { var_set(l->name, l->old); int i = var_find(l->name); if (i >= 0) g_vars[i].exp = l->exp; } else var_unset(l->name);
        free(l->name); free(l->old);
    }
}

/* ---------- синтаксическое дерево ---------- */
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_NOT, N_LIST, N_IF, N_WHILE, N_FOR, N_CASE, N_FUNC, N_GROUP, N_SUB };
enum { R_IN, R_OUT, R_APP, R_DUP, R_HERE };
typedef struct { int fd, op, hdq; char* word; char* hd; } Redir;
typedef struct CaseItem { char** pats; int np; struct Node* body; } CaseItem;
typedef struct Node {
    int type, refs, flag, nw, nr, nl, nci;
    char** w; Redir* r; struct Node *a, *b, *c, **l; char* name; CaseItem* ci;
} Node;
static Node* nn(int type) { Node* n = xm(sizeof *n); memset(n, 0, sizeof *n); n->type = type; n->refs = 1; return n; }
static void w_push(Node* n, char* s) { n->w = xr(n->w, (size_t)(n->nw + 1) * sizeof(char*)); n->w[n->nw++] = s; }
static void nl_push(Node* n, Node* c) { n->l = xr(n->l, (size_t)(n->nl + 1) * sizeof(Node*)); n->l[n->nl++] = c; }
static void free_node(Node* n) {
    if (!n || --n->refs > 0) return;
    for (int i = 0; i < n->nw; i++) free(n->w[i]);
    free(n->w);
    for (int i = 0; i < n->nr; i++) { free(n->r[i].word); free(n->r[i].hd); }
    free(n->r);
    free_node(n->a); free_node(n->b); free_node(n->c);
    for (int i = 0; i < n->nl; i++) free_node(n->l[i]);
    free(n->l); free(n->name);
    for (int i = 0; i < n->nci; i++) { for (int k = 0; k < n->ci[i].np; k++) free(n->ci[i].pats[k]); free(n->ci[i].pats); free_node(n->ci[i].body); }
    free(n->ci); free(n);
}

typedef struct { char* name; Node* body; } Fn;
static Fn* g_fns; static int g_nf;
static Fn* fn_find(const char* n) { for (int i = 0; i < g_nf; i++) if (!strcmp(g_fns[i].name, n)) return &g_fns[i]; return 0; }
static void fn_define(const char* n, Node* body) {
    Fn* f = fn_find(n);
    body->refs++;
    if (f) { free_node(f->body); f->body = body; return; }
    g_fns = xr(g_fns, (size_t)(g_nf + 1) * sizeof(Fn)); g_fns[g_nf].name = xdup(n); g_fns[g_nf].body = body; g_nf++;
}
static void fn_remove(const char* n) {
    Fn* f = fn_find(n); if (!f) return;
    free(f->name); free_node(f->body); memmove(f, f + 1, (size_t)(g_nf - 1 - (int)(f - g_fns)) * sizeof(Fn)); g_nf--;
}

/* ---------- ввод и вывод ---------- */
typedef struct { FILE* f; Buf* b; } Sink;
typedef struct { FILE* f; const char* m; size_t mn, mp; } Src;
typedef struct { Src* in; Sink out, err; } Io;

static void sink_put(Sink* s, const char* p, size_t n) {
    if (!n) return;
    if (s->b) b_put(s->b, p, n);
    else { if (s->f == stderr) fflush(stdout); fwrite(p, 1, n, s->f); }
}
static void out_s(Io* io, const char* s) { sink_put(&io->out, s, strlen(s)); }
static void emsg(Io* io, const char* fmt, ...) {            /* "ksh: сообщение\n" в поток ошибок */
    char b[320]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    sink_put(&io->err, "ksh: ", 5); sink_put(&io->err, b, (size_t)n);
}
static int src_getc(Src* s) {
    if (s->f) { if (s->f == stdin) fflush(stdout); return fgetc(s->f); }
    return s->mp < s->mn ? (unsigned char)s->m[s->mp++] : EOF;
}
static size_t src_read(Src* s, char* buf, size_t cap) {      /* из консоли - построчно, из файла и памяти - блоком */
    if (s->f && s->f != stdin) return fread(buf, 1, cap, s->f);
    if (!s->f) { size_t n = s->mn - s->mp; if (n > cap) n = cap; memcpy(buf, s->m + s->mp, n); s->mp += n; return n; }
    size_t n = 0; int c;
    while (n < cap && (c = src_getc(s)) != EOF) { buf[n++] = (char)c; if (c == '\n') break; }
    return n;
}

/* песочница: плоские имена */
static int valid_fname(const char* s) {
    size_t n = strlen(s);
    if (!n || n > 40 || !isalnum((unsigned char)s[0])) return 0;          /* первый знак - буква или цифра (как в wasi_host.h) */
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}
static FILE* fopen_sb(Io* io, const char* name, const char* mode, int quiet) {
    if (!valid_fname(name)) { if (!quiet) emsg(io, "%s: недопустимое имя (до 40 знаков A-Za-z0-9._-, первый - буква или цифра, без каталогов)", name); return 0; }
    FILE* f = fopen(name, mode);
    if (!f && !quiet) emsg(io, mode[0] == 'r' ? "%s: нет такого файла" : "%s: не удаётся открыть для записи", name);
    return f;
}
static ll file_size(const char* name) {
    if (!valid_fname(name)) return -1;
    FILE* f = fopen(name, "rb"); if (!f) return -1;
    ll r = -1; if (!fseek(f, 0, SEEK_END)) r = ftell(f);
    fclose(f); return r;
}
static int cmp_s(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static int sandbox_list(Argv* out) {
    DIR* d = opendir("."); if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; av_push(out, xdup(e->d_name)); }
    closedir(d);
    if (out->n > 1) qsort(out->v, (size_t)out->n, sizeof(char*), cmp_s);
    return 0;
}
static char* slurp(Io* io, const char* name, size_t* len) {
    FILE* f = fopen_sb(io, name, "rb", 0); if (!f) return 0;
    Buf b = {0}; static char tmp[1024]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) b_put(&b, tmp, n);
    fclose(f);
    if (len) *len = b.n;
    return b_take(&b);
}

/* ---------- шаблоны (* ? [..] \) ---------- */
static int pm_set(const char* p, const char* s, int* ok) {         /* p указывает на '[': возвращает длину набора или 0, если не набор */
    size_t i = 1; int neg = 0, hit = 0;
    if (p[i] == '!' || p[i] == '^') { neg = 1; i++; }
    size_t first = i;
    for (;;) {
        if (!p[i]) return 0;
        if (p[i] == ']' && i > first) break;
        unsigned char lo = (unsigned char)p[i];
        if (lo == '\\' && p[i + 1]) { i++; lo = (unsigned char)p[i]; }
        if (p[i + 1] == '-' && p[i + 2] && p[i + 2] != ']') {
            unsigned char hi = (unsigned char)p[i + 2]; if (hi == '\\' && p[i + 3]) { hi = (unsigned char)p[i + 3]; i++; }
            if ((unsigned char)*s >= lo && (unsigned char)*s <= hi) hit = 1;
            i += 3;
        } else { if ((unsigned char)*s == lo) hit = 1; i++; }
    }
    *ok = (hit != neg);
    return (int)i + 1;
}
static int pmatch(const char* p, const char* s, size_t n) {         /* итеративно, без рекурсии */
    size_t si = 0; const char* star_p = 0; size_t star_s = 0;
    for (;;) {
        if (*p == '*') { star_p = ++p; star_s = si; continue; }
        if (si < n && *p) {
            int ok = 0; size_t pl = 1, sl = 1;
            if (*p == '?') { ok = 1; sl = utf8_clen(s + si, n - si); }
            else if (*p == '[') { int l = pm_set(p, s + si, &ok); if (l) pl = (size_t)l; else ok = (s[si] == '['); }
            else if (*p == '\\' && p[1]) { ok = (s[si] == p[1]); pl = 2; }
            else ok = (s[si] == *p);
            if (ok) { p += pl; si += sl; continue; }
        } else if (si >= n && !*p) return 1;
        if (star_p && star_s < n) { p = star_p; star_s += utf8_clen(s + star_s, n - star_s); si = star_s; continue; }
        return 0;
    }
}

/* ---------- арифметика ---------- */
typedef struct { const char* s; size_t i; int err, skip, depth; Io* io; } Ar;
#define AR_ERR(a, ...) do { if (!(a)->err) emsg((a)->io, __VA_ARGS__); (a)->err = 1; } while (0)
static ll ar_assign(Ar* a);
static void ar_ws(Ar* a) { while (a->s[a->i] == ' ' || a->s[a->i] == '\t' || a->s[a->i] == '\n') a->i++; }
static int ar_parse_int(const char* s, ll* out) {                      /* целое: 10-ичное, 0x.., 0.. (8-ричное), со знаком */
    const char* p = s; int neg = 0; while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    int base = 10; if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; } else if (p[0] == '0' && p[1]) base = 8;
    if (!*p) return 0;
    ull v = 0;
    for (; *p; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (d >= base) { while (*p == ' ' || *p == '\t') p++; if (*p) return 0; break; }
        v = v * (ull)base + (ull)d;
    }
    *out = (ll)(neg ? 0 - v : v); return 1;
}
static ll ar_eval_text(Io* io, const char* text, int depth, int* err);
static ll ar_var(Ar* a, const char* name) {
    const char* v = var_get(name); ll r;
    if (!v || !*v) { if (!v && g_opt_u) { AR_ERR(a, "%s: параметр не задан", name); } return 0; }
    if (ar_parse_int(v, &r)) return r;
    if (a->depth > 8) { AR_ERR(a, "арифметика: %s: слишком глубокая подстановка", name); return 0; }
    int e = 0; r = ar_eval_text(a->io, v, a->depth + 1, &e); if (e) a->err = 1; return r;
}
static ll ar_unary(Ar* a);
static ll ar_unary_body(Ar* a) {
    ar_ws(a); const char* s = a->s; char c = s[a->i];
    if (c == '-') { a->i++; return (ll)(0 - (ull)ar_unary(a)); }
    if (c == '+') { a->i++; return ar_unary(a); }
    if (c == '!') { a->i++; return !ar_unary(a); }
    if (c == '~') { a->i++; return ~ar_unary(a); }
    if (c == '(') {
        a->i++; ll r = ar_assign(a); ar_ws(a);
        if (s[a->i] != ')') { AR_ERR(a, "арифметика: ожидалась ')'"); return 0; }
        a->i++; return r;
    }
    if (isdigit((unsigned char)c)) {
        size_t j = a->i; while (isalnum((unsigned char)s[j])) j++;
        char* t = xndup(s + a->i, j - a->i); ll r = 0;
        if (!ar_parse_int(t, &r)) { AR_ERR(a, "арифметика: %s: неверное число", t); }
        free(t); a->i = j; return r;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t j = a->i; while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
        char* t = xndup(s + a->i, j - a->i); a->i = j; ll r = ar_var(a, t); free(t); return r;
    }
    AR_ERR(a, c ? "арифметика: неожиданный символ '%c'" : "арифметика: неожиданный конец выражения", c); return 0;
}
static ll ar_unary(Ar* a) {
    ll r = 0;
    if (REC_ENTER()) { if (!a->err) AR_ERR(a, "арифметика: слишком глубокая вложенность"); }
    else r = ar_unary_body(a);
    REC_LEAVE(); return r;
}
static const struct { const char* op; int prec; } ar_ops[] = {
    {"<<", 8}, {">>", 8}, {"<=", 7}, {">=", 7}, {"==", 6}, {"!=", 6}, {"&&", 2}, {"||", 1},
    {"<", 7}, {">", 7}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}, {"&", 5}, {"|", 3}, {"^", 4} };
static ll ar_bin(Ar* a, int minp) {
    ll l = ar_unary(a);
    for (;;) {
        ar_ws(a); const char* s = a->s + a->i; int k = -1;
        for (size_t q = 0; q < sizeof ar_ops / sizeof ar_ops[0]; q++) { size_t ol = strlen(ar_ops[q].op); if (!strncmp(s, ar_ops[q].op, ol)) { k = (int)q; break; } }
        if (k < 0 || ar_ops[k].prec < minp || a->err) return l;
        const char* op = ar_ops[k].op; size_t ol = strlen(op);
        if (s[ol] == '=' && strcmp(op, "==") && strcmp(op, "!=") && strcmp(op, "<=") && strcmp(op, ">=")) { AR_ERR(a, "арифметика: присваивание не к переменной"); return 0; }
        a->i += ol; int p = ar_ops[k].prec; ll r; ull ul = (ull)l;
        if (!strcmp(op, "&&")) { if (!l) a->skip++; r = ar_bin(a, p + 1); if (!l) a->skip--; l = l && r; continue; }
        if (!strcmp(op, "||")) { if (l) a->skip++; r = ar_bin(a, p + 1); if (l) a->skip--; l = l || r; continue; }
        r = ar_bin(a, p + 1);
        switch (op[0]) {
        case '+': l = (ll)(ul + (ull)r); break;
        case '-': l = (ll)(ul - (ull)r); break;
        case '*': l = (ll)(ul * (ull)r); break;
        case '/': case '%':
            if (!r) { if (!a->skip) { AR_ERR(a, "арифметика: деление на ноль"); } l = 0; }
            else if (r == -1) l = op[0] == '/' ? (ll)(0 - ul) : 0;
            else l = op[0] == '/' ? l / r : l % r;
            break;
        case '<': l = op[1] == '<' ? (ll)(ul << ((ull)r & 63)) : op[1] == '=' ? l <= r : l < r; break;
        case '>': l = op[1] == '>' ? (l >> ((ull)r & 63)) : op[1] == '=' ? l >= r : l > r; break;
        case '=': l = l == r; break;
        case '!': l = l != r; break;
        case '&': l = (ll)(ul & (ull)r); break;
        case '|': l = (ll)(ul | (ull)r); break;
        case '^': l = (ll)(ul ^ (ull)r); break;
        }
    }
}
static ll ar_assign(Ar* a) {
    ar_ws(a); const char* s = a->s; size_t j = a->i;
    if (isalpha((unsigned char)s[j]) || s[j] == '_') {
        while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
        size_t ne = j; while (s[j] == ' ' || s[j] == '\t') j++;
        static const char* aops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "=" };
        for (size_t q = 0; q < sizeof aops / sizeof aops[0]; q++) {
            size_t ol = strlen(aops[q]);
            if (strncmp(s + j, aops[q], ol) || (q == 10 && s[j + 1] == '=')) continue;
            char* name = xndup(s + a->i, ne - a->i); a->i = j + ol;
            ll r = ar_assign(a), cur = aops[q][0] == '=' ? 0 : ar_var(a, name); ull uc = (ull)cur;
            switch (aops[q][0]) {
            case '+': r = (ll)(uc + (ull)r); break;
            case '-': r = (ll)(uc - (ull)r); break;
            case '*': r = (ll)(uc * (ull)r); break;
            case '/': case '%': if (!r) { if (!a->skip) { AR_ERR(a, "арифметика: деление на ноль"); } r = 0; } else if (r == -1) r = aops[q][0] == '/' ? (ll)(0 - uc) : 0; else r = aops[q][0] == '/' ? cur / r : cur % r; break;
            case '<': r = (ll)(uc << ((ull)r & 63)); break;
            case '>': r = cur >> ((ull)r & 63); break;
            case '&': r = (ll)(uc & (ull)r); break;
            case '|': r = (ll)(uc | (ull)r); break;
            case '^': r = (ll)(uc ^ (ull)r); break;
            }
            if (!a->skip && !a->err) { char nb[32]; snprintf(nb, sizeof nb, "%lld", r); var_set(name, nb); }
            free(name); return r;
        }
    }
    ll c = ar_bin(a, 1); ar_ws(a);
    if (s[a->i] == '?') {
        a->i++; if (!c) a->skip++; ll x = ar_assign(a); if (!c) a->skip--; ar_ws(a);
        if (s[a->i] != ':') { AR_ERR(a, "арифметика: ожидалось ':'"); return 0; }
        a->i++; if (c) a->skip++; ll y = ar_assign(a); if (c) a->skip--;
        return c ? x : y;
    }
    return c;
}
static ll ar_eval_text(Io* io, const char* text, int depth, int* err) {
    Ar a = { text, 0, 0, 0, depth, io };
    ll r = ar_assign(&a); ar_ws(&a);
    if (!a.err && text[a.i]) { emsg(io, "арифметика: лишние символы: %s", text + a.i); a.err = 1; }
    *err = a.err; return r;
}

/* ---------- лексер ---------- */
enum { T_EOF, T_WORD, T_NL, T_SEMI, T_DSEMI, T_ANDIF, T_ORIF, T_PIPE, T_LP, T_RP, T_LT, T_GT, T_DGT, T_GTAMP, T_HERE, T_HERET, T_AMP };
typedef struct { int t, line, fd, hdq; char* s; char* hd; } Tok;
typedef struct { Tok* v; int n, cap; } Toks;
static Tok* tk_add(Toks* T, int t, int line, int fd, char* s) {
    if (T->n == T->cap) { T->cap = T->cap ? T->cap * 2 : 64; T->v = xr(T->v, (size_t)T->cap * sizeof(Tok)); }
    Tok* k = &T->v[T->n++]; k->t = t; k->line = line; k->fd = fd; k->s = s; k->hd = 0; k->hdq = 0; return k;
}
static void toks_free(Toks* T) { for (int i = 0; i < T->n; i++) { free(T->v[i].s); free(T->v[i].hd); } free(T->v); T->v = 0; T->n = T->cap = 0; }

/* пропуск конструкций; результат - индекс после, -1 не закрыто (продолжение ввода), -2 слишком глубоко */
static long skip_sq(const char* s, size_t n, size_t i) { for (i++; i < n; i++) if (s[i] == '\'') return (long)i + 1; return -1; }
static long skip_bt(const char* s, size_t n, size_t i) { for (i++; i < n; ) { if (s[i] == '\\') i += 2; else if (s[i] == '`') return (long)i + 1; else i++; } return -1; }
static long skip_dollar(const char* s, size_t n, size_t i);
static long skip_dq(const char* s, size_t n, size_t i);
static long skip_dq_body(const char* s, size_t n, size_t i) {
    for (i++; i < n; ) {
        if (s[i] == '\\') i += 2;
        else if (s[i] == '"') return (long)i + 1;
        else if (s[i] == '`') { long j = skip_bt(s, n, i); if (j < 0) return j; i = (size_t)j; }
        else if (s[i] == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) { long j = skip_dollar(s, n, i); if (j < 0) return j; i = (size_t)j; }
        else i++;
    }
    return -1;
}
static long skip_dq(const char* s, size_t n, size_t i) { long r = REC_ENTER() ? -2 : skip_dq_body(s, n, i); REC_LEAVE(); return r; }
static long skip_group_body(const char* s, size_t n, size_t i, char open, char close) {   /* i - после открывающей скобки */
    int depth = 1;
    while (i < n) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        long j = i;
        if (c == '\'') j = skip_sq(s, n, i);
        else if (c == '"') j = skip_dq(s, n, i);
        else if (c == '`') j = skip_bt(s, n, i);
        else if (c == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{') && (s[i + 1] != open)) j = skip_dollar(s, n, i);
        else {
            if (c == open) depth++; else if (c == close && --depth == 0) return (long)i + 1;
            i++; continue;
        }
        if (j < 0) return j;
        i = (size_t)j;
    }
    return -1;
}
static long skip_group(const char* s, size_t n, size_t i, char open, char close) {
    long r = REC_ENTER() ? -2 : skip_group_body(s, n, i, open, close); REC_LEAVE(); return r;
}
static long skip_dollar(const char* s, size_t n, size_t i) {          /* s[i] == '$' */
    if (i + 1 < n && s[i + 1] == '(') return skip_group(s, n, i + 2, '(', ')');
    if (i + 1 < n && s[i + 1] == '{') return skip_group(s, n, i + 2, '{', '}');
    return (long)i + 1;
}
static long word_end(const char* s, size_t n, size_t i) {
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>') break;
        if (c == '\\') { i += i + 1 < n ? 2 : 1; continue; }
        long j;
        if (c == '\'') j = skip_sq(s, n, i);
        else if (c == '"') j = skip_dq(s, n, i);
        else if (c == '`') j = skip_bt(s, n, i);
        else if (c == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) j = skip_dollar(s, n, i);
        else { i++; continue; }
        if (j < 0) return j;
        i = (size_t)j;
    }
    return (long)i;
}
static char* unquote_word(const char* s, size_t n, int* quoted) {
    Buf b = {0}; *quoted = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { *quoted = 1; b_c(&b, s[++i]); }
        else if (s[i] == '\'') { *quoted = 1; for (i++; i < n && s[i] != '\''; i++) b_c(&b, s[i]); }
        else if (s[i] == '"') { *quoted = 1; for (i++; i < n && s[i] != '"'; i++) { if (s[i] == '\\' && i + 1 < n && strchr("$`\"\\", s[i + 1])) i++; b_c(&b, s[i]); } }
        else b_c(&b, s[i]);
    }
    return b_take(&b);
}
typedef struct { int tok, strip, quoted; char* delim; } Pend;
/* 0 - готово, 1 - ввод не закончен, 2 - ошибка (сообщение в err) */
static int lex(const char* s, Toks* T, char* err, size_t errn) {
    size_t n = strlen(s), i = 0; int line = 1; Pend pend[8]; int np = 0, rc = 0;
    while (!rc) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n'))) { if (s[i] == '\\') { i++; line++; } i++; }
        if (i < n && s[i] == '#') { while (i < n && s[i] != '\n') i++; }
        if (i >= n) break;
        char c = s[i]; int fd = -1;
        if (c == '\n') {
            tk_add(T, T_NL, line, -1, 0); i++; line++;
            for (int k = 0; k < np && !rc; k++) {
                Buf body = {0}; int found = 0;
                while (i < n && !found) {
                    size_t e = i; while (e < n && s[e] != '\n') e++;
                    size_t b0 = i; if (pend[k].strip) while (b0 < e && s[b0] == '\t') b0++;
                    if (e - b0 == strlen(pend[k].delim) && !strncmp(s + b0, pend[k].delim, e - b0)) found = 1;
                    else { b_put(&body, s + b0, e - b0); b_c(&body, '\n'); }
                    i = e < n ? e + 1 : e; line++;
                }
                if (!found) { rc = 1; snprintf(err, errn, "документ-вставка не закрыт ограничителем %s", pend[k].delim); b_free(&body); }
                else { T->v[pend[k].tok].hd = b_take(&body); T->v[pend[k].tok].hdq = pend[k].quoted; }
            }
            for (int k = 0; k < np; k++) free(pend[k].delim);
            np = 0; continue;
        }
        size_t j = i;
        while (isdigit((unsigned char)s[j])) j++;
        if (j > i && (s[j] == '<' || s[j] == '>')) { fd = atoi(s + i); i = j; c = s[i]; }
        if (c == ';') { if (s[i + 1] == ';') { tk_add(T, T_DSEMI, line, -1, 0); i += 2; } else { tk_add(T, T_SEMI, line, -1, 0); i++; } }
        else if (c == '&') { if (s[i + 1] == '&') { tk_add(T, T_ANDIF, line, -1, 0); i += 2; } else { tk_add(T, T_AMP, line, -1, 0); i++; } }
        else if (c == '|') { if (s[i + 1] == '|') { tk_add(T, T_ORIF, line, -1, 0); i += 2; } else { tk_add(T, T_PIPE, line, -1, 0); i++; } }
        else if (c == '(') { tk_add(T, T_LP, line, -1, 0); i++; }
        else if (c == ')') { tk_add(T, T_RP, line, -1, 0); i++; }
        else if (c == '>') {
            if (s[i + 1] == '>') { tk_add(T, T_DGT, line, fd, 0); i += 2; }
            else if (s[i + 1] == '&') { tk_add(T, T_GTAMP, line, fd, 0); i += 2; }
            else if (s[i + 1] == '|') { tk_add(T, T_GT, line, fd, 0); i += 2; }
            else { tk_add(T, T_GT, line, fd, 0); i++; }
        }
        else if (c == '<') {
            if (s[i + 1] == '<') {
                int strip = s[i + 2] == '-'; int ti = T->n; tk_add(T, strip ? T_HERET : T_HERE, line, fd, 0); i += strip ? 3 : 2;
                while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
                long e = word_end(s, n, i);
                if (e < 0) { rc = e == -1 ? 1 : 2; snprintf(err, errn, e == -1 ? "ограничитель документа-вставки не закрыт" : "слишком глубокая вложенность"); break; }
                if ((size_t)e == i) { rc = 2; snprintf(err, errn, "после << нужен ограничитель"); break; }
                if (np == 8) { rc = 2; snprintf(err, errn, "слишком много документов-вставок в одной строке"); break; }
                pend[np].tok = ti; pend[np].strip = strip; pend[np].delim = unquote_word(s + i, (size_t)e - i, &pend[np].quoted); np++;
                i = (size_t)e;
            } else if (s[i + 1] == '&' || s[i + 1] == '>') { rc = 2; snprintf(err, errn, "перенаправление <%c не поддерживается", s[i + 1]); }
            else { tk_add(T, T_LT, line, fd, 0); i++; }
        }
        else {
            long e = word_end(s, n, i);
            if (e < 0) { rc = e == -1 ? 1 : 2; snprintf(err, errn, e == -1 ? "не закрыта кавычка или скобка" : "слишком глубокая вложенность"); break; }
            if ((size_t)e == i) { rc = 2; snprintf(err, errn, "неожиданный символ '%c'", c); break; }
            tk_add(T, T_WORD, line, -1, xndup(s + i, (size_t)e - i));
            for (size_t k = i; k < (size_t)e; k++) if (s[k] == '\n') line++;
            i = (size_t)e;
        }
    }
    if (!rc && np) { rc = 1; snprintf(err, errn, "документ-вставка не закрыт ограничителем %s", pend[0].delim); }
    for (int k = 0; k < np; k++) free(pend[k].delim);
    tk_add(T, T_EOF, line, -1, 0);
    return rc;
}

/* ---------- разбор ---------- */
typedef struct { Tok* t; int n, pos, err; char msg[160]; } P;
static Tok* pk(P* p) { return &p->t[p->pos]; }
static int is_w(Tok* t, const char* k) { return t->t == T_WORD && !strcmp(t->s, k); }
static const char* tok_text(Tok* t) {
    static const char* nm[] = { "конец ввода", "", "перевод строки", ";", ";;", "&&", "||", "|", "(", ")", "<", ">", ">>", ">&", "<<", "<<-", "&" };
    return t->t == T_WORD ? t->s : nm[t->t];
}
static void perr(P* p, int kind, const char* fmt, ...) {
    if (p->err) return;
    va_list ap; va_start(ap, fmt); vsnprintf(p->msg, sizeof p->msg, fmt, ap); va_end(ap); p->err = kind;
}
static void fail_expect(P* p, const char* what) {
    Tok* t = pk(p);
    if (t->t == T_EOF) perr(p, 1, "строка %d: неожиданный конец, ожидалось %s", t->line, what);
    else perr(p, 2, "строка %d: ожидалось %s, найдено «%s»", t->line, what, tok_text(t));
}
static void fail_unexpected(P* p) {
    Tok* t = pk(p);
    if (t->t == T_EOF) perr(p, 1, "строка %d: неожиданный конец ввода", t->line);
    else perr(p, 2, "строка %d: синтаксическая ошибка около «%s»", t->line, tok_text(t));
}
static int at_list_end(P* p) {
    Tok* t = pk(p);
    if (t->t == T_EOF || t->t == T_RP || t->t == T_DSEMI) return 1;
    if (t->t != T_WORD) return 0;
    static const char* kw[] = { "then", "elif", "else", "fi", "do", "done", "esac", "}" };
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++) if (!strcmp(t->s, kw[i])) return 1;
    return 0;
}
static int is_redir_tok(int t) { return t == T_LT || t == T_GT || t == T_DGT || t == T_GTAMP || t == T_HERE || t == T_HERET; }
static Node* parse_andor(P* p);
static Node* parse_list(P* p);
static Node* parse_command(P* p);
static Node* parse_body(P* p) { Node* l = parse_list(p); if (l && !l->nl) { fail_unexpected(p); free_node(l); return 0; } return l; }
static int expect_kw(P* p, const char* k) { if (is_w(pk(p), k)) { p->pos++; return 1; } fail_expect(p, k); return 0; }
static void skip_nl(P* p) { while (pk(p)->t == T_NL) p->pos++; }

static int parse_redir(P* p, Node* n) {
    Tok* t = pk(p); p->pos++;
    Redir r; memset(&r, 0, sizeof r);
    if (t->t == T_HERE || t->t == T_HERET) { r.op = R_HERE; r.fd = t->fd < 0 ? 0 : t->fd; r.hd = t->hd; t->hd = 0; r.hdq = t->hdq; if (!r.hd) r.hd = xdup(""); }
    else {
        Tok* w = pk(p);
        if (w->t != T_WORD) { fail_expect(p, "имя файла после перенаправления"); return 0; }
        p->pos++; r.word = xdup(w->s);
        if (t->t == T_LT) { r.op = R_IN; r.fd = t->fd < 0 ? 0 : t->fd; }
        else { r.op = t->t == T_DGT ? R_APP : t->t == T_GTAMP ? R_DUP : R_OUT; r.fd = t->fd < 0 ? 1 : t->fd; }
    }
    n->r = xr(n->r, (size_t)(n->nr + 1) * sizeof(Redir)); n->r[n->nr++] = r; return 1;
}
static int parse_redirs(P* p, Node* n) { while (is_redir_tok(pk(p)->t)) if (!parse_redir(p, n)) return 0; return 1; }
static Node* parse_simple(P* p) {
    Node* n = nn(N_SIMPLE);
    for (;;) {
        Tok* t = pk(p);
        if (t->t == T_WORD) { w_push(n, xdup(t->s)); p->pos++; }
        else if (is_redir_tok(t->t)) { if (!parse_redir(p, n)) { free_node(n); return 0; } }
        else break;
    }
    if (!n->nw && !n->nr) { fail_unexpected(p); free_node(n); return 0; }
    return n;
}
static Node* parse_if(P* p) {
    Node* head = 0; Node** link = &head;
    do {
        p->pos++;
        Node* n = nn(N_IF); *link = n; link = &n->c;
        if (!(n->a = parse_body(p)) || !expect_kw(p, "then") || !(n->b = parse_body(p))) { free_node(head); return 0; }
    } while (is_w(pk(p), "elif"));
    if (is_w(pk(p), "else")) { p->pos++; if (!(*link = parse_body(p))) { free_node(head); return 0; } }
    if (!expect_kw(p, "fi")) { free_node(head); return 0; }
    return head;
}
static Node* parse_while(P* p) {
    Node* n = nn(N_WHILE); n->flag = is_w(pk(p), "until"); p->pos++;
    if (!(n->a = parse_body(p)) || !expect_kw(p, "do") || !(n->b = parse_body(p)) || !expect_kw(p, "done")) { free_node(n); return 0; }
    return n;
}
static Node* parse_for(P* p) {
    Node* n = nn(N_FOR); p->pos++;
    Tok* t = pk(p);
    if (t->t != T_WORD || !is_name(t->s, strlen(t->s))) { fail_expect(p, "имя переменной"); free_node(n); return 0; }
    n->name = xdup(t->s); p->pos++; skip_nl(p);
    if (is_w(pk(p), "in")) {
        p->pos++; n->flag = 1;
        while (pk(p)->t == T_WORD) { w_push(n, xdup(pk(p)->s)); p->pos++; }
        if (pk(p)->t != T_SEMI && pk(p)->t != T_NL) { fail_expect(p, "';' или перевод строки"); free_node(n); return 0; }
    } else if (pk(p)->t == T_SEMI) { /* for x; do */ }
    else if (!is_w(pk(p), "do")) { fail_expect(p, "in или do"); free_node(n); return 0; }
    while (pk(p)->t == T_SEMI || pk(p)->t == T_NL) p->pos++;
    if (!expect_kw(p, "do") || !(n->a = parse_body(p)) || !expect_kw(p, "done")) { free_node(n); return 0; }
    return n;
}
static Node* parse_case(P* p) {
    Node* n = nn(N_CASE); p->pos++;
    Tok* t = pk(p);
    if (t->t != T_WORD) { fail_expect(p, "слово после case"); free_node(n); return 0; }
    w_push(n, xdup(t->s)); p->pos++; skip_nl(p);
    if (!expect_kw(p, "in")) { free_node(n); return 0; }
    for (;;) {
        skip_nl(p);
        if (is_w(pk(p), "esac")) break;
        CaseItem it; memset(&it, 0, sizeof it);
        if (pk(p)->t == T_LP) p->pos++;
        for (;;) {
            if (pk(p)->t != T_WORD) { fail_expect(p, "образец"); free(it.pats); free_node(n); return 0; }
            it.pats = xr(it.pats, (size_t)(it.np + 1) * sizeof(char*)); it.pats[it.np++] = xdup(pk(p)->s); p->pos++;
            if (pk(p)->t == T_PIPE) { p->pos++; continue; }
            break;
        }
        n->ci = xr(n->ci, (size_t)(n->nci + 1) * sizeof(CaseItem)); n->ci[n->nci++] = it;   /* дальше за него отвечает n */
        CaseItem* cur = &n->ci[n->nci - 1];
        if (pk(p)->t != T_RP) { fail_expect(p, "')' после образца"); free_node(n); return 0; }
        p->pos++;
        if (!(cur->body = parse_list(p))) { free_node(n); return 0; }
        if (pk(p)->t == T_DSEMI) p->pos++;
        else if (!is_w(pk(p), "esac")) { fail_expect(p, "';;' или esac"); free_node(n); return 0; }
    }
    p->pos++; return n;
}
static Node* parse_command_body(P* p) {
    Tok* t = pk(p); Node* n = 0;
    if (t->t == T_LP) {
        p->pos++; n = nn(N_SUB);
        if (!(n->a = parse_body(p))) { free_node(n); return 0; }
        if (pk(p)->t != T_RP) { fail_expect(p, "')'"); free_node(n); return 0; }
        p->pos++;
    } else if (t->t == T_WORD) {
        Tok* t1 = &p->t[p->pos + 1];
        if (is_w(t, "if")) n = parse_if(p);
        else if (is_w(t, "while") || is_w(t, "until")) n = parse_while(p);
        else if (is_w(t, "for")) n = parse_for(p);
        else if (is_w(t, "case")) n = parse_case(p);
        else if (is_w(t, "{")) {
            p->pos++; n = nn(N_GROUP);
            if (!(n->a = parse_body(p)) || !expect_kw(p, "}")) { free_node(n); return 0; }
        } else if (is_w(t, "function") || (t1->t == T_LP && p->t[p->pos + 2].t == T_RP)) {
            int kw = is_w(t, "function"); if (kw) { p->pos++; t = pk(p); }
            if (t->t != T_WORD || !is_name(t->s, strlen(t->s))) { fail_expect(p, "имя функции"); return 0; }
            n = nn(N_FUNC); n->name = xdup(t->s); p->pos++;
            if (pk(p)->t == T_LP) { p->pos++; if (pk(p)->t != T_RP) { fail_expect(p, "')'"); free_node(n); return 0; } p->pos++; }
            else if (!kw) { fail_expect(p, "'()'"); free_node(n); return 0; }
            skip_nl(p);
            if (!(n->a = parse_command(p))) { free_node(n); return 0; }
            return n;
        } else if (at_list_end(p)) { fail_unexpected(p); return 0; }
        else return parse_simple(p);
        if (!n) return 0;
    } else if (is_redir_tok(t->t)) return parse_simple(p);
    else { fail_unexpected(p); return 0; }
    if (!parse_redirs(p, n)) { free_node(n); return 0; }
    return n;
}
static Node* parse_command(P* p) {
    Node* r = 0;
    if (REC_ENTER()) perr(p, 2, "слишком глубокая вложенность");
    else r = parse_command_body(p);
    REC_LEAVE(); return r;
}
static Node* parse_pipeline(P* p) {
    int neg = 0;
    if (is_w(pk(p), "!")) { neg = 1; p->pos++; }
    Node* c = parse_command(p); if (!c) return 0;
    if (pk(p)->t == T_PIPE) {
        Node* pl = nn(N_PIPE); nl_push(pl, c);
        while (pk(p)->t == T_PIPE) {
            p->pos++; skip_nl(p);
            Node* d = parse_command(p); if (!d) { free_node(pl); return 0; }
            nl_push(pl, d);
        }
        c = pl;
    }
    if (neg) { Node* x = nn(N_NOT); x->a = c; c = x; }
    return c;
}
static Node* parse_andor(P* p) {
    Node* l = parse_pipeline(p); if (!l) return 0;
    while (pk(p)->t == T_ANDIF || pk(p)->t == T_ORIF) {
        int op = pk(p)->t == T_ANDIF ? N_AND : N_OR; p->pos++; skip_nl(p);
        Node* r = parse_pipeline(p); if (!r) { free_node(l); return 0; }
        Node* x = nn(op); x->a = l; x->b = r; l = x;
    }
    return l;
}
static Node* parse_list(P* p) {
    Node* L = nn(N_LIST);
    for (;;) {
        skip_nl(p);
        if (at_list_end(p)) break;
        Node* c = parse_andor(p); if (!c) { free_node(L); return 0; }
        nl_push(L, c);
        Tok* t = pk(p);
        if (t->t == T_SEMI || t->t == T_NL) { p->pos++; continue; }
        if (t->t == T_AMP) { perr(p, 2, "строка %d: фоновый запуск (&) не поддерживается", t->line); free_node(L); return 0; }
        if (at_list_end(p)) break;
        fail_unexpected(p); free_node(L); return 0;
    }
    return L;
}

/* ---------- раскрытие слов ---------- */
enum { M_FIELDS, M_STR, M_PAT };
typedef struct { Io* io; int mode, hd, started, atempty, inword; Buf cs, cf; Argv fs, ff; } Exp;
static void run_code(const char* text, Io* io);
static const char* ifs_get(void) { const char* v = var_get("IFS"); return v ? v : " \t\n"; }
static void e_char(Exp* e, char c, int q) { b_c(&e->cs, c); b_c(&e->cf, q ? '1' : '0'); e->started = 1; }
static void e_break(Exp* e) { if (e->started) { av_push(&e->fs, b_take(&e->cs)); av_push(&e->ff, b_take(&e->cf)); e->started = 0; } }
static void e_text(Exp* e, const char* t, size_t n, int q) {
    if (q || e->mode != M_FIELDS) { for (size_t i = 0; i < n; i++) e_char(e, t[i], q); return; }
    const char* ifs = ifs_get();
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c && strchr(ifs, c)) { if (c == ' ' || c == '\t' || c == '\n') e_break(e); else { e->started = 1; e_break(e); } }
        else e_char(e, c, 0);
    }
}
/* значение параметра; NULL - не задан. Возвращает копию. */
static char* pv(const char* name) {
    char b[32];
    if (!strcmp(name, "?")) { snprintf(b, sizeof b, "%d", g_st); return xdup(b); }
    if (!strcmp(name, "#")) { snprintf(b, sizeof b, "%d", g_pn); return xdup(b); }
    if (!strcmp(name, "$")) return xdup("1");
    if (!strcmp(name, "0")) return xdup(g_name ? g_name : "ksh");
    if (isdigit((unsigned char)name[0])) { long k = strtol(name, 0, 10); return k >= 1 && k <= g_pn ? xdup(g_pa[k - 1]) : 0; }
    const char* v = var_get(name); return v ? xdup(v) : 0;
}
static void exp_at(Exp* e, int quoted, int star) {
    if (!g_pn) { e->atempty = 1; return; }
    int join = star || e->mode != M_FIELDS || e->hd;
    if (quoted && join) {
        const char* ifs = var_get("IFS"); char sep = (star && ifs) ? ifs[0] : ' ';
        for (int i = 0; i < g_pn; i++) { if (i && sep) e_char(e, sep, 1); e_text(e, g_pa[i], strlen(g_pa[i]), 1); }
        e->started = 1;
    } else if (quoted) {
        for (int i = 0; i < g_pn; i++) { if (i) e_break(e); e_text(e, g_pa[i], strlen(g_pa[i]), 1); e->started = 1; }
    } else {
        for (int i = 0; i < g_pn; i++) { if (i) e_break(e); e_text(e, g_pa[i], strlen(g_pa[i]), 0); }
    }
}
static void cmdsub(Io* io, const char* code, size_t n, Buf* out);
static char* expand_str(Io* io, const char* raw);
static char* expand_pat(Io* io, const char* raw);
static void exp_run(Exp* e, const char* s, size_t n, int dq);
static void unset_err(Exp* e, const char* name) { emsg(e->io, "%s: параметр не задан", name); g_experr = 1; g_ctl = C_EXIT; g_st = 1; }
static void exp_brace(Exp* e, const char* c, size_t n, int dq) {
    char* name; size_t nl = 0; int lenop = 0;
    const char* p = c;
    if (n > 1 && p[0] == '#') { lenop = 1; p++; n--; }
    if (n && (isalpha((unsigned char)p[0]) || p[0] == '_')) while (nl < n && (isalnum((unsigned char)p[nl]) || p[nl] == '_')) nl++;
    else if (n && isdigit((unsigned char)p[0])) while (nl < n && isdigit((unsigned char)p[nl])) nl++;
    else if (n && strchr("?#@*$", p[0])) nl = 1;
    if (!nl) { emsg(e->io, "неверная подстановка ${%.*s}", (int)n, p); g_experr = 1; return; }
    name = xndup(p, nl); const char* rest = p + nl; size_t rl = n - nl;
    int star = name[0] == '*' || name[0] == '@';
    if (lenop) {
        char* v = star ? 0 : pv(name); char b[32];
        snprintf(b, sizeof b, "%zu", star ? (size_t)g_pn : v ? utf8_count(v, strlen(v)) : 0);
        e_text(e, b, strlen(b), dq); free(v);
        if (rl) { emsg(e->io, "неверная подстановка ${#%s}", name); g_experr = 1; }
    } else if (!rl) {
        if (star) exp_at(e, dq || e->hd || e->mode != M_FIELDS, name[0] == '*');
        else { char* v = pv(name); if (v) e_text(e, v, strlen(v), dq); else if (g_opt_u) unset_err(e, name); free(v); }
    } else {
        char* v = pv(name); int colon = 0; const char* o = rest;
        if (*o == ':') { colon = 1; o++; }
        char k = *o; const char* w = o + 1; size_t wl = rl - (size_t)(w - rest);
        int test = !v || (colon && !*v);
        if (k == '-' || k == '=' || k == '+' || k == '?') {
            if (k == '-') { if (test) { e->inword++; exp_run(e, w, wl, dq); e->inword--; } else e_text(e, v, strlen(v), dq); }
            else if (k == '+') { if (!test) { e->inword++; exp_run(e, w, wl, dq); e->inword--; } }
            else if (k == '=') {
                if (test) {
                    char* ws = xndup(w, wl); char* x = expand_str(e->io, ws); free(ws);
                    if (isdigit((unsigned char)name[0]) || strchr("?#@*$", name[0])) { emsg(e->io, "%s: так присвоить нельзя", name); g_experr = 1; }
                    else var_set(name, x);
                    e_text(e, x, strlen(x), dq); free(x);
                } else e_text(e, v, strlen(v), dq);
            } else {
                if (test) {
                    char* ws = xndup(w, wl); char* x = expand_str(e->io, ws); free(ws);
                    emsg(e->io, "%s: %s", name, *x ? x : "параметр не задан"); free(x);
                    g_ctl = C_EXIT; g_ctln = 0; g_st = 1; g_experr = 1;
                } else e_text(e, v, strlen(v), dq);
            }
        } else if ((k == '#' || k == '%') && !colon) {
            int dbl = w[0] == k; if (dbl) { w++; wl--; }
            char* ws = xndup(w, wl); char* pat = expand_pat(e->io, ws); free(ws);
            const char* s = v ? v : ""; size_t sl = strlen(s), cut_a = 0, cut_b = sl;
            if (k == '#') { if (dbl) { for (size_t L = sl + 1; L-- > 0; ) if (pmatch(pat, s, L)) { cut_a = L; break; } } else { for (size_t L = 0; L <= sl; L++) if (pmatch(pat, s, L)) { cut_a = L; break; } } }
            else { if (dbl) { for (size_t L = 0; L <= sl; L++) if (pmatch(pat, s + L, sl - L)) { cut_b = L; break; } } else { for (size_t L = sl + 1; L-- > 0; ) if (pmatch(pat, s + L, sl - L)) { cut_b = L; break; } } }
            e_text(e, s + cut_a, cut_b - cut_a, dq); free(pat);
        } else { emsg(e->io, "неверная подстановка ${%.*s}", (int)n, p); g_experr = 1; }
        free(v);
    }
    free(name);
}
static void exp_dollar(Exp* e, const char* s, size_t n, size_t* pi, int dq) {
    size_t i = *pi; char c = i + 1 < n ? s[i + 1] : 0;
    int q = dq || e->hd;
    if (c == '(' || c == '{') {
        long j = skip_dollar(s, n, i);
        if (j == -2) { emsg(e->io, "слишком глубокая вложенность (рекурсия)"); g_experr = 1; g_ctl = C_EXIT; g_st = 1; *pi = n; return; }
        if (j < 0) { e_char(e, '$', q); *pi = i + 1; return; }
        if (c == '{') exp_brace(e, s + i + 2, (size_t)j - i - 3, dq);
        else if (i + 2 < n && s[i + 2] == '(' && j - 2 > (long)i + 2 && s[j - 2] == ')') {
            char* in = xndup(s + i + 3, (size_t)j - i - 5); char* x = expand_str(e->io, in); free(in);
            int err = 0; ll r = ar_eval_text(e->io, x, 0, &err); free(x);
            if (err) { g_experr = 1; r = 0; }
            char b[32]; snprintf(b, sizeof b, "%lld", r); e_text(e, b, strlen(b), q);
        } else {
            Buf out = {0}; cmdsub(e->io, s + i + 2, (size_t)j - i - 3, &out);
            e_text(e, out.p ? out.p : "", out.n, q); b_free(&out);
        }
        *pi = (size_t)j; return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t j = i + 1; while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
        char* name = xndup(s + i + 1, j - i - 1); char* v = pv(name);
        if (v) e_text(e, v, strlen(v), q); else if (g_opt_u) unset_err(e, name);
        free(v); free(name); *pi = j; return;
    }
    if (isdigit((unsigned char)c) || c == '?' || c == '#' || c == '$') {
        char nm[2] = { c, 0 }; char* v = pv(nm); if (v) e_text(e, v, strlen(v), q); else if (g_opt_u) unset_err(e, nm);
        free(v); *pi = i + 2; return;
    }
    if (c == '@' || c == '*') { exp_at(e, q || e->mode != M_FIELDS, c == '*'); *pi = i + 2; return; }
    e_char(e, '$', q); *pi = i + 1;
}
static void exp_run_body(Exp* e, const char* s, size_t n, int dq) {
    size_t i = 0, mark = 0; int was = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            if (i + 1 >= n) { e_char(e, '\\', 1); i++; continue; }
            char d = s[i + 1];
            if (d == '\n') { i += 2; continue; }
            if (dq || e->hd) { if (strchr("$`\"\\", d)) { e_char(e, d, 1); i += 2; } else { e_char(e, '\\', 1); i++; } }
            else { e_char(e, d, 1); i += 2; }
        } else if (c == '\'' && !dq && !e->hd) {
            long j = skip_sq(s, n, i);
            if (j < 0) { e_char(e, c, 0); i++; continue; }
            for (size_t k = i + 1; k + 1 < (size_t)j; k++) e_char(e, s[k], 1);
            e->started = 1; i = (size_t)j;
        } else if (c == '"' && !e->hd) {
            if (!dq) { dq = 1; was = e->started; mark = e->cs.n; e->atempty = 0; e->started = 1; }
            else { dq = 0; if (e->atempty && e->cs.n == mark) e->started = was; }
            i++;
        } else if (c == '$') exp_dollar(e, s, n, &i, dq);
        else if (c == '`' && !e->hd) {
            long j = skip_bt(s, n, i);
            if (j < 0) { e_char(e, c, dq); i++; continue; }
            Buf code = {0};
            for (size_t k = i + 1; k + 1 < (size_t)j; k++) { if (s[k] == '\\' && k + 2 < (size_t)j && strchr("`\\$", s[k + 1])) k++; b_c(&code, s[k]); }
            Buf out = {0}; cmdsub(e->io, code.p ? code.p : "", code.n, &out);
            e_text(e, out.p ? out.p : "", out.n, dq); b_free(&out); b_free(&code); i = (size_t)j;
        } else {
            if (!dq && !e->hd && e->inword && e->mode == M_FIELDS) e_text(e, &c, 1, 0); else e_char(e, c, dq || e->hd);
            i++;
        }
    }
}
static void exp_run(Exp* e, const char* s, size_t n, int dq) {
    if (REC_ENTER()) { emsg(e->io, "слишком глубокая вложенность (рекурсия)"); g_experr = 1; g_ctl = C_EXIT; g_st = 1; }
    else exp_run_body(e, s, n, dq);
    REC_LEAVE();
}
static int has_glob(const char* s, const char* f) { for (size_t i = 0; s[i]; i++) if (f[i] == '0' && (s[i] == '*' || s[i] == '?' || s[i] == '[')) return 1; return 0; }
static char* build_pat(const char* s, const char* f) {
    Buf b = {0};
    for (size_t i = 0; s[i]; i++) { if (f[i] == '1' && strchr("*?[]\\!^-", s[i])) b_c(&b, '\\'); b_c(&b, s[i]); }
    return b_take(&b);
}
static void exp_finish(Exp* e, Argv* out) {
    e_break(e);
    for (int i = 0; i < e->fs.n; i++) {
        if (has_glob(e->fs.v[i], e->ff.v[i]) && !strchr(e->fs.v[i], '/')) {
            char* pat = build_pat(e->fs.v[i], e->ff.v[i]); Argv names = {0}; int hit = 0;
            if (!sandbox_list(&names)) for (int k = 0; k < names.n; k++)
                if ((names.v[k][0] != '.' || pat[0] == '.') && pmatch(pat, names.v[k], strlen(names.v[k]))) { av_push(out, xdup(names.v[k])); hit++; }
            av_free(&names); free(pat);
            if (hit) { free(e->fs.v[i]); e->fs.v[i] = 0; continue; }
        }
        av_push(out, e->fs.v[i]); e->fs.v[i] = 0;
    }
}
static void exp_free(Exp* e) { av_free(&e->fs); av_free(&e->ff); b_free(&e->cs); b_free(&e->cf); }
static void expand_fields(Io* io, const char* raw, Argv* out) {
    Exp e; memset(&e, 0, sizeof e); e.io = io; e.mode = M_FIELDS;
    exp_run(&e, raw, strlen(raw), 0); exp_finish(&e, out); exp_free(&e);
}
static char* expand_mode(Io* io, const char* raw, int mode, int hd) {
    Exp e; memset(&e, 0, sizeof e); e.io = io; e.mode = mode; e.hd = hd;
    exp_run(&e, raw, strlen(raw), 0);
    char* r;
    if (mode == M_PAT) { Buf b = {0}; for (size_t i = 0; i < e.cs.n; i++) { if (e.cf.p[i] == '1' && strchr("*?[]\\", e.cs.p[i])) b_c(&b, '\\'); b_c(&b, e.cs.p[i]); } r = b_take(&b); }
    else r = b_take(&e.cs);
    exp_free(&e); return r;
}
static char* expand_str(Io* io, const char* raw) { return expand_mode(io, raw, M_STR, 0); }
static char* expand_pat(Io* io, const char* raw) { return expand_mode(io, raw, M_PAT, 0); }
static char* expand_hd(Io* io, const char* raw) { return expand_mode(io, raw, M_STR, 1); }

/* ---------- снимок состояния для ( ) и $( ) ----------
   Снимок забирает исходные массивы себе, а оболочке отдаёт их копии: указатели, которые держат вызывающие (позиционные параметры функции и т.п.),
   остаются верными, а по возвращении копии выбрасываются и исходные возвращаются на место. */
typedef struct { Var* v; int nv, cv; char** pa; int pn; Fn* f; int nf; int opt_e, opt_x, opt_u; } Snap;
static void snap_take(Snap* s) {
    s->v = g_vars; s->nv = g_nv; s->cv = g_cv; s->pa = g_pa; s->pn = g_pn; s->f = g_fns; s->nf = g_nf;
    s->opt_e = g_opt_e; s->opt_x = g_opt_x; s->opt_u = g_opt_u;
    g_cv = g_nv + 4; g_vars = xm((size_t)g_cv * sizeof(Var));
    for (int i = 0; i < g_nv; i++) { g_vars[i].name = xdup(s->v[i].name); g_vars[i].val = s->v[i].val ? xdup(s->v[i].val) : 0; g_vars[i].exp = s->v[i].exp; }
    g_pa = xm((size_t)(g_pn + 1) * sizeof(char*)); for (int i = 0; i < g_pn; i++) g_pa[i] = xdup(s->pa[i]);
    g_fns = xm((size_t)(g_nf + 1) * sizeof(Fn));
    for (int i = 0; i < g_nf; i++) { g_fns[i].name = xdup(s->f[i].name); g_fns[i].body = s->f[i].body; g_fns[i].body->refs++; }
}
static void snap_restore(Snap* s) {
    for (int i = 0; i < g_nv; i++) { free(g_vars[i].name); free(g_vars[i].val); }
    free(g_vars); g_vars = s->v; g_nv = s->nv; g_cv = s->cv;
    for (int i = 0; i < g_pn; i++) free(g_pa[i]);
    free(g_pa); g_pa = s->pa; g_pn = s->pn;
    for (int i = 0; i < g_nf; i++) { free(g_fns[i].name); free_node(g_fns[i].body); }
    free(g_fns); g_fns = s->f; g_nf = s->nf;
    g_opt_e = s->opt_e; g_opt_x = s->opt_x; g_opt_u = s->opt_u;
}
static void cmdsub(Io* io, const char* code, size_t n, Buf* out) {
    Snap sn; snap_take(&sn);
    Buf ob = {0}; Io sub = *io; sub.out.f = 0; sub.out.b = &ob;
    char* c = xndup(code, n); int loops = g_loops, rd = g_rdepth, nb = g_locbase;
    run_code(c, &sub); free(c);
    int r = g_st; if (g_ctl == C_EXIT || g_ctl == C_RET) g_ctl = 0;
    g_loops = loops; g_rdepth = rd; g_locbase = nb;
    snap_restore(&sn); g_cs = r; g_st = r;
    while (ob.n && ob.p[ob.n - 1] == '\n') ob.p[--ob.n] = 0;
    if (ob.n) b_put(out, ob.p, ob.n);
    b_free(&ob);
}

/* ---------- встроенные команды ---------- */
typedef int (*BFn)(int argc, char** argv, Io* io);
static int parse_ll(const char* s, ll* out) {
    if (!*s) return 0;
    if ((s[0] == '\'' || s[0] == '"') && s[1]) { *out = (unsigned char)s[1]; return 1; }
    return ar_parse_int(s, out);
}
static size_t esc1(const char* s, Buf* o, int* stop) {       /* s - символ после обратной косой; возвращает число разобранных символов */
    char c = s[0];
    switch (c) {
    case 'n': b_c(o, '\n'); return 1; case 't': b_c(o, '\t'); return 1; case 'r': b_c(o, '\r'); return 1;
    case 'a': b_c(o, '\a'); return 1; case 'b': b_c(o, '\b'); return 1; case 'f': b_c(o, '\f'); return 1;
    case 'v': b_c(o, '\v'); return 1; case 'e': b_c(o, 27); return 1; case '\\': b_c(o, '\\'); return 1;
    case '"': b_c(o, '"'); return 1; case '\'': b_c(o, '\''); return 1;
    case 'c': *stop = 1; return 1;
    case 'x': { int v = 0, k = 0; while (k < 2 && isxdigit((unsigned char)s[1 + k])) { v = v * 16 + (isdigit((unsigned char)s[1 + k]) ? s[1 + k] - '0' : tolower((unsigned char)s[1 + k]) - 'a' + 10); k++; }
        if (!k) { b_s(o, "\\x"); return 1; } b_c(o, (char)v); return (size_t)k + 1; }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = 0, k = 0, skip = c == '0' ? 1 : 0; if (c == '0') { k = 0; }
        size_t i = (size_t)skip; int cnt = 0;
        if (!skip) { v = c - '0'; i = 1; cnt = 1; }
        while (cnt < (skip ? 3 : 3) && s[i] >= '0' && s[i] <= '7') { v = v * 8 + (s[i] - '0'); i++; cnt++; }
        (void)k; b_c(o, (char)v); return i; }
    default: b_c(o, '\\'); return 0;
    }
}
static int b_echo(int argc, char** argv, Io* io) {
    int nonl = 0, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char* f = argv[i] + 1; int ok = 1; for (; *f; f++) if (!strchr("neE", *f)) ok = 0;
        if (!ok) break;
        for (f = argv[i] + 1; *f; f++) { if (*f == 'n') nonl = 1; else if (*f == 'e') esc = 1; else esc = 0; }
    }
    Buf b = {0}; int stop = 0;
    for (; i < argc && !stop; i++) {
        if (b.n || (i > 1 && 0)) {}
        for (const char* s = argv[i]; *s && !stop; ) {
            if (esc && *s == '\\' && s[1]) { s++; s += esc1(s, &b, &stop); }
            else b_c(&b, *s++);
        }
        if (i + 1 < argc && !stop) b_c(&b, ' ');
    }
    if (!nonl && !stop) b_c(&b, '\n');
    sink_put(&io->out, b.p ? b.p : "", b.n); b_free(&b); return 0;
}
static void fmt_to(Buf* o, const char* fmt, ...) {
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(0, 0, fmt, ap); va_end(ap);
    if (n > 0) { char* t = xm((size_t)n + 1); vsnprintf(t, (size_t)n + 1, fmt, ap2); b_put(o, t, (size_t)n); free(t); }
    va_end(ap2);
}
static int b_printf(int argc, char** argv, Io* io) {
    if (argc < 2) { emsg(io, "printf: формат: printf формат [аргументы]"); return 2; }
    const char* fmt = argv[1]; int ai = 2, rc = 0, stop = 0; Buf o = {0};
    do {
        int used = 0;
        for (const char* p = fmt; *p && !stop; ) {
            if (*p == '\\' && p[1]) { p++; p += esc1(p, &o, &stop); continue; }
            if (*p != '%') { b_c(&o, *p++); continue; }
            p++;
            if (*p == '%') { b_c(&o, '%'); p++; continue; }
            char flags[8]; int nf = 0, width = 0, prec = -1;
            while (*p && strchr("-+ #0", *p) && nf < 6) flags[nf++] = *p++;
            flags[nf] = 0;
            if (*p == '*') { ll v = 0; if (ai < argc) { parse_ll(argv[ai], &v); ai++; used = 1; } width = (int)(v > 4096 ? 4096 : v < -4096 ? -4096 : v); p++; }
            else while (isdigit((unsigned char)*p)) { if (width < 4096) width = width * 10 + (*p - '0'); p++; }
            if (*p == '.') { p++; prec = 0;
                if (*p == '*') { ll v = 0; if (ai < argc) { parse_ll(argv[ai], &v); ai++; used = 1; } prec = (int)(v > 4096 ? 4096 : v < 0 ? -1 : v); p++; }
                else while (isdigit((unsigned char)*p)) { if (prec < 4096) prec = prec * 10 + (*p - '0'); p++; } }
            char cv = *p; if (!cv) { emsg(io, "printf: обрывок формата"); rc = 1; break; }
            p++;
            const char* arg = ai < argc ? argv[ai] : 0; if (strchr("sdiuxXobcfFeEgG", cv)) { if (arg) { ai++; used = 1; } }
            char spec[24];
            if (cv == 's' || cv == 'c' || cv == 'b') {
                const char* sv = arg ? arg : ""; char* tmp = 0;
                if (cv == 'b') { Buf eb = {0}; int st2 = 0; for (const char* q = sv; *q && !st2; ) { if (*q == '\\' && q[1]) { q++; q += esc1(q, &eb, &st2); } else b_c(&eb, *q++); } tmp = b_take(&eb); sv = tmp; if (st2) stop = 1; }
                else if (cv == 'c') { tmp = xndup(sv, *sv ? utf8_clen(sv, strlen(sv)) : 0); sv = tmp; }
                snprintf(spec, sizeof spec, "%%%s*.*s", flags); fmt_to(&o, spec, width, cv == 'c' ? -1 : prec, sv); free(tmp);
            } else if (strchr("di", cv)) {
                ll v = 0; if (arg && !parse_ll(arg, &v)) { emsg(io, "printf: %s: ожидалось число", arg); rc = 1; }
                snprintf(spec, sizeof spec, "%%%s*.*lld", flags); fmt_to(&o, spec, width, prec, v);
            } else if (strchr("uoxX", cv)) {
                ll v = 0; if (arg && !parse_ll(arg, &v)) { emsg(io, "printf: %s: ожидалось число", arg); rc = 1; }
                snprintf(spec, sizeof spec, "%%%s*.*ll%c", flags, cv); fmt_to(&o, spec, width, prec, (ull)v);
            } else if (strchr("fFeEgG", cv)) {
                double v = 0; if (arg) { char* end; v = strtod(arg, &end); if (end == arg || *end) { emsg(io, "printf: %s: ожидалось число", arg); rc = 1; } }
                snprintf(spec, sizeof spec, "%%%s*.*%c", flags, cv); fmt_to(&o, spec, width, prec, v);
            } else { emsg(io, "printf: %%%c: неверный формат", cv); rc = 1; break; }
        }
        if (!used) break;
    } while (ai < argc && !stop && !rc);
    sink_put(&io->out, o.p ? o.p : "", o.n); b_free(&o); return rc;
}

typedef struct { char** a; int n, i, err; Io* io; } Tst;
static int t_or(Tst* t);
static int t_isbin(const char* s) { static const char* b[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "<", ">" }; for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) if (!strcmp(s, b[i])) return 1; return 0; }
static int t_isun(const char* s) { static const char* u = "-e-f-d-r-w-x-s-z-n-L-h"; return strlen(s) == 2 && s[0] == '-' && strstr(u, s) && (strstr(u, s) - u) % 2 == 0; }
static int t_prim(Tst* t) {
    if (t->i >= t->n) { t->err = 1; return 0; }
    const char* a = t->a[t->i];
    if (!strcmp(a, "(") && t->i + 1 < t->n && !(t->i + 2 < t->n && t_isbin(t->a[t->i + 1]))) {
        t->i++; int r = t_or(t);
        if (t->i < t->n && !strcmp(t->a[t->i], ")")) t->i++; else t->err = 1;
        return r;
    }
    if (t->i + 1 < t->n && t_isbin(t->a[t->i + 1])) {
        if (t->i + 2 >= t->n) { t->err = 1; return 0; }
        const char *x = a, *op = t->a[t->i + 1], *y = t->a[t->i + 2]; t->i += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(x, y);
        if (!strcmp(op, "!=")) return strcmp(x, y) != 0;
        if (!strcmp(op, "<")) return strcmp(x, y) < 0;
        if (!strcmp(op, ">")) return strcmp(x, y) > 0;
        ll p, q; if (!ar_parse_int(x, &p) || !ar_parse_int(y, &q)) { emsg(t->io, "test: ожидалось целое число: %s", ar_parse_int(x, &p) ? y : x); t->err = 2; return 0; }
        if (!strcmp(op, "-eq")) return p == q;
        if (!strcmp(op, "-ne")) return p != q;
        if (!strcmp(op, "-lt")) return p < q;
        if (!strcmp(op, "-le")) return p <= q;
        if (!strcmp(op, "-gt")) return p > q;
        return p >= q;
    }
    if (t_isun(a) && t->i + 1 < t->n) {
        const char* f = t->a[t->i + 1]; t->i += 2; char o = a[1];
        if (o == 'z') return !*f;
        if (o == 'n') return *f != 0;
        if (o == 'd') return !strcmp(f, ".") || !strcmp(f, "/");
        if (o == 'x' || o == 'L' || o == 'h') return 0;
        ll sz = file_size(f);
        if (o == 's') return sz > 0;
        return sz >= 0;                                       /* -e -f -r -w */
    }
    t->i++; return *a != 0;
}
static int t_not(Tst* t) {
    int r = 0;
    if (REC_ENTER()) t->err = 1;
    else if (t->i + 1 < t->n && !strcmp(t->a[t->i], "!")) { t->i++; r = !t_not(t); }
    else r = t_prim(t);
    REC_LEAVE(); return r;
}
static int t_and(Tst* t) { int r = t_not(t); while (!t->err && t->i < t->n && !strcmp(t->a[t->i], "-a")) { t->i++; int q = t_not(t); r = r && q; } return r; }
static int t_or(Tst* t) {
    if (REC_ENTER()) { t->err = 1; REC_LEAVE(); return 0; }
    int r = t_and(t); while (!t->err && t->i < t->n && !strcmp(t->a[t->i], "-o")) { t->i++; int q = t_and(t); r = r || q; }
    REC_LEAVE(); return r;
}
static int b_test(int argc, char** argv, Io* io) {
    int n = argc - 1; char** a = argv + 1;
    if (!strcmp(argv[0], "[")) { if (n < 1 || strcmp(a[n - 1], "]")) { emsg(io, "[: нет закрывающей ]"); return 2; } n--; }
    if (n == 0) return 1;
    Tst t = { a, n, 0, 0, io }; int r = t_or(&t);
    if (!t.err && t.i < t.n) { emsg(io, "test: лишний аргумент: %s", a[t.i]); return 2; }
    if (t.err == 1) { emsg(io, "test: неверное выражение"); return 2; }
    return t.err ? 2 : !r;
}
static int each_input(Io* io, char** files, int nf, int (*fn)(Src*, const char*, void*), void* ctx) {
    int bad = 0;
    if (!nf) return fn(io->in, 0, ctx);
    for (int i = 0; i < nf; i++) {
        if (!strcmp(files[i], "-")) { if (fn(io->in, "-", ctx)) bad = 1; continue; }
        FILE* f = fopen_sb(io, files[i], "rb", 0); if (!f) { bad = 1; continue; }
        Src s = { f, 0, 0, 0 }; if (fn(&s, files[i], ctx)) bad = 1; fclose(f);
    }
    return bad;
}
typedef struct { Io* io; int number; ll line; int bol; } CatC;
static int cat_one(Src* s, const char* name, void* v) {
    CatC* c = v; static char buf[1024]; size_t n; (void)name;
    while ((n = src_read(s, buf, sizeof buf)) > 0) {
        if (!c->number) { sink_put(&c->io->out, buf, n); continue; }
        for (size_t i = 0; i < n; i++) {
            if (c->bol) { char nb[32]; int l = snprintf(nb, sizeof nb, "%6lld\t", ++c->line); sink_put(&c->io->out, nb, (size_t)l); c->bol = 0; }
            sink_put(&c->io->out, buf + i, 1); if (buf[i] == '\n') c->bol = 1;
        }
    }
    return 0;
}
static int b_cat(int argc, char** argv, Io* io) {
    CatC c = { io, 0, 0, 1 }; int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "-n")) c.number = 1; else if (!strcmp(argv[i], "--")) { i++; break; } else { emsg(io, "cat: неизвестный ключ %s", argv[i]); return 2; } }
    return each_input(io, argv + i, argc - i, cat_one, &c);
}
typedef struct { ll lines, words, bytes, chars; } WcT;
static int wc_one(Src* s, const char* name, void* v) {
    WcT* t = v; static char buf[1024]; size_t n; int inw = 0; (void)name;
    t->lines = t->words = t->bytes = t->chars = 0;
    while ((n = src_read(s, buf, sizeof buf)) > 0) {
        t->bytes += (ll)n; t->chars += (ll)utf8_count(buf, n);
        for (size_t i = 0; i < n; i++) { if (buf[i] == '\n') t->lines++; if (isspace((unsigned char)buf[i])) inw = 0; else if (!inw) { inw = 1; t->words++; } }
    }
    return 0;
}
static void wc_print(Io* io, const WcT* t, int fl, int fw, int fm, int fc, const char* name) {
    char b[96]; int l = 0;
    if (fl) l += snprintf(b + l, sizeof b - (size_t)l, "%s%lld", l ? " " : "", t->lines);
    if (fw) l += snprintf(b + l, sizeof b - (size_t)l, "%s%lld", l ? " " : "", t->words);
    if (fm) l += snprintf(b + l, sizeof b - (size_t)l, "%s%lld", l ? " " : "", t->chars);
    if (fc) l += snprintf(b + l, sizeof b - (size_t)l, "%s%lld", l ? " " : "", t->bytes);
    sink_put(&io->out, b, (size_t)l);
    if (name) { sink_put(&io->out, " ", 1); sink_put(&io->out, name, strlen(name)); }
    sink_put(&io->out, "\n", 1);
}
static int b_wc(int argc, char** argv, Io* io) {
    int fl = 0, fw = 0, fc = 0, fm = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'l') fl = 1; else if (*f == 'w') fw = 1; else if (*f == 'c') fc = 1; else if (*f == 'm') fm = 1; else { emsg(io, "wc: неизвестный ключ -%c", *f); return 2; } }
    }
    if (!fl && !fw && !fc && !fm) fl = fw = fc = 1;
    int nf = argc - i, bad = 0; WcT total = {0};
    for (int k = 0; k < (nf ? nf : 1); k++) {
        WcT t; char* name = nf ? argv[i + k] : 0;
        if (nf && strcmp(name, "-")) { FILE* f = fopen_sb(io, name, "rb", 0); if (!f) { bad = 1; continue; } Src s = { f, 0, 0, 0 }; wc_one(&s, name, &t); fclose(f); }
        else wc_one(io->in, name, &t);
        total.lines += t.lines; total.words += t.words; total.bytes += t.bytes; total.chars += t.chars;
        wc_print(io, &t, fl, fw, fm, fc, name);
    }
    if (nf > 1) wc_print(io, &total, fl, fw, fm, fc, "итого");
    return bad;
}
typedef struct { Io* io; ll n; } HeadC;
static int head_one(Src* s, const char* name, void* v) {
    HeadC* h = v; ll lines = 0; int c; (void)name;
    while (lines < h->n && (c = src_getc(s)) != EOF) { char ch = (char)c; sink_put(&h->io->out, &ch, 1); if (c == '\n') lines++; }
    return 0;
}
static int b_head(int argc, char** argv, Io* io) {
    HeadC h = { io, 10 }; int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n")) { if (i + 1 >= argc || !ar_parse_int(argv[i + 1], &h.n)) { emsg(io, "head: после -n нужно число"); return 2; } i++; }
        else if (isdigit((unsigned char)argv[i][1])) { if (!ar_parse_int(argv[i] + 1, &h.n)) { emsg(io, "head: неверное число %s", argv[i]); return 2; } }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { emsg(io, "head: неизвестный ключ %s", argv[i]); return 2; }
    }
    return each_input(io, argv + i, argc - i, head_one, &h);
}
static int b_ls(int argc, char** argv, Io* io) {
    int lng = 0, i = 1, bad = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'l') lng = 1; else if (*f != '1' && *f != 'a' && *f != 'A') { emsg(io, "ls: неизвестный ключ -%c", *f); return 2; } }
    Argv names = {0};
    if (i < argc) { for (; i < argc; i++) { if (file_size(argv[i]) < 0) { emsg(io, "ls: %s: нет такого файла", argv[i]); bad = 1; } else av_push(&names, xdup(argv[i])); } if (names.n > 1) qsort(names.v, (size_t)names.n, sizeof(char*), cmp_s); }
    else if (sandbox_list(&names)) { emsg(io, "ls: не удаётся прочитать песочницу"); return 1; }
    for (int k = 0; k < names.n; k++) {
        if (lng) { char b[32]; int l = snprintf(b, sizeof b, "%8lld ", file_size(names.v[k])); sink_put(&io->out, b, (size_t)l); }
        out_s(io, names.v[k]); sink_put(&io->out, "\n", 1);
    }
    av_free(&names); return bad;
}
static int b_rm(int argc, char** argv, Io* io) {
    int force = 0, i = 1, bad = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'f') force = 1; else if (*f != 'r' && *f != 'R') { emsg(io, "rm: неизвестный ключ -%c", *f); return 2; } } }
    if (i >= argc && !force) { emsg(io, "rm: не указан файл"); return 2; }
    for (; i < argc; i++) {
        if (!valid_fname(argv[i])) { emsg(io, "rm: %s: недопустимое имя", argv[i]); bad = 1; continue; }
        if (remove(argv[i])) { if (!force) { emsg(io, "rm: %s: нет такого файла", argv[i]); bad = 1; } }
    }
    return bad;
}
static int b_cp(int argc, char** argv, Io* io) {
    int i = 1; while (i < argc && argv[i][0] == '-' && argv[i][1]) i++;
    if (argc - i != 2) { emsg(io, "cp: нужно два имени: cp откуда куда"); return 2; }
    if (!strcmp(argv[i], argv[i + 1])) { emsg(io, "cp: %s и %s - один файл", argv[i], argv[i + 1]); return 1; }
    FILE* a = fopen_sb(io, argv[i], "rb", 0); if (!a) return 1;
    FILE* b = fopen_sb(io, argv[i + 1], "wb", 0); if (!b) { fclose(a); return 1; }
    static char buf[1024]; size_t n; int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { rc = 1; break; }
    fclose(a); if (fclose(b)) rc = 1;
    if (rc) emsg(io, "cp: ошибка записи в %s", argv[i + 1]);
    return rc;
}
static int b_mv(int argc, char** argv, Io* io) {
    if (argc != 3) { emsg(io, "mv: нужно два имени: mv откуда куда"); return 2; }
    if (!valid_fname(argv[1]) || !valid_fname(argv[2])) { emsg(io, "mv: недопустимое имя"); return 1; }
    if (file_size(argv[1]) < 0) { emsg(io, "mv: %s: нет такого файла", argv[1]); return 1; }
    if (!strcmp(argv[1], argv[2])) return 0;
    if (rename(argv[1], argv[2])) { emsg(io, "mv: не удалось переименовать %s", argv[1]); return 1; }
    return 0;
}
static int b_pwd(int argc, char** argv, Io* io) { (void)argc; (void)argv; out_s(io, "/\n"); return 0; }
static int b_cd(int argc, char** argv, Io* io) {
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "/") || !strcmp(argv[1], ".")))) return 0;
    emsg(io, "cd: каталогов нет (песочница плоская)"); return 1;
}
static int b_true(int argc, char** argv, Io* io) { (void)argc; (void)argv; (void)io; return 0; }
static int b_false(int argc, char** argv, Io* io) { (void)argc; (void)argv; (void)io; return 1; }
static int cmp_var(const void* a, const void* b) { return strcmp(((const Var*)a)->name, ((const Var*)b)->name); }
static void quote_into(Buf* b, const char* v) { b_c(b, '\''); for (; *v; v++) { if (*v == '\'') b_s(b, "'\\''"); else b_c(b, *v); } b_c(b, '\''); }
static void list_vars(Io* io, int only_exp) {
    Var* c = xm((size_t)(g_nv + 1) * sizeof(Var)); if (g_nv) { memcpy(c, g_vars, (size_t)g_nv * sizeof(Var)); qsort(c, (size_t)g_nv, sizeof(Var), cmp_var); }
    for (int i = 0; i < g_nv; i++) {
        if ((only_exp && !c[i].exp) || !c[i].val) continue;
        Buf b = {0}; if (only_exp) b_s(&b, "export "); b_s(&b, c[i].name); b_c(&b, '='); quote_into(&b, c[i].val); b_c(&b, '\n');
        sink_put(&io->out, b.p, b.n); b_free(&b);
    }
    free(c);
}
static void pos_set(char** v, int n) {
    char** nv = xm((size_t)(n + 1) * sizeof(char*)); for (int i = 0; i < n; i++) nv[i] = xdup(v[i]);
    for (int i = 0; i < g_pn; i++) free(g_pa[i]);
    free(g_pa); g_pa = nv; g_pn = n;
}
static int b_set(int argc, char** argv, Io* io) {
    if (argc == 1) { list_vars(io, 0); return 0; }
    int i = 1;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--")) { i++; pos_set(argv + i, argc - i); return 0; }
        if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
        int on = a[0] == '-';
        for (const char* f = a + 1; *f; f++) { if (*f == 'e') g_opt_e = on; else if (*f == 'x') g_opt_x = on; else if (*f == 'u') g_opt_u = on; else { emsg(io, "set: неизвестный ключ %c%c (есть e, x, u)", a[0], *f); return 2; } }
    }
    if (i < argc) pos_set(argv + i, argc - i);
    return 0;
}
static int b_unset(int argc, char** argv, Io* io) {
    int fn = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) { if (!strcmp(argv[i], "-f")) fn = 1; else if (!strcmp(argv[i], "-v")) fn = 0; else { emsg(io, "unset: неизвестный ключ %s", argv[i]); return 2; } }
    for (; i < argc; i++) { if (fn) fn_remove(argv[i]); else var_unset(argv[i]); }
    return 0;
}
static int b_export(int argc, char** argv, Io* io) {
    if (argc == 1) { list_vars(io, 1); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '='); size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!is_name(argv[i], nl)) { emsg(io, "export: %s: неверное имя", argv[i]); rc = 1; continue; }
        char* nm = xndup(argv[i], nl);
        if (eq) var_set(nm, eq + 1);
        int k = var_find(nm); if (k < 0) { var_set(nm, ""); var_get(nm); k = var_find(nm); free(g_vars[k].val); g_vars[k].val = 0; }
        g_vars[k].exp = 1; free(nm);
    }
    return rc;
}
static int b_local(int argc, char** argv, Io* io) {
    if (!g_rdepth) { emsg(io, "local: только внутри функции"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '='); size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!is_name(argv[i], nl)) { emsg(io, "local: %s: неверное имя", argv[i]); rc = 1; continue; }
        char* nm = xndup(argv[i], nl); int dup = 0;
        for (int k = g_locbase; k < g_nloc; k++) if (!strcmp(g_loc[k].name, nm)) dup = 1;
        if (!dup) {
            if (g_nloc == g_cloc) { g_cloc = g_cloc ? g_cloc * 2 : 16; g_loc = xr(g_loc, (size_t)g_cloc * sizeof(Loc)); }
            int k = var_find(nm); Loc* l = &g_loc[g_nloc++];
            l->name = xdup(nm); l->had = k >= 0 && g_vars[k].val; l->old = l->had ? xdup(g_vars[k].val) : 0; l->exp = k >= 0 ? g_vars[k].exp : 0;
            if (k >= 0) { free(g_vars[k].val); g_vars[k].val = 0; }
        }
        if (eq) var_set(nm, eq + 1);
        free(nm);
    }
    return rc;
}
static int b_shift(int argc, char** argv, Io* io) {
    ll n = 1; if (argc > 1 && (!ar_parse_int(argv[1], &n) || n < 0)) { emsg(io, "shift: нужно неотрицательное число"); return 2; }
    if (n > g_pn) { emsg(io, "shift: параметров только %d", g_pn); return 1; }
    for (ll i = 0; i < n; i++) free(g_pa[i]);
    if (n) { memmove(g_pa, g_pa + n, (size_t)(g_pn - n) * sizeof(char*)); g_pn -= (int)n; }
    return 0;
}
static int b_exit(int argc, char** argv, Io* io) {
    ll n = g_st; if (argc > 1 && !ar_parse_int(argv[1], &n)) { emsg(io, "exit: нужно число"); n = 2; }
    g_ctl = C_EXIT; return (int)(n & 255);
}
static int b_return(int argc, char** argv, Io* io) {
    if (!g_rdepth) { emsg(io, "return: только в функции или в source"); return 1; }
    ll n = g_st; if (argc > 1 && !ar_parse_int(argv[1], &n)) { emsg(io, "return: нужно число"); n = 2; }
    g_ctl = C_RET; return (int)(n & 255);
}
static int b_break(int argc, char** argv, Io* io) {
    ll n = 1; if (argc > 1 && (!ar_parse_int(argv[1], &n) || n < 1)) { emsg(io, "%s: нужно число больше 0", argv[0]); return 2; }
    if (!g_loops) { emsg(io, "%s: вне цикла", argv[0]); return 1; }
    g_ctl = argv[0][0] == 'b' ? C_BREAK : C_CONT; g_ctln = n > g_loops ? g_loops : (int)n; return 0;
}
static int b_read(int argc, char** argv, Io* io) {
    int raw = 0, i = 1; const char* prompt = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-r")) raw = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { emsg(io, "read: неизвестный ключ %s", argv[i]); return 2; }
    }
    if (prompt) { sink_put(&io->err, prompt, strlen(prompt)); if (io->err.f) fflush(io->err.f); }
    Buf l = {0}; int c;
    while ((c = src_getc(io->in)) != EOF) {
        if (c == '\n') break;
        if (c == '\\' && !raw) { int d = src_getc(io->in); if (d == EOF) break; if (d == '\n') continue; b_c(&l, '\\'); b_c(&l, (char)d); continue; }
        b_c(&l, (char)c);
    }
    int eof = c == EOF;
    const char* ifs = ifs_get(); const char* s = l.p ? l.p : ""; size_t n = l.n, p = 0;
    char* dflt[1] = { "REPLY" }; char** names = i < argc ? argv + i : dflt; int nn = i < argc ? argc - i : 1;
    for (int k = 0; k < nn; k++) {
        if (!is_name(names[k], strlen(names[k]))) { emsg(io, "read: %s: неверное имя", names[k]); b_free(&l); return 2; }
        Buf v = {0};
        if (i >= argc) { b_put(&v, s, n); p = n; }                                    /* REPLY: строка целиком */
        else {
            while (p < n && strchr(ifs, s[p]) && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n')) p++;
            if (k == nn - 1) {
                size_t e = n; while (e > p && strchr(ifs, s[e - 1]) && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n')) e--;
                for (size_t q = p; q < e; q++) { if (s[q] == '\\' && !raw && q + 1 < e) q++; b_c(&v, s[q]); } p = n;
            } else {
                while (p < n && !strchr(ifs, s[p])) { if (s[p] == '\\' && !raw && p + 1 < n) p++; b_c(&v, s[p]); p++; }
                if (p < n && strchr(ifs, s[p])) { int ws = s[p] == ' ' || s[p] == '\t' || s[p] == '\n'; p++; if (!ws) while (p < n && (s[p] == ' ' || s[p] == '\t')) p++; }
                while (p < n && (s[p] == ' ' || s[p] == '\t') && strchr(ifs, s[p])) p++;
            }
        }
        var_set(names[k], v.p ? v.p : ""); b_free(&v);
    }
    if (i >= argc && !raw) {}
    b_free(&l);
    return eof ? 1 : 0;
}
/* expr */
typedef struct { char** a; int n, i, err; Io* io; } Ex;
static char* ex_or(Ex* x);
static int ex_isint(const char* s) { ll v; return *s && ar_parse_int(s, &v) && (isdigit((unsigned char)s[0]) || s[0] == '-' || s[0] == '+') && !(s[0] == '0' && s[1] && s[1] != ' ' && 0); }
static int ex_true(const char* s) { ll v; if (!*s) return 0; if (ex_isint(s) && ar_parse_int(s, &v)) return v != 0; return 1; }
static char* ex_num(ll v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return xdup(b); }
static char* ex_atom_body(Ex* x) {
    if (x->i >= x->n) { x->err = 1; return xdup(""); }
    const char* t = x->a[x->i++];
    if (!strcmp(t, "(")) { char* r = ex_or(x); if (x->i < x->n && !strcmp(x->a[x->i], ")")) x->i++; else x->err = 1; return r; }
    if (!strcmp(t, "length") && x->i < x->n) { const char* s = x->a[x->i++]; return ex_num((ll)utf8_count(s, strlen(s))); }
    if (!strcmp(t, "substr") && x->i + 2 < x->n) {
        const char* s = x->a[x->i]; ll p = 0, l = 0; ar_parse_int(x->a[x->i + 1], &p); ar_parse_int(x->a[x->i + 2], &l); x->i += 3;
        size_t sl = strlen(s), a = 0; ll cp = 1;
        while (a < sl && cp < p) { a += utf8_clen(s + a, sl - a); cp++; }
        size_t b = a; while (b < sl && l > 0) { b += utf8_clen(s + b, sl - b); l--; }
        return p < 1 ? xdup("") : xndup(s + a, b - a);
    }
    return xdup(t);
}
static char* ex_atom(Ex* x) {
    char* r;
    if (REC_ENTER()) { x->err = 1; r = xdup(""); } else r = ex_atom_body(x);
    REC_LEAVE(); return r;
}
static char* ex_bin(Ex* x, int lvl) {
    static const char* ops[][4] = { {"*", "/", "%", 0}, {"+", "-", 0, 0}, {0, 0, 0, 0} };
    if (lvl < 0) return ex_atom(x);
    char* l = ex_bin(x, lvl - 1);
    for (;;) {
        if (x->i >= x->n || x->err) return l;
        const char* o = x->a[x->i]; int hit = 0;
        for (int k = 0; lvl < 2 && ops[lvl][k]; k++) if (!strcmp(o, ops[lvl][k])) hit = 1;
        if (lvl == 2) { hit = !strcmp(o, "=") || !strcmp(o, "==") || !strcmp(o, "!=") || !strcmp(o, "<") || !strcmp(o, "<=") || !strcmp(o, ">") || !strcmp(o, ">="); }
        if (!hit) return l;
        x->i++; char* r = ex_bin(x, lvl - 1); char* res;
        ll p = 0, q = 0; int num = ex_isint(l) && ex_isint(r);
        if (lvl < 2 || num) { ar_parse_int(l, &p); ar_parse_int(r, &q); }
        if (lvl < 2 && !num) { emsg(x->io, "expr: нужны целые числа"); x->err = 2; res = xdup("0"); }
        else if (lvl == 0) {
            if (o[0] != '*' && !q) { emsg(x->io, "expr: деление на ноль"); x->err = 2; res = xdup("0"); }
            else res = ex_num(o[0] == '*' ? (ll)((ull)p * (ull)q) : q == -1 ? (o[0] == '/' ? (ll)(0 - (ull)p) : 0) : o[0] == '/' ? p / q : p % q);
        } else if (lvl == 1) res = ex_num(o[0] == '+' ? (ll)((ull)p + (ull)q) : (ll)((ull)p - (ull)q));
        else {
            int c = num ? (p < q ? -1 : p > q) : strcmp(l, r);
            int v = !strcmp(o, "=") || !strcmp(o, "==") ? c == 0 : !strcmp(o, "!=") ? c != 0 : !strcmp(o, "<") ? c < 0 : !strcmp(o, "<=") ? c <= 0 : !strcmp(o, ">") ? c > 0 : c >= 0;
            res = ex_num(v);
        }
        free(l); free(r); l = res;
    }
}
static char* ex_and(Ex* x) {
    char* l = ex_bin(x, 2);
    while (!x->err && x->i < x->n && !strcmp(x->a[x->i], "&")) { x->i++; char* r = ex_bin(x, 2); int ok = ex_true(l) && ex_true(r); free(r); if (!ok) { free(l); l = xdup("0"); } }
    return l;
}
static char* ex_or(Ex* x) {
    char* l = ex_and(x);
    while (!x->err && x->i < x->n && !strcmp(x->a[x->i], "|")) { x->i++; char* r = ex_and(x); if (ex_true(l)) free(r); else { free(l); l = r; } }
    return l;
}
static int b_expr(int argc, char** argv, Io* io) {
    if (argc < 2) { emsg(io, "expr: нужно выражение"); return 2; }
    Ex x = { argv + 1, argc - 1, 0, 0, io }; char* r = ex_or(&x);
    if (!x.err && x.i < x.n) { emsg(io, "expr: лишний аргумент: %s", x.a[x.i]); x.err = 1; }
    if (x.err) { if (x.err == 1) emsg(io, "expr: ошибка в выражении"); free(r); return 2; }
    out_s(io, r); sink_put(&io->out, "\n", 1); int rc = !ex_true(r); free(r); return rc;
}
static int b_eval(int argc, char** argv, Io* io) {
    Buf b = {0}; for (int i = 1; i < argc; i++) { if (i > 1) b_c(&b, ' '); b_s(&b, argv[i]); }
    if (b.n) { g_rdepth += 0; run_code(b.p, io); } else g_st = 0;
    b_free(&b); return g_st;
}
static int b_source(int argc, char** argv, Io* io) {
    if (argc < 2) { emsg(io, "%s: нужно имя файла", argv[0]); return 2; }
    char* text = slurp(io, argv[1], 0); if (!text) return 1;
    char** sa = g_pa; int sn = g_pn; int setargs = argc > 2;
    if (setargs) { g_pa = 0; g_pn = 0; pos_set(argv + 2, argc - 2); }
    g_rdepth++; run_code(text, io); g_rdepth--;
    if (g_ctl == C_RET) g_ctl = 0;
    if (setargs) { for (int i = 0; i < g_pn; i++) free(g_pa[i]); free(g_pa); g_pa = sa; g_pn = sn; }
    free(text); return g_st;
}
static const struct { const char* name; BFn fn; } g_builtins[] = {
    {":", b_true}, {"true", b_true}, {"false", b_false}, {"echo", b_echo}, {"printf", b_printf}, {"test", b_test}, {"[", b_test},
    {"cat", b_cat}, {"ls", b_ls}, {"rm", b_rm}, {"cp", b_cp}, {"mv", b_mv}, {"wc", b_wc}, {"head", b_head}, {"pwd", b_pwd}, {"cd", b_cd},
    {"read", b_read}, {"set", b_set}, {"unset", b_unset}, {"export", b_export}, {"local", b_local}, {"shift", b_shift}, {"eval", b_eval},
    {"source", b_source}, {".", b_source}, {"exit", b_exit}, {"return", b_return}, {"break", b_break}, {"continue", b_break}, {"expr", b_expr} };
static BFn builtin_find(const char* n) { for (size_t i = 0; i < sizeof g_builtins / sizeof g_builtins[0]; i++) if (!strcmp(g_builtins[i].name, n)) return g_builtins[i].fn; return 0; }

/* ---------- выполнение ---------- */
typedef struct { FILE* f[8]; int nf; void* mem[16]; int nm; } Rd;
static void rd_close(Rd* r, Io* io, int* st) {
    for (int i = 0; i < r->nf; i++) { if (ferror(r->f[i])) { emsg(io, "ошибка записи в файл"); if (!*st) *st = 1; } if (fclose(r->f[i]) && !*st) { emsg(io, "ошибка записи в файл"); *st = 1; } }
    for (int i = 0; i < r->nm; i++) free(r->mem[i]);
    r->nf = r->nm = 0;
}
static int redirs_open(Node* n, Io* io, Rd* rd) {                  /* io - копия, её и меняем */
    rd->nf = rd->nm = 0;
    for (int i = 0; i < n->nr; i++) {
        Redir* r = &n->r[i];
        if (rd->nf >= 8 || rd->nm >= 14) { emsg(io, "слишком много перенаправлений"); int z = 1; rd_close(rd, io, &z); return 0; }
        Src* s = 0; FILE* f = 0; char* w = 0;
        if (r->op == R_HERE) {
            w = r->hdq ? xdup(r->hd) : expand_hd(io, r->hd);
            s = xm(sizeof *s); s->f = 0; s->m = w; s->mn = strlen(w); s->mp = 0; rd->mem[rd->nm++] = w; rd->mem[rd->nm++] = s;
            if (r->fd != 0) { emsg(io, "документ-вставка только для ввода (0)"); int z = 1; rd_close(rd, io, &z); return 0; }
            io->in = s; continue;
        }
        w = expand_str(io, r->word);
        if (g_experr) { free(w); int z = 1; rd_close(rd, io, &z); return 0; }
        if (r->op == R_DUP) {
            int target = !strcmp(w, "1") ? 1 : !strcmp(w, "2") ? 2 : -1; free(w);
            if (target < 0 || (r->fd != 1 && r->fd != 2)) { emsg(io, "поддерживаются только >&1 и >&2 для дескрипторов 1 и 2"); int z = 1; rd_close(rd, io, &z); return 0; }
            Sink src = target == 1 ? io->out : io->err; if (r->fd == 1) io->out = src; else io->err = src;
            continue;
        }
        if (r->op == R_IN) {
            if (r->fd != 0) { emsg(io, "ввод можно перенаправить только для дескриптора 0"); free(w); int z = 1; rd_close(rd, io, &z); return 0; }
            f = fopen_sb(io, w, "rb", 0); free(w);
            if (!f) { int z = 1; rd_close(rd, io, &z); return 0; }
            s = xm(sizeof *s); s->f = f; s->m = 0; s->mn = s->mp = 0; rd->f[rd->nf++] = f; rd->mem[rd->nm++] = s; io->in = s; continue;
        }
        if (r->fd != 1 && r->fd != 2) { emsg(io, "вывод можно перенаправить только для дескрипторов 1 и 2"); free(w); int z = 1; rd_close(rd, io, &z); return 0; }
        f = fopen_sb(io, w, r->op == R_APP ? "ab" : "wb", 0); free(w);
        if (!f) { int z = 1; rd_close(rd, io, &z); return 0; }
        rd->f[rd->nf++] = f;
        Sink sk = { f, 0 }; if (r->fd == 1) io->out = sk; else io->err = sk;
    }
    return 1;
}
static int is_assign(const char* w) { size_t i = 0; if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0; while (isalnum((unsigned char)w[i]) || w[i] == '_') i++; return w[i] == '='; }
static int exec(Node* n, Io* io);
static int call_func(Fn* f, int argc, char** argv, Io* io) {
    Node* body = f->body; body->refs++;                       /* функцию могут переопределить во время выполнения */
    char** sp = g_pa; int sn = g_pn, base = g_locbase, loops = g_loops;
    g_pa = 0; g_pn = 0; pos_set(argv + 1, argc - 1);
    g_locbase = g_nloc; g_loops = 0; g_rdepth++;
    int r = exec(body, io);
    g_rdepth--; loc_restore_to(g_locbase); g_locbase = base; g_loops = loops;
    if (g_ctl == C_RET) { g_ctl = 0; }
    else if (g_ctl == C_BREAK || g_ctl == C_CONT) g_ctl = 0;
    for (int i = 0; i < g_pn; i++) free(g_pa[i]);
    free(g_pa); g_pa = sp; g_pn = sn;
    free_node(body); return g_st = r;
}
typedef struct { char* name; char* val; char* old; int had, exp; } Tmp;
static int exec_simple(Node* n, Io* io) {
    Argv av = {0}; Tmp* tmp = 0; int nt = 0, i = 0, r = 0; g_cs = 0; g_experr = 0;
    int has_cmd = 0; for (int k = 0; k < n->nw; k++) if (!is_assign(n->w[k])) { has_cmd = 1; break; }
    for (; i < n->nw && is_assign(n->w[i]); i++) {
        const char* eq = strchr(n->w[i], '='); char* nm = xndup(n->w[i], (size_t)(eq - n->w[i])); char* v = expand_str(io, eq + 1);
        if (g_experr) { free(nm); free(v); goto done; }
        if (!has_cmd) { var_set(nm, v); free(nm); free(v); if (g_ctl) goto done; }
        else { tmp = xr(tmp, (size_t)(nt + 1) * sizeof(Tmp)); tmp[nt].name = nm; tmp[nt].val = v; tmp[nt].old = 0; tmp[nt].had = 0; tmp[nt].exp = 0; nt++; }
    }
    for (; i < n->nw; i++) {
        if (av.n > 0 && (!strcmp(av.v[0], "export") || !strcmp(av.v[0], "local")) && is_assign(n->w[i])) av_push(&av, expand_str(io, n->w[i]));
        else expand_fields(io, n->w[i], &av);
        if (g_experr || g_ctl) goto done;
    }
    if (!av.n) { r = n->nw && !nt ? g_cs : (has_cmd ? 0 : g_cs); goto done; }
    for (int k = 0; k < nt; k++) {
        int vi = var_find(tmp[k].name);
        if (vi >= 0 && g_vars[vi].val) { tmp[k].had = 1; tmp[k].old = xdup(g_vars[vi].val); }
        if (vi >= 0) tmp[k].exp = g_vars[vi].exp;
        var_set(tmp[k].name, tmp[k].val); g_vars[var_find(tmp[k].name)].exp = 1;
    }
    if (g_opt_x) { Buf b = {0}; b_s(&b, "+"); for (int k = 0; k < av.n; k++) { b_c(&b, ' '); b_s(&b, av.v[k]); } b_c(&b, '\n'); sink_put(&io->err, b.p, b.n); b_free(&b); }
    {
        Fn* f = fn_find(av.v[0]);
        if (f) r = call_func(f, av.n, av.v, io);
        else { BFn b = builtin_find(av.v[0]); if (b) r = b(av.n, av.v, io); else { emsg(io, "%s: команда не найдена", av.v[0]); r = 127; } }
    }
    for (int k = nt - 1; k >= 0; k--) {
        if (tmp[k].had) { var_set(tmp[k].name, tmp[k].old); int vi = var_find(tmp[k].name); if (vi >= 0) g_vars[vi].exp = tmp[k].exp; } else var_unset(tmp[k].name);
    }
done:
    if (g_experr && !r) r = 1;
    for (int k = 0; k < nt; k++) { free(tmp[k].name); free(tmp[k].val); free(tmp[k].old); }
    free(tmp); av_free(&av);
    return r;
}
static void loop_ctl(int* brk) {                              /* после тела цикла: разобрать break/continue */
    if (g_ctl == C_BREAK || g_ctl == C_CONT) {
        int isb = g_ctl == C_BREAK;
        if (--g_ctln > 0) { *brk = 1; return; }               /* выше по вложенности */
        g_ctl = 0; if (isb) *brk = 1;
    } else if (g_ctl) *brk = 1;
}
#define NOINL __attribute__((noinline))
static NOINL int exec_sub(Node* n, Io* io) {
    Snap sn; snap_take(&sn); int loops = g_loops, rd = g_rdepth, nb = g_locbase;
    int r = exec(n->a, io);
    if (g_ctl == C_EXIT || g_ctl == C_RET) g_ctl = 0;
    g_loops = loops; g_rdepth = rd; g_locbase = nb;
    snap_restore(&sn); return g_st = r;
}
static NOINL int exec_pipe(Node* n, Io* io) {
    Buf prev = {0}; Src ps; int r = 0;
    for (int k = 0; k < n->nl; k++) {
        Io s = *io; Buf cur = {0};
        if (k) { ps.f = 0; ps.m = prev.p ? prev.p : ""; ps.mn = prev.n; ps.mp = 0; s.in = &ps; }
        if (k < n->nl - 1) { s.out.f = 0; s.out.b = &cur; }
        r = exec(n->l[k], &s);
        b_free(&prev); prev = cur;
        if (g_ctl) break;
    }
    b_free(&prev); return r;
}
static NOINL int exec_for(Node* n, Io* io) {
    Argv items = {0}; int brk = 0, r = 0;
    if (n->flag) { for (int i = 0; i < n->nw; i++) { expand_fields(io, n->w[i], &items); if (g_experr || g_ctl) { av_free(&items); return g_st = 1; } } }
    else for (int i = 0; i < g_pn; i++) av_push(&items, xdup(g_pa[i]));
    g_loops++;
    for (int i = 0; i < items.n; i++) {
        var_set(n->name, items.v[i]);
        r = exec(n->a, io); loop_ctl(&brk); if (brk) break;
    }
    g_loops--; av_free(&items); return g_st = r;
}
static NOINL int exec_case(Node* n, Io* io) {
    char* w = expand_str(io, n->w[0]); int r = 0;
    if (g_experr) { free(w); return g_st = 1; }
    for (int i = 0; i < n->nci; i++) {
        int hit = 0;
        for (int k = 0; k < n->ci[i].np && !hit; k++) { char* pat = expand_pat(io, n->ci[i].pats[k]); hit = pmatch(pat, w, strlen(w)); free(pat); }
        if (hit) { r = exec(n->ci[i].body, io); break; }
    }
    free(w); return g_st = r;
}
static int exec_inner(Node* n, Io* io) {
    int r = 0;
    switch (n->type) {
    case N_SIMPLE:
        r = exec_simple(n, io);
        g_st = r;
        if (r && g_opt_e && !g_noerr && !g_ctl) g_ctl = C_EXIT;
        return r;
    case N_LIST: for (int i = 0; i < n->nl && !g_ctl; i++) r = exec(n->l[i], io); return g_ctl ? g_st : r;
    case N_GROUP: return exec(n->a, io);
    case N_SUB: return exec_sub(n, io);
    case N_NOT: g_noerr++; r = exec(n->a, io); g_noerr--; return g_st = !r;
    case N_AND: case N_OR:
        g_noerr++; r = exec(n->a, io); g_noerr--;
        if (g_ctl) return r;
        if ((n->type == N_AND) == (r == 0)) return exec(n->b, io);
        return r;
    case N_PIPE: return exec_pipe(n, io);
    case N_IF:
        g_noerr++; r = exec(n->a, io); g_noerr--;
        if (g_ctl) return r;
        if (r == 0) return exec(n->b, io);
        if (n->c) return exec(n->c, io);
        return g_st = 0;
    case N_WHILE: {
        int brk = 0; r = 0; g_loops++;
        for (;;) {
            g_noerr++; int c = exec(n->a, io); g_noerr--;
            if (g_ctl) { loop_ctl(&brk); break; }
            if ((c == 0) == (n->flag != 0)) break;
            r = exec(n->b, io); loop_ctl(&brk); if (brk) break;
        }
        g_loops--; return g_st = r;
    }
    case N_FOR: return exec_for(n, io);
    case N_CASE: return exec_case(n, io);
    case N_FUNC: fn_define(n->name, n->a); return g_st = 0;
    }
    return 0;
}
static NOINL int exec_redir(Node* n, Io* io) {
    Io local = *io; Rd rd;
    if (!redirs_open(n, &local, &rd)) return g_st = 1;
    int r = exec_inner(n, &local); rd_close(&rd, io, &r); return g_st = r;
}
static int exec(Node* n, Io* io) {
    int r;
    if (REC_ENTER()) { if (!g_ctl) emsg(io, "слишком глубокая вложенность (рекурсия)"); g_ctl = C_EXIT; r = g_st = 1; }
    else if (g_ctl) r = g_st;
    else r = n->nr ? exec_redir(n, io) : exec_inner(n, io);
    REC_LEAVE(); return r;
}

/* ---------- запуск текста ---------- */
static void report(const char* msg, Io* io) { emsg(io, "%s", msg); }
static void run_code(const char* text, Io* io) {
    Toks T = {0}; char err[160]; int rc = lex(text, &T, err, sizeof err);
    if (rc) { report(err, io); g_st = 2; toks_free(&T); return; }
    P p; memset(&p, 0, sizeof p); p.t = T.v; p.n = T.n;
    g_st = 0;
    for (;;) {
        skip_nl(&p);
        if (pk(&p)->t == T_EOF) break;
        Node* c = parse_andor(&p);
        if (!c) { report(p.msg, io); g_st = 2; break; }
        Tok* t = pk(&p);
        if (t->t == T_SEMI || t->t == T_NL) p.pos++;
        else if (t->t == T_AMP) { perr(&p, 2, "строка %d: фоновый запуск (&) не поддерживается", t->line); report(p.msg, io); g_st = 2; free_node(c); break; }
        else if (t->t != T_EOF) { fail_unexpected(&p); report(p.msg, io); g_st = 2; free_node(c); break; }
        exec(c, io); free_node(c);
        if (g_ctl) break;
    }
    toks_free(&T);
}
static int syntax_state(const char* text) {                     /* 0 - команда полная, 1 - нужно продолжение, 2 - ошибка */
    Toks T = {0}; char err[160]; int rc = lex(text, &T, err, sizeof err);
    if (rc) { toks_free(&T); return rc; }
    P p; memset(&p, 0, sizeof p); p.t = T.v; p.n = T.n; int res = 0;
    for (;;) {
        skip_nl(&p);
        if (pk(&p)->t == T_EOF) break;
        Node* c = parse_andor(&p);
        if (!c) { res = p.err; break; }
        free_node(c);
        Tok* t = pk(&p);
        if (t->t == T_SEMI || t->t == T_NL) p.pos++;
        else if (t->t != T_EOF) { res = 2; break; }
    }
    toks_free(&T); return res;
}
static int repl(Io* io) {
    static Src in = { 0, 0, 0, 0 }; in.f = stdin;
    Io rio = *io; rio.in = &in; Buf acc = {0};
    g_name = "ksh";
    for (;;) {
        fputs(acc.n ? "> " : "ksh> ", stdout); fflush(stdout);
        Buf line = {0}; int c;
        while ((c = fgetc(stdin)) != EOF) { b_c(&line, (char)c); if (c == '\n') break; }
        if (!line.n) {                                          /* Ctrl+D; незаконченная команда - ошибка */
            b_free(&line); fputs("\n", stdout);
            if (acc.n) run_code(acc.p, &rio);
            break;
        }
        if (line.p[line.n - 1] != '\n') b_c(&line, '\n');
        b_put(&acc, line.p, line.n); b_free(&line);
        int s = syntax_state(acc.p);
        if (s == 1) continue;
        if (s == 0 || s == 2) { run_code(acc.p, &rio); }
        b_free(&acc);
        if (g_ctl == C_EXIT) break;
        g_ctl = 0; g_loops = 0;
    }
    b_free(&acc); return g_st;
}
int main(int argc, char** argv) {
    char base; g_stack0 = &base;
    static Src in; in.f = stdin;
    Io io = { &in, { stdout, 0 }, { stderr, 0 } };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "-c") && strcmp(argv[i], "--"); i++) {
        for (const char* f = argv[i] + 1; *f; f++) {
            if (*f == 'e') g_opt_e = 1; else if (*f == 'x') g_opt_x = 1; else if (*f == 'u') g_opt_u = 1;
            else { fprintf(stderr, "ksh: неизвестный ключ -%c. Использование: ksh [-e -x -u] файл [арги] | ksh -c код | ksh\n", *f); return 2; }
        }
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    int rc;
    if (i < argc && !strcmp(argv[i], "-c")) {                    /* оболочка Киры делит строку запуска по пробелам, поэтому код - всё остальное */
        if (i + 1 >= argc) { fputs("ksh: после -c нужен код\n", stderr); return 2; }
        Buf code = {0}; for (int k = i + 1; k < argc; k++) { if (k > i + 1) b_c(&code, ' '); b_s(&code, argv[k]); }
        g_name = "ksh"; run_code(code.p, &io); b_free(&code); rc = g_st;
    } else if (i < argc) {
        g_name = argv[i]; size_t n; char* text = slurp(&io, argv[i], &n);
        if (!text) rc = 127;
        else { if (i + 1 < argc) pos_set(argv + i + 1, argc - i - 1); run_code(text, &io); free(text); rc = g_st; }
    } else rc = repl(&io);
    fflush(stdout);
    return rc & 255;
}
