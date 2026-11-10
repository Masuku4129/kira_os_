/* kcc: препроцессор. Множества скрытия (Prosser), #include через виртуальную ФС (KCfg.read), #if с целыми intmax. */
#include "kcc.h"
#include <string.h>

struct HS { HS *next; const char *name; int len; };
typedef struct MArg MArg;
struct MArg { MArg *next; const char *name; int nlen; Token *tok; Token *exp; int has_exp; };
typedef struct Macro Macro;
struct Macro {
    Macro *next; const char *name; int nlen;
    int is_func, variadic, builtin, nparams;
    const char **pn; int *pl;
    Token *body;
};
typedef struct Cond Cond;
struct Cond { Cond *next; int active, taken, parent, seen_else, line; };
typedef struct Once Once;
struct Once { Once *next; const char *path; };

#define NB 512
struct PPState {
    Macro *macro_table[NB];
    Once *once_list;
    int unity_mode;                            /* #pragma kcc unity: каждый подключённый файл - один раз */
    Token out_head, *out_tail;
    int incl_level;
};
#define PP (&K)->pp_state
#define mtab (PP->macro_table)
#define once_list (PP->once_list)
#define unity_mode (PP->unity_mode)
#define out_head (PP->out_head)
#define out_tail (PP->out_tail)
#define incl_level (PP->incl_level)

static unsigned hash_str(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static int is_p(const Token *t, const char *s) {
    return t->k == TK_PUNCT && t->len == (int)strlen(s) && !memcmp(t->s, s, (size_t)t->len);
}
static int is_id(const Token *t, const char *s) {
    return t->k == TK_ID && t->len == (int)strlen(s) && !memcmp(t->s, s, (size_t)t->len);
}

static Macro *find_macro(const Token *t) {
    for (Macro *m = mtab[hash_str(t->s, t->len) % NB]; m; m = m->next)
        if (m->nlen == t->len && !memcmp(m->name, t->s, (size_t)t->len)) return m;
    return NULL;
}
static void undef_macro(const Token *t) {
    Macro **pp = &mtab[hash_str(t->s, t->len) % NB];
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->nlen == t->len && !memcmp((*pp)->name, t->s, (size_t)t->len)) { *pp = (*pp)->next; return; }
}

/* ---------- множества скрытия ---------- */
static int hs_has(const HS *h, const Token *t) {
    for (; h; h = h->next) if (h->len == t->len && !memcmp(h->name, t->s, (size_t)t->len)) return 1;
    return 0;
}
static HS *hs_add(HS *h, const char *n, int l) {
    HS *x = ka_new(sizeof *x);
    x->name = n; x->len = l; x->next = h;
    return x;
}
static HS *hs_union(const HS *a, HS *b) {
    HS head = {0}, *c = &head;
    for (; a; a = a->next) { c->next = ka_new(sizeof(HS)); c = c->next; c->name = a->name; c->len = a->len; }
    c->next = b;
    return head.next;
}
static HS *hs_inter(const HS *a, const HS *b) {
    HS head = {0}, *c = &head;
    for (; a; a = a->next)
        for (const HS *x = b; x; x = x->next)
            if (x->len == a->len && !memcmp(x->name, a->name, (size_t)a->len)) {
                c->next = ka_new(sizeof(HS)); c = c->next; c->name = a->name; c->len = a->len;
                break;
            }
    return head.next;
}

/* ---------- списки токенов ---------- */
static Token *tcopy(const Token *t) {
    if (--K.tokbudget < 0) kerr_tok(t, "macro expansion is too large");
    Token *n = ka_new(sizeof *n);
    *n = *t; n->next = NULL;
    return n;
}
static Token *eof_like(const Token *t) {
    Token *e = ka_new(sizeof *e);
    e->k = TK_EOF; e->file = t->file; e->line = t->line; e->s = ""; e->bol = 1;
    return e;
}
static Token *copy_list(const Token *t) {      /* вход EOF-терминирован; выход тоже */
    Token head = {0}, *c = &head;
    for (; t->k != TK_EOF; t = t->next) { c->next = tcopy(t); c = c->next; }
    c->next = eof_like(t);
    return head.next;
}
static Token *skip_line(Token *t) {
    while (!t->bol && t->k != TK_EOF) t = t->next;
    return t;
}
static Token *copy_line(Token **rest, Token *t) {
    Token head = {0}, *c = &head;
    for (; !t->bol && t->k != TK_EOF; t = t->next) { c->next = tcopy(t); c = c->next; }
    c->next = eof_like(t);
    *rest = t;
    return head.next;
}

Token *pp_lex_one(Token *tmpl, const char *text) {
    Token *t = lex_buf(tmpl->file, text, strlen(text));
    if (t->k == TK_EOF || t->next->k != TK_EOF) kerr_tok(tmpl, "pasting does not give a valid preprocessing token: '%s'", text);
    t->line = tmpl->line; t->bol = 0; t->sp = tmpl->sp; t->next = NULL;
    return t;
}

