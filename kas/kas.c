/* kas: ассемблер x86-64, подмножество nasm. Структура файла:
 *  1. служебное (ошибки, арена)   2. регистры, символы, секции   3. препроцессор (%define, %macro, %if)
 *  4. выражения                   5. кодировщик команд           6. директивы и проходы
 *  7. запись ELF64 ET_REL.
 * Размеры переходов подбираются несколькими проходами (старт с коротких, только рост), последний проход
 * пишет байты и перемещения. Всё, чего нет в поддержанном списке, даёт ошибку "не поддержано". */
#include "kas.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAXLINE 1024
#define MAXSTMT 16384
#define POOLSZ (1u << 20)
#define MAXSYM 4096
#define MAXSEC 16
#define MAXDEF 128
#define MAXMAC 64
#define MAXMACLINES 256
#define MAXREL 16384
#define MAXPASS 40

enum { K_UNDEF, K_LABEL, K_ABS, K_EXT };
enum { V_ABS, V_SEC, V_EXT };
enum { O_NONE, O_REG, O_IMM, O_MEM, O_FAR };
enum { RK_GPR, RK_SEG, RK_CR };
enum { R_64 = 1, R_PC32 = 2, R_32 = 10, R_32S = 11, R_PLT32 = 4 };

typedef struct { int64_t v; int k, ref, unk, wplt, rsz; int r[2], rs[2]; } Val;
typedef struct { const char *name; int kind, sec, glob, seq, defpass, line, used; int64_t val; } Sym;
typedef struct {
    char name[48]; uint32_t type, flags; uint64_t align; int64_t off, size; uint8_t *data;
    size_t cap; int nobits, nrel;
} Sec;
typedef struct { uint64_t off; int sec, type, symk, ref; int64_t addend; } Rel;
typedef struct { const char *txt; int line; } Stmt;
typedef struct { const char *name; int np; const char *par[8]; const char *body; } Def;
typedef struct { const char *name; int np, nl; const char **lines; } Mac;
typedef struct { int kind, num, size, high; } Reg;
typedef struct { int on; Val v; int pcrel, sext; } RelReq;

typedef struct {
    KasArena *a; jmp_buf jb; char *err; size_t errcap; const char *file;
    char *pool; size_t pool_used;
    Stmt *st; int nst; uint8_t *jnear;
    Def defs[MAXDEF]; int ndef;
    Mac mac[MAXMAC]; int nmac;
    int inmac, curnp, mn, macid, ifdepth, macline; const char *curname; const char *mlines[MAXMACLINES];
    Sym *sy; int nsy, symseq;
    Sec sec[MAXSEC]; int nsec, cur, bits;
    Rel *rel; int nrel;
    int pass, final, changed;
    char scope[200];
    int line, idx;
    int ifst[32], ifc[32], ifn, skip, intimes;
} Ctx;

/* ---------- 1. служебное ---------- */
static void fail(Ctx *c, const char *fmt, ...) {
    char m[300]; va_list ap;
    va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    snprintf(c->err, c->errcap, "%s:%d: ошибка: %s", c->file, c->line, m);
    longjmp(c->jb, 1);
}
/* ошибка, зависящая от значений: в промежуточных проходах значения ещё не устоялись, поэтому молчим */
#define soft(c, ...) do { if ((c)->final) fail((c), __VA_ARGS__); } while (0)

void kas_arena_init(KasArena *a, void *mem, size_t cap) { a->base = mem; a->cap = cap; a->used = 0; }
static void *aalloc(Ctx *c, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n > c->a->cap - c->a->used) { c->line = 0; fail(c, "арена исчерпана (нужно ещё %zu байт)", n); }
    void *p = c->a->base + c->a->used;
    c->a->used += n;
    memset(p, 0, n);
    return p;
}
static const char *pstr(Ctx *c, const char *s, size_t n) {
    if (c->pool_used + n + 1 > POOLSZ) fail(c, "текст после раскрытия макросов больше лимита %u байт", POOLSZ);
    char *d = c->pool + c->pool_used;
    memcpy(d, s, n); d[n] = 0;
    c->pool_used += n + 1;
    return d;
}

static int isal(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
static int isdg(int ch) { return ch >= '0' && ch <= '9'; }
static int isid(int ch) { return isal(ch) || isdg(ch) || ch == '_' || ch == '.' || ch == '$' || ch == '@' || ch == '~' || ch == '?' || ch == '#'; }
static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static const char *sk(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static int idlen(const char *p) { int n = 0; while (isid(p[n])) n++; return n; }
static int ieq(const char *s, int len, const char *w) {
    int i = 0;
    for (; i < len; i++) if (!w[i] || lc(s[i]) != w[i]) return 0;
    return w[i] == 0;
}

/* ---------- 2. регистры, символы, секции ---------- */
static const char *g64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};
static const char *g32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
static const char *g16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
static const char *g8[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"};
static const char *h8[] = {"ah", "ch", "dh", "bh"};
static const char *sgn[] = {"es", "cs", "ss", "ds", "fs", "gs"};
static const uint8_t segpre[] = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65};

static int reg_lookup(const char *s, int len, Reg *r) {
    if (len < 2 || len > 4) return 0;
    char t[8];
    for (int i = 0; i < len; i++) t[i] = (char)lc(s[i]);
    t[len] = 0;
    r->high = 0; r->kind = RK_GPR;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(t, g64[i])) { r->num = i; r->size = 8; return 1; }
        if (!strcmp(t, g32[i])) { r->num = i; r->size = 4; return 1; }
        if (!strcmp(t, g16[i])) { r->num = i; r->size = 2; return 1; }
        if (!strcmp(t, g8[i])) { r->num = i; r->size = 1; return 1; }
    }
    for (int i = 0; i < 4; i++) if (!strcmp(t, h8[i])) { r->num = 4 + i; r->size = 1; r->high = 1; return 1; }
    for (int i = 0; i < 6; i++) if (!strcmp(t, sgn[i])) { r->kind = RK_SEG; r->num = i; r->size = 2; return 1; }
    if (t[0] == 'c' && t[1] == 'r' && isdg(t[2]) && !t[3]) {
        int n = t[2] - '0';
        if (n == 0 || n == 2 || n == 3 || n == 4 || n == 8) { r->kind = RK_CR; r->num = n; r->size = 8; return 1; }
        return 0;
    }
    if (t[0] == 'r' && isdg(t[1])) {
        int n = t[1] - '0', i = 2;
        if (isdg(t[2])) { n = n * 10 + t[2] - '0'; i = 3; }
        if (n < 8 || n > 15) return 0;
        r->num = n;
        if (!t[i]) r->size = 8;
        else if (!t[i + 1] && t[i] == 'd') r->size = 4;
        else if (!t[i + 1] && t[i] == 'w') r->size = 2;
        else if (!t[i + 1] && t[i] == 'b') r->size = 1;
        else return 0;
        return 1;
    }
    return 0;
}

static Sym *sym_find(Ctx *c, const char *nm) {
    for (int i = 0; i < c->nsy; i++) if (!strcmp(c->sy[i].name, nm)) return &c->sy[i];
    return NULL;
}
static Sym *sym_get(Ctx *c, const char *nm) {
    Sym *s = sym_find(c, nm);
    if (s) return s;
    if (c->nsy >= MAXSYM) fail(c, "слишком много символов (лимит %d)", MAXSYM);
    s = &c->sy[c->nsy++];
    s->name = pstr(c, nm, strlen(nm));
    s->seq = -1; s->defpass = -1;
    return s;
}
/* локальные метки .x получают имя области (последняя нелокальная метка); имена на ".." (например ..@3.x) глобальны */
static void qualify(Ctx *c, const char *s, int len, char *out, size_t cap) {
    if (len + (int)strlen(c->scope) + 1 >= (int)cap) fail(c, "слишком длинное имя метки");
    out[0] = 0;
    if (s[0] == '.' && s[1] != '.' && len > 1) strcpy(out, c->scope);
    size_t n = strlen(out);
    memcpy(out + n, s, (size_t)len);
    out[n + (size_t)len] = 0;
}

static Sec *cursec(Ctx *c) {
    if (c->cur <= 0) fail(c, "нет текущей секции (перед кодом и данными нужна директива section)");
    return &c->sec[c->cur - 1];
}

/* ---------- 3. препроцессор ---------- */
static void strip_comment(char *s) {
    char q = 0;
    for (; *s; s++) {
        if (q) { if (*s == q) q = 0; }
        else if (*s == '"' || *s == '\'') q = *s;
        else if (*s == ';') { *s = 0; return; }
    }
}
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static Def *def_find(Ctx *c, const char *s, int len) {
    for (int i = 0; i < c->ndef; i++) if ((int)strlen(c->defs[i].name) == len && !memcmp(c->defs[i].name, s, (size_t)len)) return &c->defs[i];
    return NULL;
}
/* разбить s по запятым верхнего уровня (скобки и кавычки учтены), на месте; возвращает число частей */
static int split_args(char *s, char **argv, int max) {
    int n = 0, d = 0; char q = 0;
    s = trim(s);
    if (!*s) return 0;
    argv[n++] = s;
    for (; *s; s++) {
        if (q) { if (*s == q) q = 0; continue; }
        if (*s == '"' || *s == '\'') q = *s;
        else if (*s == '(' || *s == '[') d++;
        else if (*s == ')' || *s == ']') d--;
        else if (*s == ',' && d == 0) {
            *s = 0;
            if (n >= max) return -1;
            argv[n++] = s + 1;
        }
    }
    for (int i = 0; i < n; i++) argv[i] = trim(argv[i]);
    return n;
}

#define APP(ch) do { if (n + 1 >= cap) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE); out[n++] = (ch); } while (0)
static void expand(Ctx *c, const char *in, char *out, size_t cap, int depth) {
    size_t n = 0;
    const char *p = in;
    if (depth > 12) fail(c, "слишком глубокая вложенность %%define (рекурсия?)");
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p; APP(*p++);
            while (*p && *p != q) APP(*p++);
            if (*p) APP(*p++);
            continue;
        }
        if (!isid(*p)) { APP(*p++); continue; }
        const char *s = p;
        int numtok = isdg(*p);
        while (isid(*p)) p++;
        int len = (int)(p - s);
        Def *d = numtok ? NULL : def_find(c, s, len);
        if (!d) { for (int i = 0; i < len; i++) APP(s[i]); continue; }
        char tmp[MAXLINE];
        if (d->np < 0) {
            expand(c, d->body, tmp, sizeof tmp, depth + 1);
        } else {
            const char *q = sk(p);
            if (*q != '(') { for (int i = 0; i < len; i++) APP(s[i]); continue; }
            char argbuf[MAXLINE], *argv[8];
            const char *a0 = ++q; int dd = 1;
            while (*q && dd) { if (*q == '(') dd++; else if (*q == ')') dd--; q++; }
            if (dd) fail(c, "не закрыта скобка в вызове %s(", d->name);
            size_t al = (size_t)(q - 1 - a0);
            if (al >= sizeof argbuf) fail(c, "аргумент макроса слишком длинный");
            memcpy(argbuf, a0, al); argbuf[al] = 0;
            int na = split_args(argbuf, argv, 8);
            if (na < 0 || na != d->np) fail(c, "%s: ожидается аргументов %d", d->name, d->np);
            char sub[MAXLINE]; size_t m = 0;
            for (const char *b = d->body; *b;) {
                if (isid(*b)) {
                    const char *bs = b; while (isid(*b)) b++;
                    int bl = (int)(b - bs), hit = -1;
                    for (int i = 0; i < d->np; i++) if ((int)strlen(d->par[i]) == bl && !memcmp(d->par[i], bs, (size_t)bl)) hit = i;
                    const char *src = hit >= 0 ? argv[hit] : bs;
                    size_t sl = hit >= 0 ? strlen(argv[hit]) : (size_t)bl;
                    if (m + sl + 1 >= sizeof sub) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE);
                    memcpy(sub + m, src, sl); m += sl;
                } else {
                    if (m + 2 >= sizeof sub) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE);
                    sub[m++] = *b++;
                }
            }
            sub[m] = 0;
            expand(c, sub, tmp, sizeof tmp, depth + 1);
            p = q;
        }
        for (const char *t = tmp; *t; t++) APP(*t);
    }
    out[n] = 0;
}