/* ---------- расширение макросов ---------- */
static Token *expand_arg_list(Token *t);
static int expand_macro(Token **rest, Token *tok);

static MArg *find_param(const Macro *m, MArg *args, const Token *t) {
    if (t->k != TK_ID) return NULL;
    MArg *a = args;
    for (int i = 0; i < m->nparams && a; i++, a = a->next)
        if (m->pl[i] == t->len && !memcmp(m->pn[i], t->s, (size_t)t->len)) return a;
    return NULL;
}

static Token *stringify(const Token *arg, const Token *tmpl) {
    SB sb = {0};
    sb_add(&sb, "\"", 1);
    for (const Token *t = arg; t->k != TK_EOF; t = t->next) {
        if (t != arg && t->sp) sb_add(&sb, " ", 1);
        int esc = t->k == TK_STR || t->k == TK_CHR;
        for (int i = 0; i < t->len; i++) {
            if (esc && (t->s[i] == '"' || t->s[i] == '\\')) sb_add(&sb, "\\", 1);
            sb_add(&sb, t->s + i, 1);
        }
    }
    sb_add(&sb, "\"", 1);
    return pp_lex_one((Token *)tmpl, sb.p);
}

static Token *placemarker(const Token *t) {
    Token *p = tcopy(t);
    p->k = TK_PUNCT; p->s = ""; p->len = 0; p->mark = 1; p->hs = NULL;
    return p;
}

static Token *paste(Token *l, Token *r) {
    if (l->mark) return r;
    if (r->mark) return l;
    char *buf = ka_new((size_t)(l->len + r->len + 1));
    memcpy(buf, l->s, (size_t)l->len);
    memcpy(buf + l->len, r->s, (size_t)r->len);
    Token *n = pp_lex_one(l, buf);
    n->hs = l->hs; n->sp = l->sp; n->bol = 0;
    return n;
}


typedef struct { Token head, *cur; } TL;
static void tl_init(TL *l) { l->head.next = NULL; l->cur = &l->head; }
static void tl_push(TL *l, Token *t) { t->next = NULL; l->cur->next = t; l->cur = t; }
static void tl_cat(TL *l, const Token *src) {
    for (; src->k != TK_EOF; src = src->next) tl_push(l, tcopy(src));
}
static void tl_replace_last(TL *l, Token *n) {
    if (n == l->cur) return;
    Token *p = &l->head;
    while (p->next != l->cur) p = p->next;
    p->next = n; n->next = NULL; l->cur = n;
}

static Token *expanded(MArg *a) {
    if (!a->has_exp) {
        if (++K.depth > 400) kerr_tok(a->tok, "macro expansion is nested too deeply");
        a->exp = expand_arg_list(copy_list(a->tok));
        a->has_exp = 1;
        K.depth--;
    }
    return a->exp;
}

/* Подстановка тела макроса; результат заканчивается на rest. */
static Token *subst(const Macro *m, MArg *args, Token *rest) {
    TL L; tl_init(&L);
    for (Token *b = m->body; b->k != TK_EOF;) {
        if (m->is_func && is_p(b, "#")) {
            MArg *a = find_param(m, args, b->next);
            Token *s = stringify(a->tok, b);
            s->sp = b->sp;
            tl_push(&L, s);
            b = b->next->next;
            continue;
        }
        if (is_p(b, "##")) {
            if (L.cur == &L.head) kerr_tok(b, "'##' cannot appear at either end of a macro expansion");
            Token *r = b->next;
            MArg *a = m->is_func ? find_param(m, args, r) : NULL;
            TL R; tl_init(&R);
            if (a) { if (a->tok->k == TK_EOF) tl_push(&R, placemarker(b)); else tl_cat(&R, a->tok); }
            else tl_push(&R, tcopy(r));
            Token *first = R.head.next, *tailrest = first->next;
            first->next = NULL;
            tl_replace_last(&L, paste(L.cur, first));
            if (tailrest) { L.cur->next = tailrest; while (L.cur->next) L.cur = L.cur->next; }
            b = r->next;
            continue;
        }
        if (m->variadic && is_p(b, ",") && is_p(b->next, "##") && b->next->next->k == TK_ID) {
            MArg *a = find_param(m, args, b->next->next);
            MArg *last = args;
            while (last && last->next) last = last->next;
            if (a && a == last) {   /* GNU: ", ## __VA_ARGS__" */
                if (a->tok->k != TK_EOF) { tl_push(&L, tcopy(b)); tl_cat(&L, a->tok); }
                b = b->next->next->next;
                continue;
            }
        }
        MArg *a = m->is_func ? find_param(m, args, b) : NULL;
        if (a) {
            Token *before = L.cur;
            if (is_p(b->next, "##")) { if (a->tok->k == TK_EOF) tl_push(&L, placemarker(b)); else tl_cat(&L, a->tok); }
            else tl_cat(&L, expanded(a));
            if (L.cur != before) before->next->sp = b->sp;
            b = b->next;
            continue;
        }
        tl_push(&L, tcopy(b));
        b = b->next;
    }
    Token *p = &L.head;
    while (p->next) {
        if (p->next->mark) p->next = p->next->next; else p = p->next;
    }
    p->next = rest;
    return L.head.next;
}

static Token *builtin_tok(const Macro *m, Token *tok) {
    const char *txt;
    if (m->builtin == 1) txt = kfmt("%d", tok->line);
    else if (m->builtin == 3) txt = kfmt("%d", K.counter++);
    else if (m->builtin == 4) txt = kfmt("%d", incl_level);
    else {
        SB sb = {0};
        sb_add(&sb, "\"", 1);
        for (const char *f = tok->file; *f; f++) { if (*f == '"' || *f == '\\') sb_add(&sb, "\\", 1); sb_add(&sb, f, 1); }
        sb_add(&sb, "\"", 1);
        txt = sb.p;
    }
    Token *t = pp_lex_one(tok, txt);
    t->sp = tok->sp;
    return t;
}

static void add_hs(Token *r, const Token *stop, HS *hs) {
    for (Token *t = r; t != stop; t = t->next) { t->hs = hs_union(t->hs, hs_union(hs, NULL)); t->bol = 0; }
}

static int expand_macro(Token **rest, Token *tok) {
    if (tok->k != TK_ID || hs_has(tok->hs, tok)) return 0;
    Macro *m = find_macro(tok);
    if (!m) return 0;
    if (m->builtin) {
        Token *t = builtin_tok(m, tok);
        t->next = tok->next;
        *rest = t;
        return 1;
    }
    if (!m->is_func) {
        HS *hs = hs_add(hs_union(tok->hs, NULL), m->name, m->nlen);
        Token *r = subst(m, NULL, tok->next);
        add_hs(r, tok->next, hs);
        if (r != tok->next) r->sp = tok->sp;
        *rest = r;
        return 1;
    }
    if (!is_p(tok->next, "(")) return 0;
    MArg *ahead = NULL, **ap = &ahead;
    int depth = 0, npieces = 0;
    TL L; tl_init(&L);
    Token *t = tok->next->next;
    for (;; t = t->next) {
        if (t->k == TK_EOF) kerr_tok(tok, "unterminated argument list invoking macro '%.*s'", m->nlen, m->name);
        int fin = 0;
        if (depth == 0 && is_p(t, ")")) fin = 2;
        else if (depth == 0 && is_p(t, ",") && !(m->variadic && npieces == m->nparams - 1)) fin = 1;
        if (fin) {
            MArg *a = ka_new(sizeof *a);
            L.cur->next = eof_like(t);
            a->tok = L.head.next;
            *ap = a; ap = &a->next; npieces++;
            tl_init(&L);
            if (fin == 2) break;
            continue;
        }
        if (is_p(t, "(")) depth++;
        else if (is_p(t, ")")) depth--;
        Token *c = tcopy(t);
        c->bol = 0;
        tl_push(&L, c);
    }
    Token *rp = t;
    if (m->nparams == 0) {
        if (npieces != 1 || ahead->tok->k != TK_EOF) kerr_tok(tok, "too many arguments provided to macro '%.*s'", m->nlen, m->name);
    } else {
        if (npieces > m->nparams) kerr_tok(tok, "too many arguments provided to macro '%.*s'", m->nlen, m->name);
        if (npieces < m->nparams) {
            if (m->variadic && npieces == m->nparams - 1) {
                MArg *a = ka_new(sizeof *a);
                a->tok = eof_like(rp);
                *ap = a;
            } else kerr_tok(tok, "too few arguments provided to macro '%.*s'", m->nlen, m->name);
        }
    }
    HS *hs = hs_add(hs_inter(tok->hs, rp->hs), m->name, m->nlen);
    Token *r = subst(m, ahead, rp->next);
    add_hs(r, rp->next, hs);
    if (r != rp->next) r->sp = tok->sp;
    *rest = r;
    return 1;
}

static Token *expand_arg_list(Token *t) {
    TL L; tl_init(&L);
    while (t->k != TK_EOF) {
        if (expand_macro(&t, t)) continue;
        Token *nx = t->next;
        tl_push(&L, t);
        t = nx;
    }
    L.cur->next = eof_like(t);
    return L.head.next;
}

/* ---------- #if ---------- */
typedef struct { int64_t v; int u; } V;
static Token *ev_t;