static void emit_stmt(Ctx *c, const char *t) {
    if (c->nst >= MAXSTMT) fail(c, "слишком много строк после раскрытия (лимит %d)", MAXSTMT);
    c->st[c->nst].txt = pstr(c, t, strlen(t));
    c->st[c->nst].line = c->line;
    c->nst++;
}

static void pp_line(Ctx *c, const char *raw, int depth) {
    char buf[MAXLINE];
    size_t rl = strlen(raw);
    if (rl >= sizeof buf) fail(c, "строка длиннее %d символов", MAXLINE);
    memcpy(buf, raw, rl + 1);
    strip_comment(buf);
    char *s = trim(buf);
    if (c->inmac) {
        if (!strncmp(s, "%endmacro", 9)) {
            if (c->nmac >= MAXMAC) fail(c, "слишком много макросов");
            Mac *m = &c->mac[c->nmac++];
            m->name = c->curname; m->np = c->curnp; m->nl = c->mn;
            m->lines = aalloc(c, sizeof(char *) * (size_t)(c->mn + 1));
            for (int i = 0; i < c->mn; i++) m->lines[i] = c->mlines[i];
            c->inmac = 0;
        } else if (s[0] == '%' && !strncmp(s, "%macro", 6)) {
            fail(c, "вложенное определение %%macro не поддержано");
        } else {
            if (c->mn >= MAXMACLINES) fail(c, "тело макроса длиннее %d строк", MAXMACLINES);
            c->mlines[c->mn++] = pstr(c, s, strlen(s));
        }
        return;
    }
    if (!*s) return;
    if (*s == '%') {
        char w[16]; int i = 1, k = 0;
        while (isal(s[i]) && k < 15) w[k++] = (char)lc(s[i++]);
        w[k] = 0;
        char *rest = trim(s + i);
        if (!strcmp(w, "define")) {
            if (c->ifdepth) fail(c, "%%define внутри %%if не поддержан");
            if (c->ndef >= MAXDEF) fail(c, "слишком много %%define");
            int nl = idlen(rest);
            if (!nl) fail(c, "%%define без имени");
            Def d; memset(&d, 0, sizeof d);
            char nm[128];
            if (nl >= (int)sizeof nm) fail(c, "слишком длинное имя");
            memcpy(nm, rest, (size_t)nl); nm[nl] = 0;
            d.name = pstr(c, nm, (size_t)nl); d.np = -1;
            const char *q = rest + nl;
            if (*q == '(') {
                char pb[MAXLINE]; const char *e = strchr(q, ')');
                if (!e) fail(c, "%%define: нет ')' в списке параметров");
                size_t pl = (size_t)(e - q - 1);
                if (pl >= sizeof pb) fail(c, "слишком длинный список параметров");
                memcpy(pb, q + 1, pl); pb[pl] = 0;
                char *av[8];
                int na = split_args(pb, av, 8);
                if (na < 0) fail(c, "%%define: слишком много параметров");
                for (int j = 0; j < na; j++) d.par[j] = pstr(c, av[j], strlen(av[j]));
                d.np = na;
                q = e + 1;
            }
            q = sk(q);
            d.body = pstr(c, q, strlen(q));
            for (int j = 0; j < c->ndef; j++) if (!strcmp(c->defs[j].name, d.name)) { c->defs[j] = d; return; }
            c->defs[c->ndef++] = d;
        } else if (!strcmp(w, "macro")) {
            if (c->ifdepth) fail(c, "%%macro внутри %%if не поддержан");
            int nl = idlen(rest);
            if (!nl) fail(c, "%%macro без имени");
            c->curname = pstr(c, rest, (size_t)nl);
            const char *q = sk(rest + nl);
            if (!isdg(*q)) fail(c, "%%macro: после имени нужно число параметров");
            int np = 0;
            while (isdg(*q) && np < 100) np = np * 10 + *q++ - '0';
            if (*sk(q) || np > 9) fail(c, "%%macro: поддержано только число параметров 0..9");
            c->curnp = np; c->mn = 0; c->inmac = 1; c->macline = c->line;
        } else if (!strcmp(w, "if") || !strcmp(w, "else") || !strcmp(w, "endif") || !strcmp(w, "error")) {
            char ex[MAXLINE], line[2 * MAXLINE];
            if (!strcmp(w, "error")) { snprintf(line, sizeof line, "%%error %s", rest); emit_stmt(c, line); return; }
            expand(c, rest, ex, sizeof ex, 0);
            if (!strcmp(w, "if")) { c->ifdepth++; if (!*ex) fail(c, "%%if без условия"); }
            else if (!strcmp(w, "endif")) { if (!c->ifdepth) fail(c, "%%endif без %%if"); c->ifdepth--; }
            else if (!c->ifdepth) fail(c, "%%else без %%if");
            snprintf(line, sizeof line, "%%%s %s", w, ex);
            emit_stmt(c, line);
        } else if (!strcmp(w, "endmacro")) {
            fail(c, "%%endmacro без %%macro");
        } else {
            fail(c, "директива препроцессора %%%s не поддержана", w);
        }
        return;
    }
    /* метка перед макровызовом: "name: MACRO args" */
    const char *body = s;
    int ll = idlen(s);
    if (ll && s[ll] == ':') {
        const char *after = sk(s + ll + 1);
        int wl = idlen(after);
        for (int i = 0; i < c->nmac; i++) if ((int)strlen(c->mac[i].name) == wl && !memcmp(c->mac[i].name, after, (size_t)wl)) {
            char lab[MAXLINE]; memcpy(lab, s, (size_t)ll + 1); lab[ll + 1] = 0;
            emit_stmt(c, lab);
            body = after;
            break;
        }
    }
    int wl = idlen(body);
    Mac *m = NULL;
    for (int i = 0; wl && i < c->nmac; i++) if ((int)strlen(c->mac[i].name) == wl && !memcmp(c->mac[i].name, body, (size_t)wl)) m = &c->mac[i];
    if (m) {
        if (depth > 8) fail(c, "слишком глубокая вложенность макросов");
        char ab[MAXLINE], *av[9];
        snprintf(ab, sizeof ab, "%s", body + wl);
        int na = split_args(ab, av, 9);
        if (na < 0 || na != m->np) fail(c, "макрос %s: ожидается параметров %d", m->name, m->np);
        int id = ++c->macid;
        for (int li = 0; li < m->nl; li++) {
            char sub[MAXLINE]; size_t o = 0;
            for (const char *b = m->lines[li]; *b;) {
                const char *ins = NULL; size_t il = 0; char tmp[64];
                if (b[0] == '%' && b[1] == '%' && (isal(b[2]) || b[2] == '_')) {
                    int k = 2; while (isal(b[k]) || isdg(b[k]) || b[k] == '_') k++;
                    il = (size_t)snprintf(tmp, sizeof tmp, "..@%d.", id);
                    if (o + il + (size_t)k >= sizeof sub) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE);
                    memcpy(sub + o, tmp, il); o += il;
                    memcpy(sub + o, b + 2, (size_t)k - 2); o += (size_t)k - 2;
                    b += k;
                    continue;
                }
                if (b[0] == '%' && isdg(b[1])) {
                    int ai = b[1] - '0';
                    if (ai < 1 || ai > m->np) fail(c, "макрос %s: нет параметра %%%d", m->name, ai);
                    ins = av[ai - 1]; il = strlen(ins); b += 2;
                    if (o + il + 1 >= sizeof sub) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE);
                    memcpy(sub + o, ins, il); o += il;
                    continue;
                }
                if (o + 2 >= sizeof sub) fail(c, "строка длиннее %d символов после раскрытия", MAXLINE);
                sub[o++] = *b++;
            }
            sub[o] = 0;
            pp_line(c, sub, depth + 1);
        }
        return;
    }
    char ex[MAXLINE];
    expand(c, body, ex, sizeof ex, 0);
    if (*sk(ex)) emit_stmt(c, ex);
}

/* ---------- 4. выражения ---------- */
static Val vabs(int64_t x) {
    Val v; memset(&v, 0, sizeof v);
    v.r[0] = v.r[1] = -1; v.v = x;
    return v;
}
static int has_reg(const Val *v) { return v->r[0] >= 0; }

static Val v_regmerge(Ctx *c, Val a, const Val *b) {
    for (int i = 0; i < 2 && b->r[i] >= 0; i++) {
        if (a.rsz && a.rsz != b->rsz) fail(c, "в адресе смешаны регистры разной ширины");
        int slot = a.r[0] < 0 ? 0 : (a.r[1] < 0 ? 1 : -1);
        if (slot < 0) fail(c, "в адресе больше двух регистров");
        a.r[slot] = b->r[i]; a.rs[slot] = b->rs[i]; a.rsz = b->rsz;
    }
    return a;
}

static Val eval_bin(Ctx *c, const char **pp, int minp, int mem);

static int parse_num(Ctx *c, const char *s, int len, int64_t *out) {
    uint64_t v = 0; int base = 10, i = 0, end = len;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (len > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B') ) { base = 2; i = 2; }
    else if (len > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'q' || s[1] == 'O' || s[1] == 'Q')) { base = 8; i = 2; }
    else if (len > 1 && (s[len - 1] == 'h' || s[len - 1] == 'H')) { base = 16; end = len - 1; }
    else if (len > 1 && (s[len - 1] == 'q' || s[len - 1] == 'Q' || s[len - 1] == 'o' || s[len - 1] == 'O')) { base = 8; end = len - 1; }
    else if (len > 1 && (s[len - 1] == 'b' || s[len - 1] == 'B')) { base = 2; end = len - 1; }
    else if (len > 1 && (s[len - 1] == 'd' || s[len - 1] == 'D')) { end = len - 1; }
    if (i >= end) return 0;
    for (; i < end; i++) {
        int ch = s[i], d;
        if (ch == '_') continue;
        if (isdg(ch)) d = ch - '0'; else if (isal(ch)) d = lc(ch) - 'a' + 10; else return 0;
        if (d >= base) return 0;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) fail(c, "число слишком велико");
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = (int64_t)v;
    return 1;
}

static Val eval_unary(Ctx *c, const char **pp, int mem) {
    const char *p = sk(*pp);
    Val v;
    if (*p == '-' || *p == '+' || *p == '~' || *p == '!') {
        char op = *p;
        *pp = p + 1;
        v = eval_unary(c, pp, mem);
        if (has_reg(&v)) fail(c, "унарная операция над регистром");
        if (op == '+') return v;
        if (v.k != V_ABS) { soft(c, "унарная операция над перемещаемым значением"); return vabs(0); }
        v.v = op == '-' ? (int64_t)(0 - (uint64_t)v.v) : op == '~' ? ~v.v : !v.v;
        return v;
    }
    if (*p == '(') {
        *pp = p + 1;
        v = eval_bin(c, pp, 1, mem);
        p = sk(*pp);
        if (*p != ')') fail(c, "ожидалась ')' в выражении");
        *pp = p + 1;
        return v;
    }
    if (*p == '$') {
        int dd = p[1] == '$';
        if (!isid(p[1 + dd]) || dd) {
            *pp = p + 1 + dd;
            Sec *s = cursec(c);
            v = vabs(dd ? 0 : s->off);
            v.k = V_SEC; v.ref = c->cur;
            return v;
        }
    }
    if (isdg(*p)) {
        int l = idlen(p); int64_t n;
        if (!parse_num(c, p, l, &n)) fail(c, "неверное число '%.*s'", l, p);
        *pp = p + l;
        return vabs(n);
    }
    if (*p == '\'' || *p == '"') {
        char q = *p; int64_t n = 0; int k = 0;
        p++;
        while (*p && *p != q) { if (k >= 8) fail(c, "символьная константа длиннее 8 байт"); n |= (int64_t)(uint8_t)*p << (8 * k++); p++; }
        if (*p != q) fail(c, "не закрыта кавычка");
        *pp = p + 1;
        return vabs(n);
    }
    int l = idlen(p);
    if (!l) fail(c, "ожидалось выражение вместо '%.12s'", *p ? p : "конец строки");
    *pp = p + l;
    Reg r;
    if (reg_lookup(p, l, &r)) {
        if (!mem || r.kind != RK_GPR) fail(c, "регистр '%.*s' недопустим в выражении", l, p);
        v = vabs(0); v.r[0] = r.num; v.rs[0] = 1; v.rsz = r.size;
        return v;
    }
    char nm[256];
    qualify(c, p, l, nm, sizeof nm);
    Sym *y = sym_find(c, nm);
    if (!y || y->kind == K_UNDEF) {
        soft(c, "неопределённая метка '%s'", nm);
        v = vabs(0); v.k = V_SEC; v.ref = c->cur; v.unk = 1;
        return v;
    }
    v = vabs(y->val);
    if (y->kind == K_LABEL) { v.k = V_SEC; v.ref = y->sec; }
    else if (y->kind == K_EXT) { v.k = V_EXT; v.ref = (int)(y - c->sy); v.v = 0; y->used = 1; }
    return v;
}