static int binprec(const Token *t) {
    if (t->k != TK_PUNCT) return 0;
    static const struct { const char *s; int p; } tab[] = {
        { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "==", 6 }, { "!=", 6 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "<<", 8 }, { ">>", 8 },
        { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 }, { 0, 0 } };
    for (int i = 0; tab[i].s; i++) if (is_p(t, tab[i].s)) return tab[i].p;
    return 0;
}

static V ev_cond(int skip);

static V ev_unary(int skip) {
    Token *t = ev_t;
    V r = {0, 0};
    if (t->k == TK_EOF) kerr_tok(t, "expected value in preprocessor expression");
    ev_t = t->next;
    if (t->k == TK_NUM) { Type *ty; parse_intlit(t, &r.v, &ty); r.u = ty->is_unsigned; return r; }
    if (t->k == TK_CHR) {
        conv_char(t); r.v = t->ival; return r;
    }
    if (t->k == TK_ID) {
        if (t->len > 6 && !memcmp(t->s, "__has_", 6)) kerr_tok(t, "'%.*s' is not supported", t->len, t->s);
        return r;
    }
    if (is_p(t, "(")) {
        r = ev_cond(skip);
        if (!is_p(ev_t, ")")) kerr_tok(ev_t, "expected ')' in preprocessor expression");
        ev_t = ev_t->next;
        return r;
    }
    if (is_p(t, "+")) return ev_unary(skip);
    if (is_p(t, "-")) { r = ev_unary(skip); r.v = (int64_t)(0 - (uint64_t)r.v); return r; }
    if (is_p(t, "~")) { r = ev_unary(skip); r.v = ~r.v; return r; }
    if (is_p(t, "!")) { r = ev_unary(skip); r.v = !r.v; r.u = 0; return r; }
    kerr_tok(t, "invalid token '%.*s' in preprocessor expression", t->len, t->s);
}

static V ev_bin(int minp, int skip) {
    V l = ev_unary(skip);
    for (;;) {
        int p = binprec(ev_t);
        if (!p || p < minp) break;
        Token *op = ev_t;
        ev_t = ev_t->next;
        int is_and = is_p(op, "&&"), is_or = is_p(op, "||");
        V r = ev_bin(p + 1, skip || (is_and && !l.v) || (is_or && l.v));
        int u = l.u || r.u;
        uint64_t a = (uint64_t)l.v, b = (uint64_t)r.v;
        V o = {0, u};
        if (is_and) { o.v = l.v && r.v; o.u = 0; }
        else if (is_or) { o.v = l.v || r.v; o.u = 0; }
        else if (is_p(op, "|")) o.v = (int64_t)(a | b);
        else if (is_p(op, "^")) o.v = (int64_t)(a ^ b);
        else if (is_p(op, "&")) o.v = (int64_t)(a & b);
        else if (is_p(op, "==")) { o.v = l.v == r.v; o.u = 0; }
        else if (is_p(op, "!=")) { o.v = l.v != r.v; o.u = 0; }
        else if (is_p(op, "<")) { o.v = u ? a < b : l.v < r.v; o.u = 0; }
        else if (is_p(op, ">")) { o.v = u ? a > b : l.v > r.v; o.u = 0; }
        else if (is_p(op, "<=")) { o.v = u ? a <= b : l.v <= r.v; o.u = 0; }
        else if (is_p(op, ">=")) { o.v = u ? a >= b : l.v >= r.v; o.u = 0; }
        else if (is_p(op, "<<") || is_p(op, ">>")) {
            o.u = l.u;
            if (!skip && (r.v < 0 || r.v >= 64)) kerr_tok(op, "shift count is out of range in preprocessor expression");
            int c = (r.v < 0 || r.v >= 64) ? 0 : (int)r.v;
            if (is_p(op, "<<")) o.v = (int64_t)(a << c);
            else o.v = l.u ? (int64_t)(a >> c) : (l.v >> c);
        }
        else if (is_p(op, "+")) o.v = (int64_t)(a + b);
        else if (is_p(op, "-")) o.v = (int64_t)(a - b);
        else if (is_p(op, "*")) o.v = (int64_t)(a * b);
        else {
            if (r.v == 0) {
                if (!skip) kerr_tok(op, "division by zero in preprocessor expression");
                o.v = 0;
            } else if (is_p(op, "/")) o.v = u ? (int64_t)(a / b) : (r.v == -1 ? (int64_t)(0 - a) : l.v / r.v);
            else o.v = u ? (int64_t)(a % b) : (r.v == -1 ? 0 : l.v % r.v);
        }
        l = o;
    }
    return l;
}