static int binop(const char *p, int *len, int *prec) {
    static const struct { const char *t; int prec; } T[] = {
        {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<>", 3}, {"<=", 3}, {">=", 3}, {"<<", 7}, {">>", 7}, {"//", 9}, {"%%", 9},
        {"=", 3}, {"<", 3}, {">", 3}, {"|", 4}, {"^", 5}, {"&", 6}, {"+", 8}, {"-", 8}, {"*", 9}, {"/", 9}, {"%", 9}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        size_t l = strlen(T[i].t);
        if (!strncmp(p, T[i].t, l)) { *len = (int)l; *prec = T[i].prec; return (int)i; }
    }
    return -1;
}

static Val combine(Ctx *c, const char *op, Val a, Val b) {
    int ar = has_reg(&a), br = has_reg(&b);
    if (!strcmp(op, "+")) {
        if (a.k != V_ABS && b.k != V_ABS) { soft(c, "сумма двух адресов не поддержана"); return vabs(0); }
        Val r = (a.k != V_ABS) ? a : b;
        if (ar || br) r = v_regmerge(c, v_regmerge(c, vabs(0), &a), &b);
        if (a.k != V_ABS) { r.k = a.k; r.ref = a.ref; } else { r.k = b.k; r.ref = b.ref; }
        r.v = (int64_t)((uint64_t)a.v + (uint64_t)b.v);
        r.unk = a.unk || b.unk; r.wplt = a.wplt || b.wplt;
        return r;
    }
    if (!strcmp(op, "-")) {
        if (br) fail(c, "вычитание регистра из адреса не поддержано");
        Val r = a;
        r.v = (int64_t)((uint64_t)a.v - (uint64_t)b.v);
        r.unk = a.unk || b.unk;
        if (b.k == V_ABS) return r;
        if (a.k == b.k && a.ref == b.ref && !ar) { r.k = V_ABS; r.ref = 0; return r; }
        soft(c, "разность адресов из разных секций или символов не поддержана");
        return vabs(0);
    }
    if (!strcmp(op, "*") && (ar || br)) {
        Val *rg = ar ? &a : &b, *k = ar ? &b : &a;
        if (k->k != V_ABS || has_reg(k) || rg->v || rg->k != V_ABS || rg->r[1] >= 0) fail(c, "неверное масштабирование регистра в адресе");
        rg->rs[0] *= (int)k->v;
        return *rg;
    }
    if (ar || br) fail(c, "операция '%s' над регистром не поддержана", op);
    if (a.k != V_ABS || b.k != V_ABS) { soft(c, "операция '%s' над перемещаемым значением не поддержана", op); return vabs(0); }
    int64_t x = a.v, y = b.v, z = 0;
    if (!strcmp(op, "*")) z = (int64_t)((uint64_t)x * (uint64_t)y);
    else if (!strcmp(op, "/") || !strcmp(op, "//")) { if (!y) { soft(c, "деление на ноль"); y = 1; } z = y == -1 ? (int64_t)(0 - (uint64_t)x) : x / y; }
    else if (!strcmp(op, "%") || !strcmp(op, "%%")) { if (!y) { soft(c, "деление на ноль"); y = 1; } z = y == -1 ? 0 : x % y; }
    else if (!strcmp(op, "<<")) z = y >= 64 || y < 0 ? 0 : (int64_t)((uint64_t)x << y);
    else if (!strcmp(op, ">>")) z = y >= 64 || y < 0 ? 0 : (int64_t)((uint64_t)x >> y);
    else if (!strcmp(op, "&")) z = x & y;
    else if (!strcmp(op, "|")) z = x | y;
    else if (!strcmp(op, "^")) z = x ^ y;
    else if (!strcmp(op, "&&")) z = x && y;
    else if (!strcmp(op, "||")) z = x || y;
    else if (!strcmp(op, "==") || !strcmp(op, "=")) z = x == y;
    else if (!strcmp(op, "!=") || !strcmp(op, "<>")) z = x != y;
    else if (!strcmp(op, "<")) z = x < y;
    else if (!strcmp(op, ">")) z = x > y;
    else if (!strcmp(op, "<=")) z = x <= y;
    else if (!strcmp(op, ">=")) z = x >= y;
    Val r = vabs(z);
    r.unk = a.unk || b.unk;
    return r;
}

static Val eval_bin(Ctx *c, const char **pp, int minp, int mem) {
    Val l = eval_unary(c, pp, mem);
    for (;;) {
        const char *p = sk(*pp);
        int len, prec, i = binop(p, &len, &prec);
        if (i < 0 || prec < minp) break;
        char op[3]; memcpy(op, p, (size_t)len); op[len] = 0;
        *pp = p + len;
        Val r = eval_bin(c, pp, prec + 1, mem);
        l = combine(c, op, l, r);
    }
    return l;
}
/* выражение + необязательный "wrt ..plt" */
static Val eval_top(Ctx *c, const char **pp, int mem) {
    Val v = eval_bin(c, pp, 1, mem);
    const char *p = sk(*pp);
    if (ieq(p, idlen(p), "wrt")) {
        p = sk(p + 3);
        if (strncmp(p, "..plt", 5) || isid(p[5])) fail(c, "после wrt поддержано только ..plt");
        v.wplt = 1;
        *pp = p + 5;
    }
    return v;
}

/* ---------- 5. кодировщик команд ---------- */
typedef struct { int t, rk, rn, rsz, high, szkw, jk, seg, rel; Val v, v2; } Opd;
typedef struct {
    int rep, lock, seg, p66, p67, rex, rexreq, nohi;
    uint8_t op[4]; int nop;
    int hasm, modrm, hass, sib;
    int dsz; int64_t dv; RelReq dr;
    int isz; int64_t iv; RelReq ir;
    int isz2; int64_t iv2;
} Enc;
typedef struct { int lock, rep, rexw, p66, p67; } Pre;

static void emit_bytes(Ctx *c, const uint8_t *b, size_t n) {
    Sec *s = cursec(c);
    if (s->nobits) fail(c, "код и данные в секции nobits (%s) не поддержаны; используйте res*", s->name);
    if (c->final) {
        if ((size_t)s->off + n > s->cap) fail(c, "внутренняя ошибка: размер секции %s изменился на последнем проходе", s->name);
        memcpy(s->data + s->off, b, n);
    }
    s->off += (int64_t)n;
    if (s->off > ((int64_t)1 << 28)) fail(c, "секция %s больше 256 МиБ", s->name);
}
static void addrel(Ctx *c, int64_t off, int type, const Val *v, int64_t addend) {
    if (c->nrel >= MAXREL) fail(c, "слишком много перемещений (лимит %d)", MAXREL);
    if (c->final) {
        Rel *r = &c->rel[c->nrel];
        r->sec = c->cur; r->off = (uint64_t)off; r->type = type;
        r->symk = v->k == V_EXT; r->ref = v->ref; r->addend = addend;
        c->sec[c->cur - 1].nrel++;
    }
    c->nrel++;
}
static int64_t sub64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static int fitsn(int64_t v, int n) {
    if (n == 1) return v >= -128 && v <= 255;
    if (n == 2) return v >= -32768 && v <= 65535;
    if (n == 4) return v >= -2147483648LL && v <= 4294967295LL;
    return 1;
}
static void putle(uint8_t *b, int64_t v, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)((uint64_t)v >> (8 * i)); }

/* записать поле инструкции: константа, либо перемещение, либо значение, вычисленное тут (метка той же секции) */
static void field(Ctx *c, uint8_t *b, int pos, int size, int64_t cv, const RelReq *r, int total) {
    int64_t base = cursec(c)->off;
    if (!r->on) {
        if (!fitsn(cv, size)) soft(c, "значение %lld не помещается в %d байт", (long long)cv, size);
        putle(b + pos, cv, size);
        return;
    }
    const Val *v = &r->v;
    if (v->k == V_ABS) {
        if (r->pcrel) { soft(c, "относительная ссылка на абсолютный адрес не поддержана"); return; }
        if (!fitsn(v->v, size)) soft(c, "значение %lld не помещается в %d байт", (long long)v->v, size);
        putle(b + pos, v->v, size);
        return;
    }
    if (r->pcrel) {
        if (v->k == V_SEC && v->ref == c->cur && !v->wplt) {
            int64_t d = sub64(v->v, base + total);
            int lo = size == 1 ? -128 : size == 2 ? -32768 : -2147483648LL, hi = size == 1 ? 127 : size == 2 ? 32767 : 2147483647LL;
            if (d < lo || d > hi) soft(c, "смещение перехода %lld не помещается в %d байт", (long long)d, size);
            putle(b + pos, d, size);
            return;
        }
        if (size != 4) { soft(c, "переход на другую секцию или внешний символ требует 32-битного смещения"); return; }
        addrel(c, base + pos, v->wplt ? R_PLT32 : R_PC32, v, sub64(v->v, total - pos));
        return;
    }
    if (size == 8) addrel(c, base + pos, R_64, v, v->v);
    else if (size == 4) addrel(c, base + pos, r->sext ? R_32S : R_32, v, v->v);
    else soft(c, "перемещение размером %d байт не поддержано (только 4 и 8)", size);
}

static void finish(Ctx *c, Enc *e) {
    uint8_t b[40];
    memset(b, 0, sizeof b);
    int n = 0;
    if (e->rep) b[n++] = (uint8_t)e->rep;
    if (e->lock) b[n++] = 0xF0;
    if (e->seg) b[n++] = (uint8_t)e->seg;
    if (e->p66) b[n++] = 0x66;
    if (e->p67) b[n++] = 0x67;
    if (e->rex || e->rexreq) {
        if (c->bits != 64) fail(c, "REX недоступен вне 64-битного режима");
        if (e->nohi) fail(c, "регистры ah/bh/ch/dh несовместимы с REX");
        b[n++] = (uint8_t)(0x40 | e->rex);
    }
    for (int i = 0; i < e->nop; i++) b[n++] = e->op[i];
    if (e->hasm) b[n++] = (uint8_t)e->modrm;
    if (e->hass) b[n++] = (uint8_t)e->sib;
    int dpos = n; n += e->dsz;
    int ipos = n; n += e->isz;
    int i2pos = n; n += e->isz2;
    if (n > 32) fail(c, "инструкция длиннее допустимого");
    if (e->dsz) field(c, b, dpos, e->dsz, e->dv, &e->dr, n);
    if (e->isz) field(c, b, ipos, e->isz, e->iv, &e->ir, n);
    if (e->isz2) field(c, b, i2pos, e->isz2, e->iv2, &(RelReq){0}, n);
    emit_bytes(c, b, (size_t)n);
}

static void rexbit(Ctx *c, Enc *e, int num, int bit) {
    if (num >= 8) {
        if (c->bits != 64) fail(c, "регистры r8-r15 доступны только в 64-битном режиме");
        e->rex |= bit;
    }
}
static void reg8chk(Enc *e, const Opd *o) {
    if (o->t == O_REG && o->rk == RK_GPR && o->rsz == 1) {
        if (o->high) e->nohi = 1;
        else if (o->rn >= 4 && o->rn < 8) e->rexreq = 1;
    }
}
static void osz(Ctx *c, Enc *e, int sz) {
    if (sz == 8) { if (c->bits != 64) fail(c, "64-битный операнд доступен только в режиме bits 64"); e->rex |= 8; }
    else if (sz == 2) { if (c->bits != 16) e->p66 = 1; }
    else if (sz == 4) { if (c->bits == 16) e->p66 = 1; }
}
static void op1(Enc *e, int a) { e->op[e->nop++] = (uint8_t)a; }

static void enc_mem(Ctx *c, Enc *e, const Opd *m, int regf) {
    const Val *v = &m->v;
    int base = -1, idx = -1, sc = 1;
    if (v->r[0] >= 0) {
        if (v->r[1] < 0) {
            if (v->rs[0] == 1) base = v->r[0]; else { idx = v->r[0]; sc = v->rs[0]; }
        } else if (v->rs[0] == 1) { base = v->r[0]; idx = v->r[1]; sc = v->rs[1]; }
        else if (v->rs[1] == 1) { base = v->r[1]; idx = v->r[0]; sc = v->rs[0]; }
        else fail(c, "в адресе два масштабированных регистра");
    }
    if (m->seg) e->seg = segpre[m->seg - 1];
    int rf = (regf & 7) << 3, isrel = v->k != V_ABS;
    e->hasm = 1;
    e->dv = v->v;
    if (base < 0 && idx < 0) {
        if (e->p67) fail(c, "не поддержано: префикс a32 при адресе без регистров");
        if (isrel) { e->dr.on = 1; e->dr.v = *v; }
        if (c->bits == 64) {
            if (m->rel) { e->modrm = rf | 5; e->dsz = 4; e->dr.on = 1; e->dr.v = *v; e->dr.pcrel = 1; return; }
            e->modrm = rf | 4; e->hass = 1; e->sib = 0x25; e->dsz = 4; e->dr.sext = 1;
        } else if (c->bits == 32) { e->modrm = rf | 5; e->dsz = 4; }
        else { e->modrm = rf | 6; e->dsz = 2; }
        return;
    }
    if (c->bits == 16) fail(c, "не поддержано: 16-битная адресация с регистрами");
    if (v->rsz == 4) { if (c->bits == 64) e->p67 = 1; }
    else if (v->rsz == 8) {
        if (c->bits != 64) fail(c, "64-битные регистры в адресе только в режиме bits 64");
        if (e->p67) fail(c, "префикс a32 несовместим с 64-битными регистрами в адресе");
    }
    else fail(c, "в адресе допустимы только 32- и 64-битные регистры");
    if (idx == 4) fail(c, "rsp/esp нельзя использовать индексом");
    if (sc != 1 && sc != 2 && sc != 4 && sc != 8) fail(c, "масштаб должен быть 1, 2, 4 или 8");
    if (idx >= 0) rexbit(c, e, idx, 2);
    if (base >= 0) rexbit(c, e, base, 1);
    int mod, dsz;
    if (base < 0) { mod = 0; dsz = 4; }
    else if (isrel) { mod = 2; dsz = 4; }
    else if (v->v == 0 && (base & 7) != 5) { mod = 0; dsz = 0; }
    else if (v->v >= -128 && v->v <= 127) { mod = 1; dsz = 1; }
    else { mod = 2; dsz = 4; }
    int sib = idx >= 0 || base < 0 || (base & 7) == 4;
    e->modrm = (mod << 6) | rf | (sib ? 4 : (base & 7));
    if (sib) {
        e->hass = 1;
        e->sib = ((sc == 8 ? 3 : sc == 4 ? 2 : sc == 2 ? 1 : 0) << 6) | ((idx >= 0 ? idx & 7 : 4) << 3) | (base < 0 ? 5 : (base & 7));
    }
    e->dsz = dsz;
    if (isrel && dsz) { e->dr.on = 1; e->dr.v = *v; e->dr.sext = c->bits == 64 && !e->p67; }
}
/* ModRM: reg-поле regf (число регистра или расширение /n), r/m - регистр или память */
static void rm_gen(Ctx *c, Enc *e, int regf, const Opd *rm) {
    e->hasm = 1;
    rexbit(c, e, regf, 4);
    if (rm->t == O_REG) { e->modrm = 0xC0 | ((regf & 7) << 3) | (rm->rn & 7); rexbit(c, e, rm->rn, 1); reg8chk(e, rm); }
    else enc_mem(c, e, rm, regf);
}
static void rm_op(Ctx *c, Enc *e, const Opd *reg, const Opd *rm) { reg8chk(e, reg); rm_gen(c, e, reg->rn, rm); }
static void set_imm(Enc *e, int sz, const Val *v, int sext) {
    e->isz = sz; e->iv = v->v;
    if (v->k != V_ABS || v->unk) { e->ir.on = 1; e->ir.v = *v; e->ir.sext = sext; }
}
static int is_const(const Val *v) { return v->k == V_ABS && !v->unk; }

static void parse_opd(Ctx *c, const char **pp, Opd *o) {
    const char *p = sk(*pp);
    memset(o, 0, sizeof *o);
    for (;;) {
        int l = idlen(p), kw = 0;
        if (ieq(p, l, "byte")) kw = 1; else if (ieq(p, l, "word")) kw = 2;
        else if (ieq(p, l, "dword")) kw = 4; else if (ieq(p, l, "qword")) kw = 8;
        else if (ieq(p, l, "short")) { o->jk = 1; p = sk(p + l); continue; }
        else if (ieq(p, l, "near")) { o->jk = 2; p = sk(p + l); continue; }
        else if (ieq(p, l, "far") || ieq(p, l, "tword") || ieq(p, l, "oword") || ieq(p, l, "strict") || ieq(p, l, "to"))
            fail(c, "не поддержано: модификатор '%.*s'", l, p);
        if (!kw) break;
        o->szkw = kw;
        p = sk(p + l);
    }
    int l = idlen(p);
    Reg r;
    if (l && reg_lookup(p, l, &r) && r.kind == RK_SEG && *sk(p + l) == ':' && *sk(sk(p + l) + 1) == '[') {
        o->seg = r.num + 1;
        p = sk(sk(p + l) + 1);
        l = 0;
    }
    if (*p == '[') {
        p = sk(p + 1);
        for (;;) {
            l = idlen(p);
            if (ieq(p, l, "rel")) o->rel = 1;
            else if (ieq(p, l, "abs")) o->rel = 0;
            else if (!ieq(p, l, "nosplit")) break;
            p = sk(p + l);
        }
        l = idlen(p);
        if (l && reg_lookup(p, l, &r) && r.kind == RK_SEG && *sk(p + l) == ':') { o->seg = r.num + 1; p = sk(sk(p + l) + 1); }
        o->v = eval_top(c, &p, 1);
        p = sk(p);
        if (*p != ']') fail(c, "ожидалась ']' в адресе");
        o->t = O_MEM;
        *pp = p + 1;
        return;
    }
    if (l && reg_lookup(p, l, &r)) {
        o->t = O_REG; o->rk = r.kind; o->rn = r.num; o->rsz = r.size; o->high = r.high;
        *pp = p + l;
        return;
    }
    o->v = eval_top(c, &p, 0);
    o->t = O_IMM;
    p = sk(p);
    if (*p == ':') {
        p++;
        o->v2 = eval_top(c, &p, 0);
        o->t = O_FAR;
    }
    *pp = p;
}

enum { M_NOARG = 1, M_STR, M_ALU, M_MOV, M_TEST, M_LEA, M_PUSH, M_POP, M_JMP, M_CALL, M_JCC, M_INCDEC, M_SHIFT,
       M_UNARY, M_LGDT, M_INT, M_MOVX, M_SETCC, M_CMOV, M_INOUT };
/* NOARG: a = байты опкода (старший первый), b = длина | 0x100 REX.W | 0x200 только bits 64.  STR: a = опкод, b = размер */
static const struct { const char *n; int k, a, b; } MT[] = {
    {"cli", M_NOARG, 0xFA, 1}, {"sti", M_NOARG, 0xFB, 1}, {"cld", M_NOARG, 0xFC, 1}, {"std", M_NOARG, 0xFD, 1},
    {"hlt", M_NOARG, 0xF4, 1}, {"nop", M_NOARG, 0x90, 1}, {"ret", M_NOARG, 0xC3, 1}, {"leave", M_NOARG, 0xC9, 1},
    {"int3", M_NOARG, 0xCC, 1}, {"pause", M_NOARG, 0xF390, 2}, {"ud2", M_NOARG, 0x0F0B, 2},
    {"pushfq", M_NOARG, 0x9C, 0x201}, {"popfq", M_NOARG, 0x9D, 0x201},
    {"iretq", M_NOARG, 0xCF, 0x301}, {"retfq", M_NOARG, 0xCB, 0x301},
    {"swapgs", M_NOARG, 0x0F01F8, 0x203}, {"syscall", M_NOARG, 0x0F05, 0x202}, {"sysret", M_NOARG, 0x0F07, 2},
    {"sysretq", M_NOARG, 0x0F07, 0x302}, {"rdmsr", M_NOARG, 0x0F32, 2}, {"wrmsr", M_NOARG, 0x0F30, 2},
    {"clac", M_NOARG, 0x0F01CA, 3}, {"stac", M_NOARG, 0x0F01CB, 3}, {"cpuid", M_NOARG, 0x0FA2, 2},
    {"rdtsc", M_NOARG, 0x0F31, 2}, {"lfence", M_NOARG, 0x0FAEE8, 3}, {"mfence", M_NOARG, 0x0FAEF0, 3},
    {"sfence", M_NOARG, 0x0FAEF8, 3}, {"wbinvd", M_NOARG, 0x0F09, 2}, {"invd", M_NOARG, 0x0F08, 2},
    {"cdq", M_NOARG, 0x99, 1}, {"cqo", M_NOARG, 0x99, 0x301}, {"cdqe", M_NOARG, 0x98, 0x301},
    {"movsb", M_STR, 0xA4, 1}, {"movsw", M_STR, 0xA5, 2}, {"movsd", M_STR, 0xA5, 4}, {"movsq", M_STR, 0xA5, 8},
    {"stosb", M_STR, 0xAA, 1}, {"stosw", M_STR, 0xAB, 2}, {"stosd", M_STR, 0xAB, 4}, {"stosq", M_STR, 0xAB, 8},
    {"lodsb", M_STR, 0xAC, 1}, {"lodsw", M_STR, 0xAD, 2}, {"lodsd", M_STR, 0xAD, 4}, {"lodsq", M_STR, 0xAD, 8},
    {"scasb", M_STR, 0xAE, 1}, {"scasw", M_STR, 0xAF, 2}, {"scasd", M_STR, 0xAF, 4}, {"scasq", M_STR, 0xAF, 8},
    {"cmpsb", M_STR, 0xA6, 1}, {"cmpsw", M_STR, 0xA7, 2}, {"cmpsd", M_STR, 0xA7, 4}, {"cmpsq", M_STR, 0xA7, 8},
    {"add", M_ALU, 0, 0}, {"or", M_ALU, 1, 0}, {"adc", M_ALU, 2, 0}, {"sbb", M_ALU, 3, 0},
    {"and", M_ALU, 4, 0}, {"sub", M_ALU, 5, 0}, {"xor", M_ALU, 6, 0}, {"cmp", M_ALU, 7, 0},
    {"mov", M_MOV, 0, 0}, {"test", M_TEST, 0, 0}, {"lea", M_LEA, 0, 0}, {"push", M_PUSH, 0, 0}, {"pop", M_POP, 0, 0},
    {"jmp", M_JMP, 0, 0}, {"call", M_CALL, 0, 0}, {"inc", M_INCDEC, 0, 0}, {"dec", M_INCDEC, 1, 0},
    {"rol", M_SHIFT, 0, 0}, {"ror", M_SHIFT, 1, 0}, {"rcl", M_SHIFT, 2, 0}, {"rcr", M_SHIFT, 3, 0},
    {"shl", M_SHIFT, 4, 0}, {"sal", M_SHIFT, 4, 0}, {"shr", M_SHIFT, 5, 0}, {"sar", M_SHIFT, 7, 0},
    {"not", M_UNARY, 2, 0}, {"neg", M_UNARY, 3, 0}, {"mul", M_UNARY, 4, 0}, {"div", M_UNARY, 6, 0}, {"idiv", M_UNARY, 7, 0},
    {"sgdt", M_LGDT, 0, 0}, {"sidt", M_LGDT, 1, 0}, {"lgdt", M_LGDT, 2, 0}, {"lidt", M_LGDT, 3, 0},
    {"int", M_INT, 0, 0}, {"movzx", M_MOVX, 0xB6, 0}, {"movsx", M_MOVX, 0xBE, 0}, {"movsxd", M_MOVX, 0x63, 0},
    {"in", M_INOUT, 0, 0}, {"out", M_INOUT, 1, 0}};