static V ev_cond(int skip) {
    V c = ev_bin(1, skip);
    if (!is_p(ev_t, "?")) return c;
    ev_t = ev_t->next;
    V a = ev_cond(skip || !c.v);
    if (!is_p(ev_t, ":")) kerr_tok(ev_t, "expected ':' in preprocessor expression");
    ev_t = ev_t->next;
    V b = ev_cond(skip || c.v);
    V r;
    if (c.v) r = a; else r = b;   /* kcc: условный оператор над структурами не поддерживается */
    r.u = a.u || b.u;
    return r;
}

static const char *path_dir(const char *p);
static const char *eval_path = "";
/* __has_include("x") / __has_include(<x>): ищет файл теми же путями, что и #include, не читая его содержимое в препроцессор. */
static int has_include(Token *name_tok, int angle, const char *nm) {
    const KCfg *c = K.cfg;
    size_t dl = 0;
    (void)name_tok;
    if (!angle || nm[0] == '/') {
        const char *p = nm[0] == '/' ? nm : kfmt("%s%s", path_dir(eval_path), nm);
        if (c->read(c->ud, p, &dl)) return 1;
    }
    for (int i = 0; i < c->nincdirs; i++) {
        const char *d = c->incdirs[i];
        const char *p = d[0] && d[strlen(d) - 1] != '/' ? kfmt("%s/%s", d, nm) : kfmt("%s%s", d, nm);
        if (c->read(c->ud, p, &dl)) return 1;
    }
    return 0;
}

static int eval_if(Token *line, const Token *where) {
    TL L; tl_init(&L);
    for (Token *t = line; t->k != TK_EOF;) {
        if (is_id(t, "defined")) {
            Token *n = t->next;
            int par = is_p(n, "(");
            if (par) n = n->next;
            if (n->k != TK_ID) kerr_tok(t, "macro name missing after 'defined'");
            Token *id = n;
            n = n->next;
            if (par) { if (!is_p(n, ")")) kerr_tok(t, "missing ')' after 'defined'"); n = n->next; }
            tl_push(&L, pp_lex_one(t, find_macro(id) ? "1" : "0"));
            t = n;
            continue;
        }
        if (is_id(t, "__has_include")) {
            Token *n = t->next;
            if (!is_p(n, "(")) kerr_tok(t, "missing '(' after '__has_include'");
            n = n->next;
            int angle = 0;
            const char *nm;
            if (n->k == TK_STR) { nm = ka_strdup(n->s + 1, (size_t)n->len - 2); n = n->next; }
            else if (is_p(n, "<")) {
                SB sb = {0};
                n = n->next;
                for (; n->k != TK_EOF && !is_p(n, ">"); n = n->next) { if (n->sp && sb.len) sb_add(&sb, " ", 1); sb_add(&sb, n->s, (size_t)n->len); }
                if (n->k == TK_EOF) kerr_tok(t, "expected '>' in __has_include");
                n = n->next;
                angle = 1; nm = sb.p ? ka_strdup(sb.p, sb.len) : "";
            } else kerr_tok(t, "expected \"FILENAME\" or <FILENAME> in __has_include");
            if (!is_p(n, ")")) kerr_tok(t, "missing ')' after '__has_include'");
            n = n->next;
            tl_push(&L, pp_lex_one(t, has_include(t, angle, nm) ? "1" : "0"));
            t = n;
            continue;
        }
        Token *nx = t->next;
        tl_push(&L, t);
        t = nx;
    }
    L.cur->next = eof_like(where);
    Token *ex = expand_arg_list(L.head.next);
    if (ex->k == TK_EOF) kerr_tok(where, "#if with no expression");
    ev_t = ex;
    V v = ev_cond(0);
    if (ev_t->k != TK_EOF) kerr_tok(ev_t, "unexpected token '%.*s' in preprocessor expression", ev_t->len, ev_t->s);
    return v.v != 0;
}

/* ---------- директивы ---------- */
static void pp_tokens(Token *tok, const char *path, int depth);

static const char *path_dir(const char *p) {
    const char *sl = strrchr(p, '/');
    if (!sl) return "";
    return ka_strdup(p, (size_t)(sl - p + 1));
}