static const struct { const char *n; int cc; } CCT[] = {
    {"o", 0}, {"no", 1}, {"b", 2}, {"c", 2}, {"nae", 2}, {"ae", 3}, {"nb", 3}, {"nc", 3}, {"e", 4}, {"z", 4}, {"ne", 5}, {"nz", 5},
    {"be", 6}, {"na", 6}, {"a", 7}, {"nbe", 7}, {"s", 8}, {"ns", 9}, {"p", 10}, {"pe", 10}, {"np", 11}, {"po", 11},
    {"l", 12}, {"nge", 12}, {"ge", 13}, {"nl", 13}, {"le", 14}, {"ng", 14}, {"g", 15}, {"nle", 15}};

static int cc_of(const char *s) {
    for (unsigned i = 0; i < sizeof CCT / sizeof CCT[0]; i++) if (!strcmp(s, CCT[i].n)) return CCT[i].cc;
    return -1;
}
/* 0 - не мнемоника; иначе вид, a - параметр */
static int mnem_find(const char *w, int *a, int *b) {
    for (unsigned i = 0; i < sizeof MT / sizeof MT[0]; i++) if (!strcmp(w, MT[i].n)) { *a = MT[i].a; *b = MT[i].b; return MT[i].k; }
    int cc;
    if (w[0] == 'j' && (cc = cc_of(w + 1)) >= 0) { *a = cc; return M_JCC; }
    if (!strncmp(w, "set", 3) && (cc = cc_of(w + 3)) >= 0) { *a = cc; return M_SETCC; }
    if (!strncmp(w, "cmov", 4) && (cc = cc_of(w + 4)) >= 0) { *a = cc; return M_CMOV; }
    return 0;
}

static void chk_imm(Ctx *c, int sz, const Val *v) {
    if (!is_const(v)) return;
    if (sz == 8) { if (v->v < -2147483648LL || v->v > 2147483647LL) soft(c, "значение %lld не помещается в знаковое imm32", (long long)v->v); }
    else if (!fitsn(v->v, sz)) soft(c, "значение %lld не помещается в %d байт", (long long)v->v, sz);
}
/* допустимые размеры операнда push/pop: естественный для режима, 16 бит в 32/64, 32 бита в 16 */
static int stack_sz_ok(Ctx *c, int sz) {
    int dsz = c->bits == 64 ? 8 : c->bits == 32 ? 4 : 2;
    return sz == dsz || (sz == 2 && c->bits != 16) || (sz == 4 && c->bits == 16);
}
static void unsup(Ctx *c, const char *w) { fail(c, "не поддержано: форма операндов команды '%s'", w); }

static void do_insn(Ctx *c, const char *w, int kind, int ta, int tb, const char *p, const Pre *pre) {
    Opd o[3]; int no = 0;
    p = sk(p);
    while (*p) {
        if (no >= 3) fail(c, "слишком много операндов у '%s'", w);
        parse_opd(c, &p, &o[no++]);
        p = sk(p);
        if (*p == ',') { p = sk(p + 1); if (!*p) fail(c, "после запятой нет операнда"); continue; }
        if (*p) fail(c, "лишний текст после операндов: '%.20s'", p);
    }
    for (int i = 0; i < no; i++) if (o[i].t == O_REG && o[i].rk == RK_GPR) {
        if (o[i].rsz == 8 && c->bits != 64) fail(c, "64-битный регистр недоступен в режиме bits %d", c->bits);
        if (o[i].rn >= 8 && c->bits != 64) fail(c, "регистры r8-r15 доступны только в режиме bits 64");
    }
    Enc e; memset(&e, 0, sizeof e);
    e.rep = pre->rep; e.lock = pre->lock; e.p67 = pre->p67;
    if (pre->rexw) { if (c->bits != 64) fail(c, "o64 только в режиме bits 64"); e.rex |= 8; }
    int need = -1;
    switch (kind) {
    case M_NOARG: case M_STR: need = 0; break;
    case M_ALU: case M_MOV: case M_TEST: case M_LEA: case M_MOVX: case M_CMOV: case M_INOUT: need = 2; break;
    case M_SHIFT: need = 2; break;
    default: need = 1; break;
    }
    if (no != need) fail(c, "'%s': ожидается операндов %d, получено %d", w, need, no);
    Opd *d = &o[0], *s = &o[1];
    switch (kind) {
    case M_NOARG: {
        int n = tb & 0xFF;
        if ((tb & 0x200) && c->bits != 64) fail(c, "'%s' доступна только в режиме bits 64", w);
        if (tb & 0x100) { if (c->bits != 64) fail(c, "'%s' только bits 64", w); e.rex |= 8; }
        for (int i = 0; i < n; i++) op1(&e, (ta >> (8 * (n - 1 - i))) & 0xFF);
        break;
    }
    case M_STR:
        if (tb > 1) osz(c, &e, tb);
        op1(&e, ta);
        break;
    case M_ALU: {
        int n = ta, sz = d->t == O_REG ? d->rsz : d->szkw;
        if ((d->t == O_REG && d->rk != RK_GPR) || (s->t == O_REG && s->rk != RK_GPR)) unsup(c, w);
        if (!sz && s->t == O_REG) sz = s->rsz;
        if (!sz) fail(c, "'%s': не задан размер операнда (byte/word/dword/qword)", w);
        if ((d->t == O_MEM && d->szkw && s->t == O_REG && d->szkw != s->rsz) || (d->t == O_REG && s->t == O_REG && d->rsz != s->rsz) ||
            (s->t == O_MEM && s->szkw && s->szkw != sz)) fail(c, "'%s': размеры операндов не совпадают", w);
        if (d->t == O_IMM || d->t == O_FAR) unsup(c, w);
        osz(c, &e, sz);
        if (s->t == O_IMM) {
            chk_imm(c, sz, &s->v);
            if (sz == 1) {
                if (d->t == O_REG && d->rn == 0 && !d->high) op1(&e, (n << 3) | 4);
                else { op1(&e, 0x80); rm_gen(c, &e, n, d); }
                set_imm(&e, 1, &s->v, 0);
            } else if (is_const(&s->v) && s->v.v >= -128 && s->v.v <= 127) {
                op1(&e, 0x83); rm_gen(c, &e, n, d); set_imm(&e, 1, &s->v, 0);
            } else {
                if (d->t == O_REG && d->rn == 0) op1(&e, (n << 3) | 5);
                else { op1(&e, 0x81); rm_gen(c, &e, n, d); }
                set_imm(&e, sz == 2 ? 2 : 4, &s->v, sz == 8);
            }
        } else if (s->t == O_REG) { op1(&e, (n << 3) | (sz == 1 ? 0 : 1)); rm_op(c, &e, s, d); }
        else if (d->t == O_REG && s->t == O_MEM) { op1(&e, (n << 3) | (sz == 1 ? 2 : 3)); rm_op(c, &e, d, s); }
        else unsup(c, w);
        break;
    }
    case M_TEST: {
        int sz = d->t == O_REG ? d->rsz : d->szkw;
        if (!sz && s->t == O_REG) sz = s->rsz;
        if (!sz) fail(c, "'test': не задан размер операнда");
        if ((d->t == O_REG && d->rk != RK_GPR) || (s->t == O_REG && s->rk != RK_GPR) || d->t == O_IMM) unsup(c, w);
        if (d->t == O_REG && s->t == O_REG && d->rsz != s->rsz) fail(c, "'test': размеры операндов не совпадают");
        osz(c, &e, sz);
        if (s->t == O_IMM) {
            chk_imm(c, sz, &s->v);
            if (d->t == O_REG && d->rn == 0 && !d->high) op1(&e, sz == 1 ? 0xA8 : 0xA9);
            else { op1(&e, sz == 1 ? 0xF6 : 0xF7); rm_gen(c, &e, 0, d); }
            set_imm(&e, sz == 1 ? 1 : sz == 2 ? 2 : 4, &s->v, sz == 8);
        } else if (s->t == O_REG) { op1(&e, sz == 1 ? 0x84 : 0x85); rm_op(c, &e, s, d); }
        else if (d->t == O_REG && s->t == O_MEM) { op1(&e, sz == 1 ? 0x84 : 0x85); rm_op(c, &e, d, s); }
        else unsup(c, w);
        break;
    }
    case M_MOV: {
        if (d->t == O_REG && d->rk == RK_CR) {
            if (s->t != O_REG || s->rk != RK_GPR) unsup(c, w);
            if (s->rsz != (c->bits == 64 ? 8 : 4)) fail(c, "mov crN: в режиме bits %d нужен %d-битный регистр", c->bits, c->bits == 64 ? 64 : 32);
            op1(&e, 0x0F); op1(&e, 0x22); rm_gen(c, &e, d->rn, s);
            break;
        }
        if (s->t == O_REG && s->rk == RK_CR) {
            if (d->t != O_REG || d->rk != RK_GPR) unsup(c, w);
            if (d->rsz != (c->bits == 64 ? 8 : 4)) fail(c, "mov crN: в режиме bits %d нужен %d-битный регистр", c->bits, c->bits == 64 ? 64 : 32);
            op1(&e, 0x0F); op1(&e, 0x20); rm_gen(c, &e, s->rn, d);
            break;
        }
        if (d->t == O_REG && d->rk == RK_SEG) {
            if (!((s->t == O_REG && s->rk == RK_GPR && (s->rsz == 2 || s->rsz == 4)) || (s->t == O_MEM && (s->szkw == 0 || s->szkw == 2)))) unsup(c, w);
            op1(&e, 0x8E); rm_gen(c, &e, d->rn, s);
            break;
        }
        if (s->t == O_REG && s->rk == RK_SEG) {
            if (d->t == O_REG && d->rk == RK_GPR) osz(c, &e, d->rsz);
            else if (d->t != O_MEM || (d->szkw && d->szkw != 2)) unsup(c, w);
            op1(&e, 0x8C); rm_gen(c, &e, s->rn, d);
            break;
        }
        if ((d->t == O_REG && d->rk != RK_GPR) || d->t == O_IMM || d->t == O_FAR || s->t == O_FAR) unsup(c, w);
        int sz = d->t == O_REG ? d->rsz : d->szkw;
        if (!sz && s->t == O_REG) sz = s->rsz;
        if (!sz) fail(c, "'mov': не задан размер операнда (byte/word/dword/qword)");
        if ((d->t == O_MEM && d->szkw && s->t == O_REG && d->szkw != s->rsz) || (d->t == O_REG && s->t == O_REG && d->rsz != s->rsz) ||
            (s->t == O_MEM && s->szkw && d->t == O_REG && s->szkw != d->rsz)) fail(c, "'mov': размеры операндов не совпадают");
        if (d->t == O_MEM && s->t == O_MEM) unsup(c, w);
        osz(c, &e, sz);
        if (s->t == O_REG && s->rk == RK_GPR) {
            if (d->t == O_MEM && c->bits != 64 && s->rn == 0 && !s->high && d->v.r[0] < 0 && !d->rel) {
                op1(&e, 0xA2 | (sz != 1)); e.dsz = c->bits == 32 ? 4 : 2; e.dv = d->v.v;
                if (d->v.k != V_ABS) { e.dr.on = 1; e.dr.v = d->v; }
                if (d->seg) e.seg = segpre[d->seg - 1];
            } else { op1(&e, sz == 1 ? 0x88 : 0x89); rm_op(c, &e, s, d); }
        } else if (s->t == O_MEM) {
            if (d->t == O_REG && c->bits != 64 && d->rn == 0 && !d->high && s->v.r[0] < 0 && !s->rel) {
                op1(&e, 0xA0 | (sz != 1)); e.dsz = c->bits == 32 ? 4 : 2; e.dv = s->v.v;
                if (s->v.k != V_ABS) { e.dr.on = 1; e.dr.v = s->v; }
                if (s->seg) e.seg = segpre[s->seg - 1];
            } else { op1(&e, sz == 1 ? 0x8A : 0x8B); rm_op(c, &e, d, s); }
        } else if (d->t == O_REG) {
            const Val *v = &s->v;
            if (sz == 1) { chk_imm(c, 1, v); reg8chk(&e, d); rexbit(c, &e, d->rn, 1); op1(&e, 0xB0 + (d->rn & 7)); set_imm(&e, 1, v, 0); }
            else if (sz == 2 || sz == 4) { chk_imm(c, sz, v); rexbit(c, &e, d->rn, 1); op1(&e, 0xB8 + (d->rn & 7)); set_imm(&e, sz, v, 0); }
            else if (is_const(v) && v->v >= 0 && v->v <= 4294967295LL) {
                e.rex &= ~8; rexbit(c, &e, d->rn, 1); op1(&e, 0xB8 + (d->rn & 7)); set_imm(&e, 4, v, 0);
            } else if (is_const(v) && v->v >= -2147483648LL && v->v <= 2147483647LL) {
                op1(&e, 0xC7); rm_gen(c, &e, 0, d); set_imm(&e, 4, v, 1);
            } else { rexbit(c, &e, d->rn, 1); op1(&e, 0xB8 + (d->rn & 7)); set_imm(&e, 8, v, 0); }
        } else if (d->t == O_MEM) {
            chk_imm(c, sz, &s->v);
            op1(&e, sz == 1 ? 0xC6 : 0xC7); rm_gen(c, &e, 0, d);
            set_imm(&e, sz == 1 ? 1 : sz == 2 ? 2 : 4, &s->v, sz == 8);
        } else unsup(c, w);
        break;
    }
    case M_LEA:
        if (d->t != O_REG || d->rk != RK_GPR || d->rsz == 1 || s->t != O_MEM) unsup(c, w);
        osz(c, &e, d->rsz); op1(&e, 0x8D); rm_op(c, &e, d, s);
        break;
    case M_PUSH: case M_POP: {
        int push = kind == M_PUSH, dsz = c->bits == 64 ? 8 : c->bits == 32 ? 4 : 2;
        Opd *a = d;
        if (a->t == O_REG) {
            if (a->rk != RK_GPR || !stack_sz_ok(c, a->rsz)) fail(c, "'%s': неверный размер регистра", w);
            if (a->rsz != dsz) osz(c, &e, a->rsz);
            rexbit(c, &e, a->rn, 1);
            op1(&e, (push ? 0x50 : 0x58) + (a->rn & 7));
        } else if (a->t == O_IMM && push) {
            int sz = a->szkw ? a->szkw : dsz;
            if (!stack_sz_ok(c, sz)) fail(c, "push: размер константы %d недопустим в режиме bits %d", sz, c->bits);
            if (sz != dsz) osz(c, &e, sz);
            if (is_const(&a->v) && a->v.v >= -128 && a->v.v <= 127) { op1(&e, 0x6A); set_imm(&e, 1, &a->v, 0); }
            else { chk_imm(c, sz == 8 ? 8 : sz, &a->v); op1(&e, 0x68); set_imm(&e, sz == 2 ? 2 : 4, &a->v, c->bits == 64); }
        } else if (a->t == O_MEM) {
            int sz = a->szkw;
            if (!sz) fail(c, "'%s': не задан размер операнда (word/dword/qword)", w);
            if (!stack_sz_ok(c, sz)) fail(c, "'%s': размер операнда %d недопустим", w, sz);
            if (sz != dsz) osz(c, &e, sz);
            op1(&e, push ? 0xFF : 0x8F); rm_gen(c, &e, push ? 6 : 0, a);
        } else unsup(c, w);
        break;
    }
    case M_JMP: case M_CALL: case M_JCC: {
        Opd *t = d;
        if (t->t == O_FAR) {
            if (kind != M_JMP || c->bits == 64) fail(c, "прямой far-переход недоступен здесь");
            int osize = t->szkw ? t->szkw : 4;
            if (osize != 4) fail(c, "не поддержано: far jmp со смещением %d байт", osize);
            if (c->bits == 16) e.p66 = 1;
            if (!is_const(&t->v)) soft(c, "селектор far jmp должен быть константой");
            op1(&e, 0xEA);
            set_imm(&e, 4, &t->v2, 0);
            e.isz2 = 2; e.iv2 = t->v.v;
            break;
        }
        if (t->t == O_REG || t->t == O_MEM) {
            int want = c->bits == 64 ? 8 : c->bits == 32 ? 4 : 2;
            if (kind == M_JCC) unsup(c, w);
            if (t->t == O_REG && (t->rk != RK_GPR || t->rsz != want)) fail(c, "'%s': нужен %d-битный регистр", w, want * 8);
            if (t->t == O_MEM && t->szkw && t->szkw != want) fail(c, "'%s': размер операнда должен быть %d байт", w, want);
            op1(&e, 0xFF); rm_gen(c, &e, kind == M_JMP ? 4 : 2, t);
            break;
        }
        int osize = c->bits == 16 ? 2 : 4;
        Val *tv = &t->v;
        int same = tv->k == V_SEC && tv->ref == c->cur && !tv->wplt;
        int sh = 0;
        if (kind == M_CALL) { if (t->jk == 1) fail(c, "call short не существует"); }
        else if (t->jk == 1) sh = 1;
        else if (t->jk == 0 && !c->jnear[c->idx]) {
            if (same && (tv->unk || (sub64(tv->v, cursec(c)->off + 2) >= -128 && sub64(tv->v, cursec(c)->off + 2) <= 127))) sh = 1;
            else { c->jnear[c->idx] = 1; c->changed = 1; }
        }
        if (sh) {
            if (!same) soft(c, "short-переход на метку другой секции или внешний символ невозможен");
            op1(&e, kind == M_JMP ? 0xEB : 0x70 + ta);
            e.dsz = 1;
        } else {
            if (kind == M_CALL) op1(&e, 0xE8);
            else if (kind == M_JMP) op1(&e, 0xE9);
            else { op1(&e, 0x0F); op1(&e, 0x80 + ta); }
            e.dsz = osize;
        }
        e.dr.on = 1; e.dr.v = *tv; e.dr.pcrel = 1;
        break;
    }
    case M_INCDEC: {
        int sz = d->t == O_REG ? d->rsz : d->szkw;
        if (!sz) fail(c, "'%s': не задан размер операнда", w);
        if ((d->t != O_REG && d->t != O_MEM) || (d->t == O_REG && d->rk != RK_GPR)) unsup(c, w);
        osz(c, &e, sz);
        if (sz == 1) { op1(&e, 0xFE); rm_gen(c, &e, ta, d); }
        else if (d->t == O_REG && c->bits != 64) op1(&e, 0x40 | (ta << 3) | (d->rn & 7));
        else { op1(&e, 0xFF); rm_gen(c, &e, ta, d); }
        break;
    }
    case M_SHIFT: {
        int sz = d->t == O_REG ? d->rsz : d->szkw;
        if (!sz) fail(c, "'%s': не задан размер операнда", w);
        if ((d->t != O_REG && d->t != O_MEM) || (d->t == O_REG && d->rk != RK_GPR)) unsup(c, w);
        osz(c, &e, sz);
        if (s->t == O_IMM) {
            if (!is_const(&s->v)) soft(c, "'%s': счётчик сдвига должен быть константой", w);
            if (s->v.v == 1) { op1(&e, sz == 1 ? 0xD0 : 0xD1); rm_gen(c, &e, ta, d); }
            else { op1(&e, sz == 1 ? 0xC0 : 0xC1); rm_gen(c, &e, ta, d); set_imm(&e, 1, &s->v, 0); }
        } else if (s->t == O_REG && s->rk == RK_GPR && s->rsz == 1 && s->rn == 1 && !s->high) { op1(&e, sz == 1 ? 0xD2 : 0xD3); rm_gen(c, &e, ta, d); }
        else unsup(c, w);
        break;
    }
    case M_UNARY: {
        int sz = d->t == O_REG ? d->rsz : d->szkw;
        if (!sz) fail(c, "'%s': не задан размер операнда", w);
        if ((d->t != O_REG && d->t != O_MEM) || (d->t == O_REG && d->rk != RK_GPR)) unsup(c, w);
        osz(c, &e, sz);
        op1(&e, sz == 1 ? 0xF6 : 0xF7); rm_gen(c, &e, ta, d);
        break;
    }
    case M_LGDT:
        if (d->t != O_MEM) unsup(c, w);
        op1(&e, 0x0F); op1(&e, 0x01); rm_gen(c, &e, ta, d);
        break;
    case M_INT:
        if (d->t != O_IMM) unsup(c, w);
        chk_imm(c, 1, &d->v);
        op1(&e, 0xCD); set_imm(&e, 1, &d->v, 0);
        break;
    case M_MOVX: {
        if (d->t != O_REG || d->rk != RK_GPR || (s->t != O_REG && s->t != O_MEM) || (s->t == O_REG && s->rk != RK_GPR)) unsup(c, w);
        int ssz = s->t == O_REG ? s->rsz : s->szkw;
        if (!ssz) fail(c, "'%s': не задан размер источника", w);
        if (ta == 0x63) {
            if (d->rsz != 8 || ssz != 4) fail(c, "movsxd: поддержано r64 <- r/m32");
            osz(c, &e, 8); op1(&e, 0x63);
        } else {
            if (ssz > 2 || ssz >= d->rsz || d->rsz == 1) fail(c, "'%s': неверные размеры операндов", w);
            osz(c, &e, d->rsz); op1(&e, 0x0F); op1(&e, ta + (ssz == 2));
        }
        rm_op(c, &e, d, s);
        break;
    }
    case M_SETCC:
        if ((d->t != O_REG && d->t != O_MEM) || (d->t == O_REG && (d->rk != RK_GPR || d->rsz != 1)) || (d->t == O_MEM && d->szkw > 1)) unsup(c, w);
        op1(&e, 0x0F); op1(&e, 0x90 + ta); rm_gen(c, &e, 0, d);
        break;
    case M_CMOV:
        if (d->t != O_REG || d->rk != RK_GPR || d->rsz == 1 || (s->t != O_REG && s->t != O_MEM) || (s->t == O_REG && s->rsz != d->rsz)) unsup(c, w);
        osz(c, &e, d->rsz); op1(&e, 0x0F); op1(&e, 0x40 + ta); rm_op(c, &e, d, s);
        break;
    case M_INOUT: {
        Opd *r = ta ? s : d, *pt = ta ? d : s;
        if (r->t != O_REG || r->rk != RK_GPR || r->rn != 0 || r->high || r->rsz == 8) unsup(c, w);
        osz(c, &e, r->rsz);
        if (pt->t == O_IMM) { chk_imm(c, 1, &pt->v); op1(&e, (ta ? 0xE6 : 0xE4) | (r->rsz != 1)); set_imm(&e, 1, &pt->v, 0); }
        else if (pt->t == O_REG && pt->rk == RK_GPR && pt->rsz == 2 && pt->rn == 2) op1(&e, (ta ? 0xEE : 0xEC) | (r->rsz != 1));
        else unsup(c, w);
        break;
    }
    default:
        fail(c, "внутренняя ошибка: вид команды %d", kind);
    }
    finish(c, &e);
}

/* ---------- 6. директивы и проходы ---------- */
static void endchk(Ctx *c, const char *p) {
    p = sk(p);
    if (*p) fail(c, "лишний текст в конце строки: '%.20s'", p);
}
static void setsym(Ctx *c, Sym *y, int kind, int sec, int64_t val) {
    if (y->kind != kind || y->sec != sec || y->val != val) c->changed = 1;
    y->kind = kind; y->sec = sec; y->val = val; y->defpass = c->pass;
    if (y->seq < 0) y->seq = c->symseq++;
}
static Sym *sym_define(Ctx *c, const char *s, int len, int setscope) {
    char nm[256];
    qualify(c, s, len, nm, sizeof nm);
    if (setscope && s[0] != '.') { if (strlen(nm) >= sizeof c->scope) fail(c, "слишком длинное имя метки"); strcpy(c->scope, nm); }
    Sym *y = sym_get(c, nm);
    if (y->defpass == c->pass) fail(c, "метка '%s' определена повторно", nm);
    if (y->kind == K_EXT) fail(c, "'%s' объявлена extern и определяется в этом файле", nm);
    return y;
}