static void read_define(Token *tok) {
    Token *name = tok;
    if (name->k != TK_ID || name->bol) kerr_tok(name, "macro name must be an identifier");
    if (is_id(name, "defined")) kerr_tok(name, "'defined' cannot be used as a macro name");
    Macro *m = ka_new(sizeof *m);
    m->name = name->s; m->nlen = name->len;
    tok = name->next;
    if (!tok->bol && is_p(tok, "(") && !tok->sp) {
        m->is_func = 1;
        tok = tok->next;
        const char *names[64]; int lens[64]; int n = 0;
        while (!is_p(tok, ")")) {
            if (n) {
                if (!is_p(tok, ",")) kerr_tok(tok, "expected ',' or ')' in macro parameter list");
                tok = tok->next;
            }
            if (tok->bol || tok->k == TK_EOF) kerr_tok(name, "missing ')' in macro parameter list");
            if (n >= 63) kerr_tok(tok, "too many macro parameters");
            if (is_p(tok, "...")) {
                names[n] = "__VA_ARGS__"; lens[n++] = 11; m->variadic = 1;
                tok = tok->next;
                if (!is_p(tok, ")")) kerr_tok(tok, "'...' must be the last macro parameter");
                break;
            }
            if (tok->k != TK_ID) kerr_tok(tok, "invalid macro parameter");
            for (int i = 0; i < n; i++) if (lens[i] == tok->len && !memcmp(names[i], tok->s, (size_t)tok->len)) kerr_tok(tok, "duplicate macro parameter");
            names[n] = tok->s; lens[n++] = tok->len;
            tok = tok->next;
            if (is_p(tok, "...")) {
                m->variadic = 1;
                tok = tok->next;
                if (!is_p(tok, ")")) kerr_tok(tok, "'...' must be the last macro parameter");
                break;
            }
        }
        tok = tok->next;
        m->nparams = n;
        m->pn = ka_new(sizeof(char *) * (size_t)(n + 1));
        m->pl = ka_new(sizeof(int) * (size_t)(n + 1));
        for (int i = 0; i < n; i++) { m->pn[i] = names[i]; m->pl[i] = lens[i]; }
    }
    Token *rest;
    m->body = copy_line(&rest, tok);
    if (m->body->k != TK_EOF) m->body->sp = 0;
    for (Token *b = m->body; b->k != TK_EOF; b = b->next) {
        if (is_p(b, "##") && (b == m->body || b->next->k == TK_EOF)) kerr_tok(b, "'##' cannot appear at either end of a macro expansion");
        if (m->is_func && is_p(b, "#")) {
            int ok = 0;
            if (b->next->k == TK_ID)
                for (int i = 0; i < m->nparams; i++) if (m->pl[i] == b->next->len && !memcmp(m->pn[i], b->next->s, (size_t)b->next->len)) ok = 1;
            if (!ok) kerr_tok(b, "'#' is not followed by a macro parameter");
        }
    }
    undef_macro(name);
    unsigned h = hash_str(name->s, name->len) % NB;
    m->next = mtab[h]; mtab[h] = m;
}

static void define_builtin(const char *name, int id) {
    Macro *m = ka_new(sizeof *m);
    m->name = name; m->nlen = (int)strlen(name); m->builtin = id;
    unsigned h = hash_str(name, m->nlen) % NB;
    m->next = mtab[h]; mtab[h] = m;
}

static const char *const predef[] = {
    "__KIRA__ 1", "__STDC__ 1", "__STDC_VERSION__ 199901L", "__STDC_HOSTED__ 0", "__LP64__ 1", "_LP64 1",
    "__CHAR_BIT__ 8", "__SIZEOF_SHORT__ 2", "__SIZEOF_INT__ 4", "__SIZEOF_LONG__ 8", "__SIZEOF_LONG_LONG__ 8",
    "__SIZEOF_POINTER__ 8", "__SIZE_TYPE__ unsigned long", "__PTRDIFF_TYPE__ long", "__INT_MAX__ 2147483647",
    "__LONG_MAX__ 9223372036854775807L", "__LONG_LONG_MAX__ 9223372036854775807LL",
    "__ATOMIC_RELAXED 0", "__ATOMIC_CONSUME 1", "__ATOMIC_ACQUIRE 2", "__ATOMIC_RELEASE 3", "__ATOMIC_ACQ_REL 4", "__ATOMIC_SEQ_CST 5",
    "__ORDER_LITTLE_ENDIAN__ 1234", "__ORDER_BIG_ENDIAN__ 4321", "__BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__", 0 };

static int file_once(const char *path) {
    for (Once *o = once_list; o; o = o->next) if (!strcmp(o->path, path)) return 1;
    return 0;
}