static Sec *sec_switch(Ctx *c, const char *p) {
    int l = 0;
    while (p[l] && p[l] != ' ' && p[l] != '\t') l++;
    if (!l) fail(c, "section: нет имени секции");
    if (l >= (int)sizeof c->sec[0].name) fail(c, "слишком длинное имя секции");
    char nm[48]; memcpy(nm, p, (size_t)l); nm[l] = 0;
    Sec *s = NULL;
    for (int i = 0; i < c->nsec; i++) if (!strcmp(c->sec[i].name, nm)) { s = &c->sec[i]; c->cur = i + 1; }
    if (!s) {
        if (c->nsec >= MAXSEC) fail(c, "слишком много секций (лимит %d)", MAXSEC);
        s = &c->sec[c->nsec++];
        c->cur = c->nsec;
        memcpy(s->name, nm, (size_t)l + 1);
        s->type = 1; s->flags = 2; s->align = 1;
        if (!strcmp(nm, ".text")) { s->flags = 6; s->align = 16; }
        else if (!strcmp(nm, ".rodata")) s->align = 4;
        else if (!strcmp(nm, ".data")) { s->flags = 3; s->align = 4; }
        else if (!strcmp(nm, ".bss")) { s->type = 8; s->flags = 3; s->align = 4; s->nobits = 1; }
    }
    p = sk(p + l);
    while (*p) {
        int al = idlen(p);
        if (ieq(p, al, "alloc")) s->flags |= 2;
        else if (ieq(p, al, "noalloc")) s->flags &= ~2u;
        else if (ieq(p, al, "exec")) s->flags |= 4;
        else if (ieq(p, al, "noexec")) s->flags &= ~4u;
        else if (ieq(p, al, "write")) s->flags |= 1;
        else if (ieq(p, al, "nowrite")) s->flags &= ~1u;
        else if (ieq(p, al, "progbits")) { s->type = 1; s->nobits = 0; }
        else if (ieq(p, al, "nobits")) { s->type = 8; s->nobits = 1; }
        else if (al > 6 && !strncmp(p, "align=", 6)) {
            int64_t n;
            if (!parse_num(c, p + 6, al - 6, &n) || n <= 0 || (n & (n - 1))) fail(c, "section: align= должно быть степенью двойки");
            s->align = (uint64_t)n;
        } else fail(c, "section: атрибут '%.*s' не поддержан", al, p);
        p = sk(p + al);
    }
    return s;
}

static void emit_zero(Ctx *c, int64_t n, int fill) {
    Sec *s = cursec(c);
    if (n < 0) { soft(c, "отрицательный размер %lld", (long long)n); return; }
    if (n > ((int64_t)1 << 28)) fail(c, "размер %lld слишком велик", (long long)n);
    if (s->nobits) { s->off += n; if (s->off > ((int64_t)1 << 28)) fail(c, "секция %s больше 256 МиБ", s->name); return; }
    uint8_t buf[256];
    memset(buf, fill, sizeof buf);
    while (n > 0) { size_t k = n > 256 ? 256 : (size_t)n; emit_bytes(c, buf, k); n -= (int64_t)k; }
}

static void emit_val(Ctx *c, const Val *v, int sz) {
    uint8_t b[8] = {0};
    RelReq r; memset(&r, 0, sizeof r);
    if (v->k != V_ABS) { r.on = 1; r.v = *v; }
    int64_t cv = v->v;
    if (!r.on && sz < 8 && (cv >= ((int64_t)1 << (8 * sz)) || cv <= -((int64_t)1 << (8 * sz)))) soft(c, "значение %lld не помещается в %d байт", (long long)cv, sz);
    if (!r.on) { putle(b, cv, sz); }
    else field(c, b, 0, sz, cv, &r, sz);
    emit_bytes(c, b, (size_t)sz);
}

static void do_data(Ctx *c, int sz, const char *p) {
    for (;;) {
        p = sk(p);
        if (*p == '"' || *p == '\'') {
            char q = *p; const char *e = p + 1;
            while (*e && *e != q) e++;
            if (!*e) fail(c, "не закрыта кавычка");
            const char *after = sk(e + 1);
            if (*after == ',' || !*after) {
                int n = (int)(e - p - 1);
                if (sz == 1) emit_bytes(c, (const uint8_t *)p + 1, (size_t)n);
                else {
                    if (n > sz) fail(c, "строка длиннее элемента данных");
                    Val v = vabs(0);
                    for (int i = 0; i < n; i++) v.v |= (int64_t)(uint8_t)p[1 + i] << (8 * i);
                    emit_val(c, &v, sz);
                }
                p = after;
                if (*p == ',') { p++; continue; }
                break;
            }
        }
        Val v = eval_top(c, &p, 0);
        emit_val(c, &v, sz);
        p = sk(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    endchk(c, p);
}

static void do_body(Ctx *c, const char *p);

static void do_times(Ctx *c, const char *p) {
    Val n = eval_top(c, &p, 0);
    if (n.k != V_ABS || has_reg(&n)) { soft(c, "times: счётчик должен быть константой"); n = vabs(0); }
    if (n.v < 0) { soft(c, "times: отрицательный счётчик"); n.v = 0; }
    if (n.v > (1 << 24)) fail(c, "times: счётчик %lld слишком велик", (long long)n.v);
    p = sk(p);
    if (!*p) fail(c, "times: нет повторяемой команды");
    if (c->intimes) fail(c, "не поддержано: вложенный times");
    c->intimes = 1;
    for (int64_t i = 0; i < n.v; i++) do_body(c, p);
    c->intimes = 0;
}

static void do_align(Ctx *c, const char *p) {
    Val v = eval_top(c, &p, 0);
    endchk(c, p);
    if (v.k != V_ABS || v.v <= 0 || (v.v & (v.v - 1))) { soft(c, "align: значение должно быть степенью двойки"); return; }
    Sec *s = cursec(c);
    if ((uint64_t)v.v > s->align) s->align = (uint64_t)v.v;
    int64_t pad = (v.v - (s->off & (v.v - 1))) & (v.v - 1);
    emit_zero(c, pad, 0x90);
}

static void lower_into(char *dst, size_t cap, const char *s, int l) {
    int i = 0;
    for (; i < l && (size_t)i + 1 < cap; i++) dst[i] = (char)lc(s[i]);
    dst[i] = 0;
}

static void do_body(Ctx *c, const char *p) {
    p = sk(p);
    if (!*p) return;
    Pre pre; memset(&pre, 0, sizeof pre);
    char w[24]; int l;
    for (;;) {
        l = idlen(p);
        if (!l || l >= 20) break;
        lower_into(w, sizeof w, p, l);
        if (!strcmp(w, "lock")) pre.lock = 1;
        else if (!strcmp(w, "rep") || !strcmp(w, "repe") || !strcmp(w, "repz")) pre.rep = 0xF3;
        else if (!strcmp(w, "repne") || !strcmp(w, "repnz")) pre.rep = 0xF2;
        else if (!strcmp(w, "o64")) pre.rexw = 1;
        else if (!strcmp(w, "a32")) pre.p67 = 1;
        else if (!strcmp(w, "o16") || !strcmp(w, "o32") || !strcmp(w, "a16") || !strcmp(w, "a64") || !strcmp(w, "wait"))
            fail(c, "не поддержано: префикс '%s'", w);
        else break;
        p = sk(p + l);
    }
    l = idlen(p);
    if (!l) fail(c, "ожидалась мнемоника или директива вместо '%.12s'", p);
    if (l >= 20) fail(c, "неизвестная мнемоника '%.*s'", l, p);
    lower_into(w, sizeof w, p, l);
    const char *q = sk(p + l);
    if (!strcmp(w, "bits")) {
        Val v = eval_top(c, &q, 0);
        endchk(c, q);
        if (v.k != V_ABS || (v.v != 16 && v.v != 32 && v.v != 64)) fail(c, "bits: допустимо 16, 32 или 64");
        c->bits = (int)v.v;
    } else if (!strcmp(w, "section") || !strcmp(w, "segment")) {
        sec_switch(c, q);
    } else if (!strcmp(w, "global") || !strcmp(w, "extern")) {
        int ext = w[0] == 'e';
        for (;;) {
            q = sk(q);
            int nl = idlen(q);
            if (!nl) fail(c, "%s: ожидалось имя символа", w);
            char nm[256]; qualify(c, q, nl, nm, sizeof nm);
            Sym *y = sym_get(c, nm);
            if (ext) {
                if (y->kind == K_LABEL || y->kind == K_ABS) fail(c, "'%s' уже определён в этом файле", nm);
                y->kind = K_EXT; y->defpass = c->pass;
                if (y->seq < 0) y->seq = c->symseq++;
            } else { y->glob = 1; y->line = c->line; }
            q = sk(q + nl);
            if (*q == ',') { q++; continue; }
            break;
        }
        endchk(c, q);
    } else if (!strcmp(w, "align") || !strcmp(w, "alignb")) {
        do_align(c, q);
    } else if (!strcmp(w, "times")) {
        do_times(c, q);
    } else if (!strcmp(w, "db")) do_data(c, 1, q);
    else if (!strcmp(w, "dw")) do_data(c, 2, q);
    else if (!strcmp(w, "dd")) do_data(c, 4, q);
    else if (!strcmp(w, "dq")) do_data(c, 8, q);
    else if (!strcmp(w, "resb") || !strcmp(w, "resw") || !strcmp(w, "resd") || !strcmp(w, "resq")) {
        int sz = w[3] == 'b' ? 1 : w[3] == 'w' ? 2 : w[3] == 'd' ? 4 : 8;
        Val n = eval_top(c, &q, 0);
        endchk(c, q);
        if (n.k != V_ABS || has_reg(&n)) { soft(c, "%s: размер должен быть константой", w); return; }
        if (n.v > ((int64_t)1 << 28) / sz) fail(c, "%s: размер %lld слишком велик", w, (long long)n.v);
        emit_zero(c, n.v * sz, 0);
    } else {
        int a, b, k = mnem_find(w, &a, &b);
        if (!k) fail(c, "неизвестная мнемоника '%s'", w);
        do_insn(c, w, k, a, b, q, &pre);
    }
}

static int is_data_word(const char *r, int l) {
    static const char *W[] = {"db", "dw", "dd", "dq", "resb", "resw", "resd", "resq", "times"};
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) if (ieq(r, l, W[i])) return 1;
    return 0;
}

static void do_cond(Ctx *c, const char *p) {
    int l = idlen(p + 1);
    char w[16];
    lower_into(w, sizeof w, p + 1, l);
    const char *q = sk(p + 1 + l);
    if (!strcmp(w, "if")) {
        if (c->ifn >= 32) fail(c, "%%if вложен глубже 32 уровней");
        int parent = c->skip, cond = 0;
        if (!parent) {
            Val v = eval_top(c, &q, 0);
            if (v.k != V_ABS || has_reg(&v)) soft(c, "условие %%if должно быть константой");
            cond = v.v != 0;
        }
        c->ifst[c->ifn] = parent; c->ifc[c->ifn++] = cond;
        c->skip = parent || !cond;
    } else if (!strcmp(w, "else")) {
        if (!c->ifn) fail(c, "%%else без %%if");
        c->skip = c->ifst[c->ifn - 1] || c->ifc[c->ifn - 1];
    } else if (!strcmp(w, "endif")) {
        if (!c->ifn) fail(c, "%%endif без %%if");
        c->skip = c->ifst[--c->ifn];
    } else if (!c->skip) {
        soft(c, "%%error: %s", q);
    }
}

static void do_line(Ctx *c, const char *p) {
    p = sk(p);
    if (*p == '%') { do_cond(c, p); return; }
    if (c->skip || !*p) return;
    int l = idlen(p);
    if (l) {
        if (p[l] == ':') {
            Sym *y = sym_define(c, p, l, 1);
            setsym(c, y, K_LABEL, c->cur, cursec(c)->off);
            p = sk(p + l + 1);
        } else {
            const char *r = sk(p + l);
            int l2 = idlen(r);
            if (l2 && ieq(r, l2, "equ")) {
                Sym *y = sym_define(c, p, l, 0);
                const char *e = sk(r + 3);
                Val v = eval_top(c, &e, 0);
                endchk(c, e);
                if (has_reg(&v)) fail(c, "equ: регистр в значении");
                if (v.k == V_EXT) fail(c, "не поддержано: equ со ссылкой на внешний символ");
                if (v.k == V_ABS) setsym(c, y, K_ABS, 0, v.v); else setsym(c, y, K_LABEL, v.ref, v.v);
                return;
            }
            char w[24]; int a, b;
            lower_into(w, sizeof w, p, l);
            if (l2 && is_data_word(r, l2) && l < 20 && !mnem_find(w, &a, &b)) {
                Sym *y = sym_define(c, p, l, 1);
                setsym(c, y, K_LABEL, c->cur, cursec(c)->off);
                p = r;
            }
        }
    }
    do_body(c, p);
}

static void run_pass(Ctx *c) {
    c->nrel = 0; c->cur = 0; c->bits = 64; c->scope[0] = 0; c->ifn = 0; c->skip = 0; c->intimes = 0;
    for (int i = 0; i < c->nsec; i++) { c->sec[i].off = 0; c->sec[i].nrel = 0; }
    for (int i = 0; i < c->nst; i++) {
        c->line = c->st[i].line; c->idx = i;
        do_line(c, c->st[i].txt);
    }
    if (c->ifn) { c->line = c->st[c->nst - 1].line; fail(c, "%%if без %%endif"); }
    for (int i = 0; i < c->nsec; i++) {
        if (c->sec[i].size != c->sec[i].off) c->changed = 1;
        c->sec[i].size = c->sec[i].off;
    }
}

/* ---------- 7. запись ELF64 ET_REL (раскладка как у nasm -f elf64) ---------- */
static void w16(uint8_t *b, uint16_t v) { putle(b, v, 2); }
static void w32(uint8_t *b, uint32_t v) { putle(b, v, 4); }
static void w64(uint8_t *b, uint64_t v) { putle(b, (int64_t)v, 8); }
static uint64_t al16(uint64_t x) { return (x + 15) & ~(uint64_t)15; }

static void sort_seq(Sym **a, int n) {
    for (int i = 1; i < n; i++) {
        Sym *t = a[i]; int j = i - 1;
        while (j >= 0 && a[j]->seq > t->seq) { a[j + 1] = a[j]; j--; }
        a[j + 1] = t;
    }
}

static void elf_write(Ctx *c, const uint8_t **out, size_t *outlen) {
    Sym **loc = aalloc(c, sizeof(Sym *) * (size_t)(c->nsy + 1)), **glo = aalloc(c, sizeof(Sym *) * (size_t)(c->nsy + 1));
    int *eidx = aalloc(c, sizeof(int) * (size_t)(c->nsy + 1));
    int nl = 0, ng = 0;
    for (int i = 0; i < c->nsy; i++) {
        Sym *y = &c->sy[i];
        if (y->kind == K_UNDEF) {
            if (y->glob) { c->line = y->line; fail(c, "global-символ '%s' не определён", y->name); }
            continue;
        }
        if (y->kind == K_EXT && !y->used && !y->glob) continue;   /* как nasm: неиспользованный extern в таблицу не попадает */
        if (y->kind == K_EXT || y->glob) glo[ng++] = y;
        else loc[nl++] = y;
    }
    sort_seq(loc, nl); sort_seq(glo, ng);
    int nsec = c->nsec, first_glob = 2 + nsec + nl;
    for (int i = 0; i < nl; i++) eidx[loc[i] - c->sy] = 2 + nsec + i;
    for (int i = 0; i < ng; i++) eidx[glo[i] - c->sy] = first_glob + i;
    int nsyms = first_glob + ng;
    /* строки */
    size_t strsz = 2 + strlen(c->file);
    for (int i = 0; i < nl; i++) strsz += strlen(loc[i]->name) + 1;
    for (int i = 0; i < ng; i++) strsz += strlen(glo[i]->name) + 1;
    int nrelsec = 0;
    for (int i = 0; i < nsec; i++) if (c->sec[i].nrel) nrelsec++;
    size_t shstrsz = 1 + 10 + 8 + 8;
    for (int i = 0; i < nsec; i++) shstrsz += strlen(c->sec[i].name) + 1 + (c->sec[i].nrel ? strlen(c->sec[i].name) + 6 : 0);
    int nsh = 1 + nsec + 3 + nrelsec;
    /* раскладка */
    uint64_t cur = 64 + 64 * (uint64_t)nsh;
    uint64_t soff[MAXSEC + 8], ssz[MAXSEC + 8];
    int k = 0;
    for (int i = 0; i < nsec; i++, k++) {
        cur = al16(cur); soff[k] = cur;
        ssz[k] = (uint64_t)c->sec[i].size;
        if (!c->sec[i].nobits) cur += ssz[k];
    }
    int i_shstr = k++, i_sym = k++, i_str = k++;
    cur = al16(cur); soff[i_shstr] = cur; ssz[i_shstr] = shstrsz; cur += shstrsz;
    cur = al16(cur); soff[i_sym] = cur; ssz[i_sym] = 24 * (uint64_t)nsyms; cur += ssz[i_sym];
    cur = al16(cur); soff[i_str] = cur; ssz[i_str] = strsz; cur += strsz;
    int rk0 = k;
    for (int i = 0; i < nsec; i++) if (c->sec[i].nrel) {
        cur = al16(cur); soff[k] = cur; ssz[k] = 24 * (uint64_t)c->sec[i].nrel; cur += ssz[k]; k++;
    }
    uint64_t total = al16(cur);
    uint8_t *f = aalloc(c, (size_t)total);
    memcpy(f, "\x7f" "ELF\2\1\1", 7);
    w16(f + 16, 1); w16(f + 18, 0x3e); w32(f + 20, 1); w64(f + 40, 64);
    w16(f + 52, 64); w16(f + 58, 64); w16(f + 60, (uint16_t)nsh); w16(f + 62, (uint16_t)(nsec + 1));
    /* имена секций */
    uint8_t *hs = f + soff[i_shstr];
    size_t hp = 1;
    uint32_t nmoff[MAXSEC + 8];
    for (int i = 0; i < nsec; i++) { nmoff[i] = (uint32_t)hp; strcpy((char *)hs + hp, c->sec[i].name); hp += strlen(c->sec[i].name) + 1; }
    nmoff[i_shstr] = (uint32_t)hp; strcpy((char *)hs + hp, ".shstrtab"); hp += 10;
    nmoff[i_sym] = (uint32_t)hp; strcpy((char *)hs + hp, ".symtab"); hp += 8;
    nmoff[i_str] = (uint32_t)hp; strcpy((char *)hs + hp, ".strtab"); hp += 8;
    k = rk0;
    for (int i = 0; i < nsec; i++) if (c->sec[i].nrel) {
        nmoff[k++] = (uint32_t)hp;
        sprintf((char *)hs + hp, ".rela%s", c->sec[i].name);
        hp += strlen(c->sec[i].name) + 6;
    }
    /* строки и символы */
    uint8_t *st = f + soff[i_str], *sy = f + soff[i_sym];
    size_t sp = 1;
    uint32_t *noff = aalloc(c, sizeof(uint32_t) * (size_t)(c->nsy + 1));
    strcpy((char *)st + sp, c->file);
    w32(sy + 24, (uint32_t)sp); sy[24 + 4] = 0x04; w16(sy + 24 + 6, 0xfff1);
    sp += strlen(c->file) + 1;
    for (int i = 0; i < nsec; i++) { uint8_t *e = sy + 24 * (2 + i); e[4] = 0x03; w16(e + 6, (uint16_t)(i + 1)); }
    /* имена в .strtab идут в порядке создания символов, а сама таблица - сначала локальные, потом глобальные */
    Sym **all = aalloc(c, sizeof(Sym *) * (size_t)(nl + ng + 1));
    for (int i = 0; i < nl + ng; i++) all[i] = i < nl ? loc[i] : glo[i - nl];
    sort_seq(all, nl + ng);
    for (int i = 0; i < nl + ng; i++) {
        noff[all[i] - c->sy] = (uint32_t)sp;
        strcpy((char *)st + sp, all[i]->name); sp += strlen(all[i]->name) + 1;
    }
    for (int i = 0; i < nl + ng; i++) {
        Sym *y = i < nl ? loc[i] : glo[i - nl];
        uint8_t *e = sy + 24 * (size_t)(2 + nsec + i);
        w32(e, noff[y - c->sy]);
        e[4] = i < nl ? 0x00 : 0x10;
        w16(e + 6, y->kind == K_EXT ? 0 : y->kind == K_ABS ? 0xfff1 : (uint16_t)y->sec);
        if (y->kind != K_EXT) w64(e + 8, (uint64_t)y->val);
    }
    /* перемещения */
    k = rk0;
    int rsi[MAXSEC + 8];
    for (int i = 0; i < nsec; i++) if (c->sec[i].nrel) {
        uint8_t *rt = f + soff[k]; int m = 0;
        for (int j = 0; j < c->nrel; j++) {
            Rel *r = &c->rel[j];
            if (r->sec != i + 1) continue;
            uint32_t symi = r->symk ? (uint32_t)eidx[r->ref] : (uint32_t)(1 + r->ref);
            w64(rt + 24 * (size_t)m, r->off);
            w64(rt + 24 * (size_t)m + 8, ((uint64_t)symi << 32) | (uint32_t)r->type);
            w64(rt + 24 * (size_t)m + 16, (uint64_t)r->addend);
            m++;
        }
        rsi[k] = i + 1;
        k++;
    }
    /* содержимое секций и заголовки */
    for (int i = 0; i < nsec; i++) if (!c->sec[i].nobits && c->sec[i].size) memcpy(f + soff[i], c->sec[i].data, (size_t)c->sec[i].size);
    for (int h = 1; h < nsh; h++) {
        uint8_t *e = f + 64 + 64 * (size_t)h;
        int idx = h - 1;
        w32(e, nmoff[idx]);
        w64(e + 24, soff[idx]); w64(e + 32, ssz[idx]);
        if (idx < nsec) {
            Sec *s = &c->sec[idx];
            w32(e + 4, s->type); w64(e + 8, s->flags); w64(e + 48, s->align);
        } else if (idx == i_shstr || idx == i_str) { w32(e + 4, 3); w64(e + 48, 1); }
        else if (idx == i_sym) { w32(e + 4, 2); w32(e + 40, (uint32_t)(i_str + 1)); w32(e + 44, (uint32_t)first_glob); w64(e + 48, 8); w64(e + 56, 24); }
        else { w32(e + 4, 4); w32(e + 40, (uint32_t)(i_sym + 1)); w32(e + 44, (uint32_t)rsi[idx]); w64(e + 48, 8); w64(e + 56, 24); }
    }
    (void)nsyms;
    *out = f; *outlen = (size_t)total;
}

int kas_assemble(KasArena *a, const char *file, const char *src, size_t len,
                 const uint8_t **out, size_t *outlen, char *err, size_t errcap) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.a = a; c.err = err; c.errcap = errcap; c.file = file;
    if (errcap) err[0] = 0;
    if (setjmp(c.jb)) return 1;
    c.pool = aalloc(&c, POOLSZ);
    c.st = aalloc(&c, sizeof(Stmt) * MAXSTMT);
    c.jnear = aalloc(&c, MAXSTMT);
    c.sy = aalloc(&c, sizeof(Sym) * MAXSYM);
    c.rel = aalloc(&c, sizeof(Rel) * MAXREL);
    /* препроцессор: строки исходника -> список операторов */
    c.line = 0;
    for (size_t i = 0; i < len;) {
        char buf[MAXLINE];
        size_t j = i;
        while (j < len && src[j] != '\n') j++;
        c.line++;
        if (j - i >= sizeof buf) fail(&c, "строка длиннее %d символов", MAXLINE);
        memcpy(buf, src + i, j - i);
        buf[j - i] = 0;
        if (strlen(buf) != j - i) fail(&c, "нулевой байт в строке");
        pp_line(&c, buf, 0);
        i = j + 1;
    }
    if (c.inmac) { c.line = c.macline; fail(&c, "%%macro без %%endmacro"); }
    if (c.ifdepth) fail(&c, "%%if без %%endif");
    /* проходы: размеры переходов только растут, метки сходятся к неподвижной точке */
    for (int pass = 0;; pass++) {
        if (pass >= MAXPASS) { c.line = 0; fail(&c, "размеры переходов не сошлись за %d проходов", MAXPASS); }
        c.pass = pass; c.changed = 0;
        run_pass(&c);
        if (pass >= 1 && !c.changed) break;
    }
    for (int i = 0; i < c.nsec; i++) {
        Sec *s = &c.sec[i];
        s->cap = (size_t)s->size;
        if (!s->nobits) s->data = aalloc(&c, s->cap + 1);
    }
    c.final = 1; c.pass++; c.changed = 0;
    run_pass(&c);
    if (c.changed) { c.line = 0; fail(&c, "внутренняя ошибка: последний проход изменил раскладку"); }
    c.line = 0;
    elf_write(&c, out, outlen);
    return 0;
}