static void do_include(Token *hash, Token *line, const char *cur_path, int depth) {
    int angle = 0;
    const char *name; int nl;
    if (line->k == TK_STR && line->hdr) { angle = 1; name = line->s + 1; nl = line->len - 2; }
    else if (line->k == TK_STR) { name = line->s + 1; nl = line->len - 2; }
    else {
        Token *ex = expand_arg_list(line);
        if (ex->k == TK_STR && !ex->hdr) { name = ex->s + 1; nl = ex->len - 2; }
        else if (is_p(ex, "<")) {
            SB sb = {0};
            Token *t = ex->next;
            for (; t->k != TK_EOF && !is_p(t, ">"); t = t->next) { if (t->sp && sb.len) sb_add(&sb, " ", 1); sb_add(&sb, t->s, (size_t)t->len); }
            if (t->k == TK_EOF) kerr_tok(hash, "expected '>' in #include");
            angle = 1; name = sb.p ? sb.p : ""; nl = (int)sb.len;
        } else kerr_tok(hash, "expected \"FILENAME\" or <FILENAME> after #include");
    }
    if (nl <= 0) kerr_tok(hash, "empty filename in #include");
    char *nm = ka_strdup(name, (size_t)nl);
    const KCfg *c = K.cfg;
    const char *found = NULL, *data = NULL; size_t dl = 0;
    if (!angle || nm[0] == '/') {
        const char *p = nm[0] == '/' ? nm : kfmt("%s%s", path_dir(cur_path), nm);
        data = c->read(c->ud, p, &dl);
        if (data) found = p;
    }
    for (int i = 0; !found && i < c->nincdirs; i++) {
        const char *d = c->incdirs[i];
        const char *p = d[0] && d[strlen(d) - 1] != '/' ? kfmt("%s/%s", d, nm) : kfmt("%s%s", d, nm);
        data = c->read(c->ud, p, &dl);
        if (data) found = p;
    }
    if (!found) kerr_tok(hash, "'%s' file not found", nm);
    if (file_once(found)) return;
    if (unity_mode) { Once *o = ka_new(sizeof *o); o->path = found; o->next = once_list; once_list = o; }
    if (depth >= 100) kerr_tok(hash, "#include nested too deeply");
    Token *t = lex_buf(found, data, dl);
    incl_level++;
    pp_tokens(t, found, depth + 1);
    incl_level--;
}

static void pp_tokens(Token *tok, const char *path, int depth) {
    Cond *cs = NULL;
    while (tok->k != TK_EOF) {
        int act = cs ? cs->active : 1;
        if (tok->bol && is_p(tok, "#")) {
            tok = tok->next;
            if (tok->bol || tok->k == TK_EOF) continue;           /* пустая директива */
            Token *dn = tok;
            tok = tok->next;
            if (dn->k != TK_ID) {
                if (!act) { tok = skip_line(tok); continue; }
                kerr_tok(dn, "invalid preprocessing directive");
            }
            if (is_id(dn, "if") || is_id(dn, "ifdef") || is_id(dn, "ifndef")) {
                int v = 0;
                if (act) {
                    if (is_id(dn, "if")) { Token *rest; Token *line = copy_line(&rest, tok); tok = rest; eval_path = path; v = eval_if(line, dn); }
                    else {
                        if (tok->k != TK_ID || tok->bol) kerr_tok(dn, "macro name missing in #%.*s", dn->len, dn->s);
                        v = find_macro(tok) != NULL;
                        if (is_id(dn, "ifndef")) v = !v;
                        tok = skip_line(tok->next);
                    }
                } else tok = skip_line(tok);
                Cond *c = ka_new(sizeof *c);
                c->next = cs; c->parent = act; c->active = act && v; c->taken = v; c->line = dn->line;
                cs = c;
                continue;
            }
            if (is_id(dn, "elif")) {
                if (!cs) kerr_tok(dn, "#elif without #if");
                if (cs->seen_else) kerr_tok(dn, "#elif after #else");
                if (!cs->parent || cs->taken) { cs->active = 0; tok = skip_line(tok); }
                else { Token *rest; Token *line = copy_line(&rest, tok); tok = rest; eval_path = path; int v = eval_if(line, dn); cs->active = v; cs->taken = v; }
                continue;
            }
            if (is_id(dn, "else")) {
                if (!cs) kerr_tok(dn, "#else without #if");
                if (cs->seen_else) kerr_tok(dn, "#else after #else");
                cs->seen_else = 1;
                cs->active = cs->parent && !cs->taken;
                cs->taken = 1;
                tok = skip_line(tok);
                continue;
            }
            if (is_id(dn, "endif")) {
                if (!cs) kerr_tok(dn, "#endif without #if");
                cs = cs->next;
                tok = skip_line(tok);
                continue;
            }
            if (!act) { tok = skip_line(tok); continue; }
            if (is_id(dn, "include")) {
                Token *rest; Token *line = copy_line(&rest, tok);
                tok = rest;
                if (line->k == TK_EOF) kerr_tok(dn, "#include expects \"FILENAME\" or <FILENAME>");
                do_include(dn, line, path, depth);
            } else if (is_id(dn, "define")) {
                read_define(tok);
                tok = skip_line(tok);
            } else if (is_id(dn, "undef")) {
                if (tok->k != TK_ID || tok->bol) kerr_tok(dn, "macro name missing in #undef");
                undef_macro(tok);
                tok = skip_line(tok);
            } else if (is_id(dn, "error") || is_id(dn, "warning")) {
                SB sb = {0};
                for (Token *t = tok; !t->bol && t->k != TK_EOF; t = t->next) { if (t->sp && sb.len) sb_add(&sb, " ", 1); sb_add(&sb, t->s, (size_t)t->len); }
                if (is_id(dn, "error")) kerr_tok(dn, "#error %s", sb.p ? sb.p : "");
                kwarn(dn->file, dn->line, "#warning %s", sb.p ? sb.p : "");
                tok = skip_line(tok);
            } else if (is_id(dn, "pragma")) {
                if (!tok->bol && is_id(tok, "once")) {
                    Once *o = ka_new(sizeof *o);
                    o->path = path; o->next = once_list; once_list = o;
                } else if (!tok->bol && is_id(tok, "kcc") && !tok->next->bol && is_id(tok->next, "unity")) unity_mode = 1;
                tok = skip_line(tok);
            } else if (is_id(dn, "line")) {
                if (tok->bol || tok->k != TK_NUM) kerr_tok(dn, "#line directive requires a positive integer argument");
                int64_t v; Type *ty;
                parse_intlit(tok, &v, &ty);
                if (v <= 0 || v > 2147483647) kerr_tok(tok, "#line directive requires a positive integer argument");
                Token *nx = tok->next;
                const char *nf = NULL;
                if (!nx->bol && nx->k == TK_STR) {
                    nf = ka_strdup(nx->s + 1, (size_t)nx->len - 2);
                    nx = nx->next;
                } else if (!nx->bol && nx->k != TK_EOF) kerr_tok(nx, "invalid filename for #line directive");
                Token *nl = skip_line(nx);
                int delta = (int)v - nl->line;
                for (Token *t = nl; t->k != TK_EOF; t = t->next) { t->line += delta; if (nf) t->file = nf; }
                tok = nl;
            } else kerr_tok(dn, "invalid preprocessing directive #%.*s", dn->len, dn->s);
            continue;
        }
        if (!act) { tok = tok->next; continue; }
        if (expand_macro(&tok, tok)) continue;
        Token *nx = tok->next;
        out_tail->next = tok; out_tail = tok; tok->next = NULL;
        tok = nx;
    }
    if (cs) kerr(tok->file, cs->line, "unterminated conditional directive");
}

Token *preprocess(const char *path) {
    K.pp_state = ka_new(sizeof(PPState));
    out_head.next = NULL; out_tail = &out_head;
    define_builtin("__LINE__", 1);
    define_builtin("__FILE__", 2);
    define_builtin("__COUNTER__", 3);
    define_builtin("__INCLUDE_LEVEL__", 4);
    define_builtin("__BASE_FILE__", 5);
    const KCfg *c = K.cfg;
    SB pre = {0};
    for (int i = 0; predef[i]; i++) sb_printf(&pre, "#define %s\n", predef[i]);
    sb_printf(&pre, "#define %s 1\n", c->target && !strcmp(c->target, "aarch64") ? "__aarch64__" : "__x86_64__");
    if (kcc_nosse) sb_printf(&pre, "#define __KCC_NOSSE__ 1\n");     /* сборка без SSE: float/double недоступны (libc-lite не печатает вещественные) */
    if (!c->target || !strcmp(c->target, "x86_64")) {            /* builtin-ы счёта нулей: bsf/bsr (результат при x == 0 не определён, как у GCC) */
        sb_printf(&pre, "static inline int __kcc_ctz(unsigned x) { unsigned r; __asm__(\"bsfl %%1, %%0\" : \"=a\"(r) : \"a\"(x)); return (int)r; }\n");
        sb_printf(&pre, "static inline int __kcc_ctzll(unsigned long long x) { unsigned long long r; __asm__(\"bsfq %%1, %%0\" : \"=a\"(r) : \"a\"(x)); return (int)r; }\n");
        sb_printf(&pre, "static inline int __kcc_clz(unsigned x) { unsigned r; __asm__(\"bsrl %%1, %%0\" : \"=a\"(r) : \"a\"(x)); return 31 - (int)r; }\n");
        sb_printf(&pre, "#define __builtin_ctz __kcc_ctz\n#define __builtin_ctzl __kcc_ctzll\n#define __builtin_ctzll __kcc_ctzll\n#define __builtin_clz __kcc_clz\n");
    }
    for (int i = 0; i < c->ndefs; i++) {
        const char *d = c->defs[i], *eq = strchr(d, '=');
        if (eq) sb_printf(&pre, "#define %.*s %s\n", (int)(eq - d), d, eq + 1);
        else sb_printf(&pre, "#define %s 1\n", d);
    }
    Token *pt = lex_buf("<built-in>", pre.p, pre.len);
    pp_tokens(pt, "<built-in>", 0);
    size_t len = 0;
    const char *data = c->read(c->ud, path, &len);
    if (!data) kerr(path, 0, "cannot open source file");
    Token *t = lex_buf(path, data, len);
    pp_tokens(t, path, 0);
    out_tail->next = eof_like(t);
    Token *r = out_head.next;
    convert_tokens(r);
    return r;
}
