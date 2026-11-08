/* kcc: разбор C99 (+ расширения GNU по docs/KCC.md) в типизированное AST. Все приведения вставляются при построении узлов. */
#include "kcc.h"
#include <string.h>

typedef struct VS VS;
struct VS { VS *next; const char *name; int nlen, vla; Obj *var; Type *tdef; Type *ety; int64_t eval; };   /* vla: var - указатель на блок стека, сам объект - *var */
typedef struct TS TS;
struct TS { TS *next; const char *name; int nlen; Type *ty; };
typedef struct Scope Scope;
struct Scope { Scope *parent; VS *vars; TS *tags; VS **hv; TS **ht; };
#define GH 1024

typedef struct { int is_typedef, is_static, is_extern, is_inline, noret, packed, align, weak; } Attr;

typedef struct Init Init;
struct Init { Type *ty; Init **ch; int nch; Node *expr; Member *mem; int flex; };

static Token *tok;
static Scope *scope, *gscope;
static Obj *globals_head, *globals_tail;
static Obj *cur_fn;
static Obj *fn_locals_tail;
static int nlocals;
static int str_count, static_count, type_count;
static Node *cur_switch;
static int brk_lbl, cont_lbl;
static Node *fn_gotos, *fn_labels;

/* ---------- VLA: состояние разбора ----------
   Длина каждого VLA вычисляется один раз в скрытую локальную Type.vlen; присваивания копятся в vla_head и выпускаются там, где объявление встречено.
   Объект VLA - блок стека, его адрес лежит в скрытом указателе (VS.vla); первое VLA блока сохраняет sp в скрытой локальной, конец блока,
   break/continue/goto, выходящие из блока, восстанавливают sp (ND_SPREST). */
static Node *vla_head, *vla_tail;
static int in_params;
static Obj *vla_objs;
static Obj *vla_stack[64];
static int vla_depth, blk_vla_base, blk_is_se, stmtexpr_next, brk_vd, cont_vd;

/* ---------- вспомогательное ---------- */
static int P(const char *s) { return tok->k == TK_PUNCT && tok_is(tok, s); }
static int KW(const char *s) { return tok->k == TK_KW && tok_is(tok, s); }
static int eat(const char *s) { if (P(s)) { tok = tok->next; return 1; } return 0; }
static int eat_kw(const char *s) { if (KW(s)) { tok = tok->next; return 1; } return 0; }
static const char *tokdesc(const Token *t) {
    if (t->k == TK_EOF) return "end of input";
    return kfmt("'%.*s'", t->len > 40 ? 40 : t->len, t->s);
}
static void expect(const char *s) {
    if (!eat(s)) kerr_tok(tok, "expected '%s' before %s", s, tokdesc(tok));
}
static Token *expect_id(void) {
    if (tok->k != TK_ID) kerr_tok(tok, "expected identifier before %s", tokdesc(tok));
    Token *t = tok;
    tok = tok->next;
    return t;
}
static const char *tname(const Token *t) { return ka_strdup(t->s, (size_t)t->len); }

static void unsupported(const Token *t, const char *what) {
    kerr_tok(t, "unsupported construct: %s", what);
}

/* ---------- области видимости ---------- */
static Scope *new_scope(int global) {
    Scope *s = ka_new(sizeof *s);
    s->parent = scope;
    if (global) { s->hv = ka_new(sizeof(VS *) * GH); s->ht = ka_new(sizeof(TS *) * GH); }
    scope = s;
    return s;
}
static void leave_scope(void) { scope = scope->parent; }
static unsigned hstr(const char *s, int n) {
    unsigned h = 5381;
    for (int i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i];
    return h % GH;
}
static VS *find_in(Scope *s, const char *n, int l) {
    VS *v = s->hv ? s->hv[hstr(n, l)] : s->vars;
    for (; v; v = v->next) if (v->nlen == l && !memcmp(v->name, n, (size_t)l)) return v;
    return NULL;
}
static VS *find_vs(const Token *t) {
    for (Scope *s = scope; s; s = s->parent) {
        VS *v = find_in(s, t->s, t->len);
        if (v) return v;
    }
    return NULL;
}
static VS *add_vs(const Token *name) {
    VS *v = ka_new(sizeof *v);
    v->name = name->s; v->nlen = name->len;
    if (scope->hv) { unsigned h = hstr(name->s, name->len); v->next = scope->hv[h]; scope->hv[h] = v; }
    else { v->next = scope->vars; scope->vars = v; }
    return v;
}
static TS *find_ts(Scope *s, const Token *t) {
    TS *x = s->ht ? s->ht[hstr(t->s, t->len)] : s->tags;
    for (; x; x = x->next) if (x->nlen == t->len && !memcmp(x->name, t->s, (size_t)t->len)) return x;
    return NULL;
}
static Type *find_tag(const Token *t, int cur_only) {
    for (Scope *s = scope; s; s = cur_only ? NULL : s->parent) {
        TS *x = find_ts(s, t);
        if (x) return x->ty;
    }
    return NULL;
}
static void add_tag(const Token *name, Type *ty) {
    TS *x = ka_new(sizeof *x);
    x->name = name->s; x->nlen = name->len; x->ty = ty;
    if (scope->ht) { unsigned h = hstr(name->s, name->len); x->next = scope->ht[h]; scope->ht[h] = x; }
    else { x->next = scope->tags; scope->tags = x; }
}

static Type *find_typedef(const Token *t) {
    if (t->k != TK_ID) return NULL;
    VS *v = find_vs(t);
    return v ? v->tdef : NULL;
}

/* ---------- прототипы ---------- */
static Node *expr(void);
static Node *assign(void);
static Node *cond_expr(void);
static Node *cast_expr(void);
static Node *unary(void);
static Node *stmt(void);
static Node *compound(int new_sc);
static Type *declspec(Attr *a);
static Type *declarator(Type *ty, Token **name);
static Type *typename(void);
static int is_typename(const Token *t);
static Init *initializer(Type *ty, Type **newty);
static Node *conv_assign(Node *e, Type *to, const Token *t, const char *what);
static Node *rv(Node *n);
static int64_t const_expr(void);
static Obj *new_gvar(const Token *nm, Type *ty);
static Obj *vla_make_len(Node *e, const Token *t);
static Node *vla_take(void);
static Node *vla_chain(Node *p, Node *e, const Token *t);
static Node *size_node(Type *ty, const Token *t);

#define ENTER() do { if (++K.depth > 600) kerr_tok(tok, "nesting is too deep"); } while (0)
#define TSZ(t) (ty_unqual(t)->size)
#define TAL(t) (ty_unqual(t)->align)

/* обёртки с ограничением глубины рекурсии (защита стека от враждебного ввода) */
static Node *unary_(void);
static Node *stmt_(void);
static Node *cond_expr_(void);
static Type *declarator_(Type *ty, Token **name);
static Type *type_suffix_(Type *ty);
static Type *struct_decl_(int is_union);
static Type *declspec_(Attr *a);
static void init2_(Init *in);

static Node * unary(void) {
    ENTER();
    Node * r = unary_();
    K.depth--;
    return r;
}

static Node * stmt(void) {
    ENTER();
    Node * r = stmt_();
    K.depth--;
    return r;
}

static Node * cond_expr(void) {
    ENTER();
    Node * r = cond_expr_();
    K.depth--;
    return r;
}

static Type * declarator(Type *ty, Token **name) {
    ENTER();
    Type * r = declarator_(ty, name);
    K.depth--;
    return r;
}

static Type * type_suffix(Type *ty) {
    ENTER();
    Type * r = type_suffix_(ty);
    K.depth--;
    return r;
}

static Type * struct_decl(int is_union) {
    ENTER();
    Type * r = struct_decl_(is_union);
    K.depth--;
    return r;
}

static Type * declspec(Attr *a) {
    ENTER();
    Type * r = declspec_(a);
    K.depth--;
    return r;
}

static void init2(Init *in) {
    ENTER();
    init2_(in);
    K.depth--;
}

/* ---------- атрибуты ---------- */
static void attrs(Attr *a) {
    while (KW("__attribute__")) {
        tok = tok->next;
        expect("("); expect("(");
        while (!P(")")) {
            if (tok->k != TK_ID && tok->k != TK_KW) kerr_tok(tok, "expected attribute name");
            const char *s = tok->s; int l = tok->len;
            if (l > 4 && s[0] == '_' && s[1] == '_' && s[l - 1] == '_' && s[l - 2] == '_') { s += 2; l -= 4; }
            const Token *nt = tok;
            char name[40];
            if (l >= (int)sizeof name) kerr_tok(tok, "unsupported attribute '%.*s'", nt->len, nt->s);
            memcpy(name, s, (size_t)l); name[l] = 0;
            tok = tok->next;
            if (!strcmp(name, "packed")) a->packed = 1;
            else if (!strcmp(name, "noreturn")) a->noret = 1;
            else if (!strcmp(name, "weak")) a->weak = 1;
            else if (!strcmp(name, "aligned")) {
                int64_t v = 16;
                if (eat("(")) { v = const_expr(); expect(")"); }
                if (v <= 0 || (v & (v - 1)) || v > (1 << 20)) kerr_tok(nt, "requested alignment is not a power of 2");
                a->align = (int)v;
            } else if (!strcmp(name, "unused") || !strcmp(name, "always_inline") || !strcmp(name, "noinline") || !strcmp(name, "used") ||
                       !strcmp(name, "deprecated") || !strcmp(name, "format") || !strcmp(name, "warn_unused_result") ||
                       !strcmp(name, "nonnull") || !strcmp(name, "const") || !strcmp(name, "pure") || !strcmp(name, "cold") ||
                       !strcmp(name, "hot") || !strcmp(name, "gnu_inline") || !strcmp(name, "returns_twice")) {
                if (eat("(")) {
                    int d = 1;
                    while (d) {
                        if (tok->k == TK_EOF) kerr_tok(nt, "unterminated attribute");
                        if (P("(")) d++;
                        else if (P(")")) d--;
                        tok = tok->next;
                    }
                }
            } else kerr_tok(nt, "unsupported attribute '%s'", name);
            if (!eat(",")) break;
        }
        expect(")"); expect(")");
    }
}

/* ---------- спецификаторы ---------- */
enum { C_VOID = 1, C_BOOL = 4, C_CHAR = 16, C_SHORT = 64, C_INT = 256, C_LONG = 1024, C_OTHER = 4096, C_SIGNED = 8192, C_UNSIGNED = 16384 };

static int is_typename(const Token *t) {
    if (t->k == TK_KW) {
        static const char *const k[] = { "void", "char", "short", "int", "long", "signed", "unsigned", "_Bool", "struct", "union", "enum",
            "typeof", "const", "volatile", "restrict", "typedef", "static", "extern", "inline", "register", "auto", "_Noreturn",
            "__attribute__", "__extension__", "float", "double", "_Complex", "_Thread_local", "__thread", "_Atomic", "_Alignas", "__builtin_va_list", 0 };
        for (int i = 0; k[i]; i++) if (tok_is(t, k[i])) return 1;
        return 0;
    }
    return find_typedef(t) != NULL;
}

static Type *struct_decl(int is_union);
static Type *enum_decl(void);

/* va_list: x86-64 SysV - массив из одной структуры (24 байта), AAPCS64 - структура (32 байта), передаётся по значению */
static Type *va_ty, *va_st;
static Type *va_type(void) {
    if (!va_ty) {
        int a64 = K.cfg->target && !strcmp(K.cfg->target, "aarch64");
        va_st = ty_new(TY_STRUCT, a64 ? 32 : 24, 8);
        va_st->complete = 1; va_st->tag = "__va_list_tag";
        va_ty = a64 ? va_st : ty_array(va_st, 1);
    }
    return va_ty;
}

static Type *declspec_(Attr *a) {
    Attr scratch = {0};
    if (!a) a = &scratch;
    Type *ty = NULL;
    int counter = 0, cq = 0, vq = 0;
    const Token *first = tok;
    while (is_typename(tok)) {
        if (tok->k == TK_KW) {
            if (KW("typedef") || KW("static") || KW("extern") || KW("inline") || KW("register") || KW("auto") || KW("_Noreturn") || KW("_Thread_local") || KW("__thread")) {
                if (KW("_Thread_local") || KW("__thread")) unsupported(tok, "_Thread_local/__thread (no TLS in kernel and klink, see docs/KCC.md G4)");
                if (KW("typedef")) a->is_typedef = 1;
                else if (KW("static")) a->is_static = 1;
                else if (KW("extern")) a->is_extern = 1;
                else if (KW("inline")) a->is_inline = 1;
                else if (KW("_Noreturn")) a->noret = 1;
                if (a->is_typedef + a->is_static + a->is_extern > 1) kerr_tok(tok, "cannot combine storage-class specifiers");
                tok = tok->next;
                continue;
            }
            if (KW("const")) { cq = 1; tok = tok->next; continue; }
            if (KW("volatile")) { vq = 1; tok = tok->next; continue; }
            if (KW("restrict")) { tok = tok->next; continue; }
            if (KW("__extension__")) { tok = tok->next; continue; }
            if (KW("__attribute__")) { attrs(a); continue; }
            if (KW("_Atomic")) unsupported(tok, "_Atomic");
            if (KW("_Alignas")) {
                const Token *at = tok;
                tok = tok->next;
                expect("(");
                int64_t v;
                if (is_typename(tok)) v = TAL(typename());
                else v = const_expr();
                expect(")");
                if (v < 0 || (v & (v - 1)) || v > (1 << 20)) kerr_tok(at, "requested alignment is not a power of 2");
                if (v > a->align) a->align = (int)v;
                continue;
            }
            if (KW("_Complex")) unsupported(tok, "_Complex");
            if (KW("float") || KW("double")) {
                if (counter == C_LONG && KW("double")) unsupported(tok, "long double (kcc has no 80-bit x87 type; use double)");
                if (counter || ty) kerr_tok(tok, "invalid combination of type specifiers");
                if (kcc_nosse) unsupported(tok, "float and double (-mno-sse build: no SSE registers)");
                ty = KW("float") ? &ty_float : &ty_double;
                tok = tok->next; counter += C_OTHER;
                continue;
            }
        }
        if (KW("__builtin_va_list")) {
            if (counter || ty) break;
            ty = va_type(); tok = tok->next; counter += C_OTHER;
            continue;
        }
        if (KW("struct") || KW("union") || KW("enum") || KW("typeof") || tok->k == TK_ID) {
            if (counter) break;
            if (ty) break;
            if (KW("struct") || KW("union")) { int u = KW("union"); tok = tok->next; ty = struct_decl(u); }
            else if (KW("enum")) { tok = tok->next; ty = enum_decl(); }
            else if (KW("typeof")) {
                tok = tok->next; expect("(");
                if (is_typename(tok)) ty = typename();
                else { Node *e = expr(); ty = e->ty; }
                expect(")");
            } else { ty = find_typedef(tok); tok = tok->next; }
            counter += C_OTHER;
            continue;
        }
        if (KW("void")) counter += C_VOID;
        else if (KW("_Bool")) counter += C_BOOL;
        else if (KW("char")) counter += C_CHAR;
        else if (KW("short")) counter += C_SHORT;
        else if (KW("int")) counter += C_INT;
        else if (KW("long")) counter += C_LONG;
        else if (KW("signed")) counter |= C_SIGNED;
        else if (KW("unsigned")) counter |= C_UNSIGNED;
        else break;
        switch (counter) {
        case C_VOID: ty = &ty_void; break;
        case C_BOOL: ty = &ty_bool; break;
        case C_CHAR: ty = &ty_char; break;
        case C_SIGNED + C_CHAR: ty = &ty_char; break;
        case C_UNSIGNED + C_CHAR: ty = &ty_uchar; break;
        case C_SHORT: case C_SHORT + C_INT: case C_SIGNED + C_SHORT: case C_SIGNED + C_SHORT + C_INT: ty = &ty_short; break;
        case C_UNSIGNED + C_SHORT: case C_UNSIGNED + C_SHORT + C_INT: ty = &ty_ushort; break;
        case C_INT: case C_SIGNED: case C_SIGNED + C_INT: ty = &ty_int; break;
        case C_UNSIGNED: case C_UNSIGNED + C_INT: ty = &ty_uint; break;
        case C_LONG: case C_LONG + C_INT: case C_SIGNED + C_LONG: case C_SIGNED + C_LONG + C_INT: ty = &ty_long; break;
        case C_UNSIGNED + C_LONG: case C_UNSIGNED + C_LONG + C_INT: ty = &ty_ulong; break;
        case C_LONG + C_LONG: case C_LONG + C_LONG + C_INT: case C_SIGNED + C_LONG + C_LONG: case C_SIGNED + C_LONG + C_LONG + C_INT: ty = &ty_llong; break;
        case C_UNSIGNED + C_LONG + C_LONG: case C_UNSIGNED + C_LONG + C_LONG + C_INT: ty = &ty_ullong; break;
        default: kerr_tok(tok, "invalid combination of type specifiers");
        }
        tok = tok->next;
    }
    if (!ty) {
        if (counter & (C_SIGNED | C_UNSIGNED)) ty = (counter & C_UNSIGNED) ? &ty_uint : &ty_int;
        else kerr_tok(first, "expected a type specifier before %s", tokdesc(first));
    }
    if (cq || vq) ty = ty_qual(ty, cq, vq);
    return ty;
}

/* ---------- структуры, объединения, перечисления ---------- */
static int64_t const_expr(void);
static int is_inttype_ok(const Type *t) { return ty_is_int(t); }

static void layout_struct(Type *ty, int is_union, int packed, int align_attr) {
    int64_t bits = 0; int maxal = 1; int64_t size_u = 0;
    int idx = 0;
    for (Member *m = ty->mem; m; m = m->next) {
        m->idx = idx++;
        Type *mt = m->ty;
        int al = packed ? 1 : TAL(mt);
        if (m->align > al) al = m->align;
        if (mt->k == TY_ARRAY && mt->alen < 0) {
            if (m->next) kerr_tok(m->tok, "flexible array member must be last");
            al = packed ? 1 : TAL(mt->base);
            bits = (bits + 7) / 8 * 8;
            bits = (bits + (int64_t)al * 8 - 1) / ((int64_t)al * 8) * ((int64_t)al * 8);
            m->off = (int)(bits / 8);
            if (al > maxal) maxal = al;
            continue;
        }
        if (m->bw >= 0 && m->bo == -1) {          /* битовое поле */
            int unit = mt->size * 8;
            if (m->bw == 0) {
                if (!is_union) bits = (bits + unit - 1) / unit * unit;
                continue;
            }
            if (m->bw > unit) kerr_tok(m->tok, "width of bit-field exceeds its type");
            if (is_union) { m->off = 0; m->bo = 0; if ((m->bw + 7) / 8 > size_u) size_u = (m->bw + 7) / 8; if (!packed && mt->align > maxal) maxal = mt->align; continue; }
            if (packed) {
                m->off = (int)(bits / 8); m->bo = (int)(bits % 8);
                if (m->bo + m->bw > unit) unsupported(m->tok, "packed bit-field crossing its storage unit");
            } else {
                if (bits / unit != (bits + m->bw - 1) / unit) bits = (bits + unit - 1) / unit * unit;
                m->off = (int)(bits / unit * (unit / 8)); m->bo = (int)(bits % unit);
                if (mt->align > maxal) maxal = mt->align;
            }
            bits += m->bw;
            continue;
        }
        if (!mt->complete && !(ty_unqual(mt)->complete)) kerr_tok(m->tok, "field has incomplete type '%s'", ty_name(mt));
        if (is_union) {
            m->off = 0;
            if (TSZ(mt) > size_u) size_u = TSZ(mt);
        } else {
            bits = (bits + 7) / 8 * 8;
            bits = (bits + (int64_t)al * 8 - 1) / ((int64_t)al * 8) * ((int64_t)al * 8);
            m->off = (int)(bits / 8);
            bits += (int64_t)TSZ(mt) * 8;
        }
        if (al > maxal) maxal = al;
    }
    if (align_attr > maxal) maxal = align_attr;
    int64_t sz = is_union ? size_u : (bits + 7) / 8;
    sz = (sz + maxal - 1) / maxal * maxal;
    if (sz > 0x7fffffff) kerr(NULL, 0, "struct is too large");
    ty->size = (int)sz; ty->align = maxal; ty->complete = 1; ty->nmem = idx;
}

static Type *struct_decl_(int is_union) {
    Attr at = {0};
    attrs(&at);
    Token *tag = NULL;
    if (tok->k == TK_ID) tag = expect_id();
    if (tag && !P("{")) {
        Type *t = find_tag(tag, 0);
        if (t) {
            if (t->k != (is_union ? TY_UNION : TY_STRUCT)) kerr_tok(tag, "use of '%.*s' with tag type that does not match previous declaration", tag->len, tag->s);
            return t;
        }
        t = ty_new(is_union ? TY_UNION : TY_STRUCT, 0, 1);
        t->tag = tname(tag); t->id = ++type_count;
        add_tag(tag, t);
        return t;
    }
    if (!tag) {
        if (!P("{")) kerr_tok(tok, "expected '{' or identifier after struct/union");
    }
    Type *t = tag ? find_tag(tag, 1) : NULL;
    if (t && (t->complete || t->k != (is_union ? TY_UNION : TY_STRUCT))) kerr_tok(tag, "redefinition of '%.*s'", tag->len, tag->s);
    if (!t) {
        t = ty_new(is_union ? TY_UNION : TY_STRUCT, 0, 1);
        t->id = ++type_count;
        if (tag) { t->tag = tname(tag); add_tag(tag, t); }
    }
    expect("{");
    Member head = {0}, *cur = &head;
    Type *saved_self = t;
    while (!P("}")) {
        if (tok->k == TK_EOF) kerr_tok(tok, "expected '}' at end of struct");
        if (KW("_Static_assert")) {
            tok = tok->next; expect("(");
            int64_t v = const_expr();
            const Token *at2 = tok;
            if (eat(",")) { if (tok->k != TK_STR) kerr_tok(tok, "expected string literal in _Static_assert"); tok = tok->next; }
            expect(")"); expect(";");
            if (!v) kerr_tok(at2, "static assertion failed");
            continue;
        }
        Attr ma = {0};
        Type *base = declspec(&ma);
        if (ma.is_typedef || ma.is_static || ma.is_extern) kerr_tok(tok, "storage class not allowed in struct member");
        if (P(";") && ty_is_agg(base) && !ty_unqual(base)->tag) {      /* анонимная структура/объединение */
            if (!ty_unqual(base)->complete) kerr_tok(tok, "anonymous member has incomplete type");
            Member *m = ka_new(sizeof *m);
            m->ty = base; m->bw = -1; m->bo = 0; m->tok = tok;
            cur->next = m; cur = m;
            tok = tok->next;
            continue;
        }
        int first = 1;
        while (!eat(";")) {
            if (!first) expect(",");
            first = 0;
            Token *nm = NULL;
            Type *mt = base;
            if (!P(":")) mt = declarator(base, &nm);
            if (mt->vm) kerr_tok(nm ? nm : tok, "fields must have a constant size: variable length array in a struct or union");
            attrs(&ma);
            Member *m = ka_new(sizeof *m);
            m->ty = mt; m->name = nm ? tname(nm) : NULL; m->tok = nm ? nm : tok; m->bw = -1; m->bo = 0; m->align = ma.align;
            if (eat(":")) {
                int64_t w = const_expr();
                if (!is_inttype_ok(mt) || mt->k == TY_ENUM) {
                    if (!is_inttype_ok(mt)) kerr_tok(m->tok, "bit-field has non-integral type '%s'", ty_name(mt));
                }
                if (w < 0 || (w == 0 && nm)) kerr_tok(m->tok, "invalid bit-field width");
                m->bw = (int)w; m->bo = -1;
            } else {
                if (!nm) kerr_tok(tok, "expected member name or ';' after declaration specifiers");
                if (mt->k == TY_FUNC) kerr_tok(m->tok, "field '%s' declared as a function", m->name);
                if (mt->k == TY_VOID) kerr_tok(m->tok, "field has incomplete type 'void'");
            }
            if (m->name)
                for (Member *o = head.next; o; o = o->next)
                    if (o->name && !strcmp(o->name, m->name)) kerr_tok(m->tok, "duplicate member '%s'", m->name);
            cur->next = m; cur = m;
        }
    }
    expect("}");
    attrs(&at);
    saved_self->mem = head.next;
    layout_struct(saved_self, is_union, at.packed, at.align);
    saved_self->packed = (unsigned)at.packed;
    return saved_self;
}

static Type *enum_decl(void) {
    Attr at = {0};
    attrs(&at);
    Token *tag = NULL;
    if (tok->k == TK_ID) tag = expect_id();
    if (tag && !P("{")) {
        Type *t = find_tag(tag, 0);
        if (!t) kerr_tok(tag, "incomplete enum type 'enum %.*s'", tag->len, tag->s);
        if (t->k != TY_ENUM) kerr_tok(tag, "use of '%.*s' with tag type that does not match previous declaration", tag->len, tag->s);
        return t;
    }
    Type *t = ty_new(TY_ENUM, 4, 4);
    t->complete = 1; t->id = ++type_count;
    if (tag) {
        if (find_tag(tag, 1)) kerr_tok(tag, "redefinition of 'enum %.*s'", tag->len, tag->s);
        t->tag = tname(tag); add_tag(tag, t);
    }
    expect("{");
    int64_t v = 0; int neg = 0; int first = 1;
    while (!P("}")) {
        if (!first) { expect(","); if (P("}")) break; }
        first = 0;
        Token *nm = expect_id();
        if (eat("=")) v = const_expr();
        if (v > 2147483647LL || v < -2147483648LL) kerr_tok(nm, "enumerator value is not representable in int");
        if (v < 0) neg = 1;
        if (find_in(scope, nm->s, nm->len)) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
        VS *vs = add_vs(nm);
        vs->ety = t; vs->eval = v;
        v++;
    }
    expect("}");
    t->is_unsigned = !neg;
    return t;
}

/* ---------- описатели ---------- */
static Type *type_suffix(Type *ty);

static Param *func_params_(int *variadic, int *noproto) {
    Param head = {0}, *cur = &head;
    int has_br = 0, dep = 0;                                /* объекты параметров нужны только если в списке есть '[' (возможный VLA) */
    for (Token *q = tok; q->k != TK_EOF; q = q->next) {
        if (q->k != TK_PUNCT) continue;
        if (tok_is(q, "(")) dep++;
        else if (tok_is(q, ")")) { if (dep-- == 0) break; }
        else if (tok_is(q, "[") && q->next->k != TK_NUM && !tok_is(q->next, "]")) has_br = 1;
    }
    *variadic = 0; *noproto = 0;
    if (P(")")) { *noproto = 1; return NULL; }
    if (KW("void") && tok->next->k == TK_PUNCT && tok_is(tok->next, ")")) { tok = tok->next; return NULL; }
    for (;;) {
        if (eat("...")) {
            if (!head.next) kerr_tok(tok, "ISO C requires a named parameter before '...'");
            *variadic = 1;
            break;
        }
        Attr a = {0};
        Type *base = declspec(&a);
        if (a.is_typedef || a.is_static || a.is_extern) kerr_tok(tok, "storage class not allowed on parameter");
        Token *nm = NULL;
        Type *ty = declarator(base, &nm);
        attrs(&a);
        if (ty->k == TY_VOID) kerr_tok(tok, "parameter cannot have type 'void'");
        if (ty->k == TY_ARRAY) ty = ty_ptr(ty->base);
        else if (ty->k == TY_FUNC) ty = ty_ptr(ty);
        Param *p = ka_new(sizeof *p);
        p->ty = ty; p->name = nm ? tname(nm) : NULL;
        if (nm) for (Param *o = head.next; o; o = o->next) if (o->name && !strcmp(o->name, p->name)) kerr_tok(nm, "redefinition of parameter '%s'", p->name);
        if (nm && has_br) {                                 /* объект параметра существует уже сейчас: на него ссылаются размеры VLA следующих параметров */
            Obj *po = ka_new(sizeof *po);
            po->name = p->name; po->ty = ty; po->is_local = 1; po->slot = -1; po->align = ty->align; po->line = nm->line;
            po->is_param = 1; po->next = vla_objs; vla_objs = po;   /* в список vobjs: func_def найдёт по имени */
            VS *v = add_vs(nm);
            v->var = po;
        }
        cur->next = p; cur = p;
        if (!eat(",")) break;
    }
    return head.next;
}

static Param *func_params(int *variadic, int *noproto, Node **vinit, Obj **vobjs) {
    Node *sh = vla_head, *st = vla_tail; Obj *so = vla_objs;
    vla_head = vla_tail = NULL; vla_objs = NULL;
    in_params++;
    new_scope(0);
    Param *r = func_params_(variadic, noproto);
    leave_scope();
    in_params--;
    *vinit = vla_head; *vobjs = vla_objs;
    vla_head = sh; vla_tail = st; vla_objs = so;
    return r;
}

static Type *type_suffix_(Type *ty) {
    if (eat("(")) {
        int var, nop;
        Node *vi; Obj *vo;
        Param *ps = func_params(&var, &nop, &vi, &vo);
        expect(")");
        if (ty->k == TY_FUNC) kerr_tok(tok, "function cannot return function type");
        if (ty->k == TY_ARRAY) kerr_tok(tok, "function cannot return array type");
        Type *ft = ty_func(ty, ps, var, nop);
        ft->vinit = vi; ft->vobjs = vo;
        return ft;
    }
    if (eat("[")) {
        while (KW("static") || KW("const") || KW("volatile") || KW("restrict")) tok = tok->next;
        int64_t len = -1;
        Obj *vlen = NULL;
        if (!P("]")) {
            const Token *et = tok;
            Node *e = NULL;
            if (P("*") && tok->next->k == TK_PUNCT && tok_is(tok->next, "]")) {      /* [*] только в прототипе: размер не нужен, массив станет указателем */
                if (!in_params) kerr_tok(tok, "star modifier used outside of function prototype");
                tok = tok->next;
            } else {
                e = cond_expr();
                if (!ty_is_int(e->ty)) kerr_tok(et, "size of array has non-integer type '%s'", ty_name(e->ty));
            }
            if (!e) len = -1;
            else if (e->k == ND_NUM) {
                len = e->val;
                if (len < 0) kerr_tok(tok, "array size is negative");
            } else {
                vlen = vla_make_len(e, et);                     /* массив переменной длины: длина вычисляется здесь один раз */
                len = -2;
            }
        }
        expect("]");
        Type *inner = type_suffix(ty);
        if (inner->k == TY_FUNC) kerr_tok(tok, "array of functions is invalid");
        if (inner->k == TY_VOID) kerr_tok(tok, "array of void is invalid");
        if (!ty_unqual(inner)->complete) kerr_tok(tok, "array has incomplete element type '%s'", ty_name(inner));
        if (inner->k == TY_ARRAY && inner->alen == -1) kerr_tok(tok, "array has incomplete element type '%s'", ty_name(inner));
        Type *arr = ty_array(inner, len == -2 ? -1 : len);
        if (len == -2) { arr->alen = -2; arr->complete = 1; arr->vlen = vlen; }
        if (len == -2 || inner->vm) arr->vm = 1;
        return arr;
    }
    return ty;
}

static Type *pointers(Type *ty) {
    while (eat("*")) {
        ty = ty_ptr(ty);
        for (;;) {
            if (eat_kw("const")) ty = ty_qual(ty, 1, 0);
            else if (eat_kw("volatile")) ty = ty_qual(ty, 0, 1);
            else if (eat_kw("restrict")) continue;
            else if (KW("__attribute__")) { Attr d = {0}; attrs(&d); }
            else break;
        }
    }
    return ty;
}

static Type *declarator_(Type *ty, Token **name) {
    ty = pointers(ty);
    if (P("(")) {
        const Token *n = tok->next;
        int nested = (n->k == TK_PUNCT && (tok_is(n, "*") || tok_is(n, "(") || tok_is(n, "["))) || (n->k == TK_ID && !find_typedef(n)) || (n->k == TK_KW && tok_is(n, "__attribute__"));
        if (nested) {
            Token *start = tok->next;
            int d = 1;
            Token *t = start;
            while (d) {
                if (t->k == TK_EOF) kerr_tok(start, "unbalanced parentheses in declarator");
                if (t->k == TK_PUNCT && tok_is(t, "(")) d++;
                else if (t->k == TK_PUNCT && tok_is(t, ")")) d--;
                t = t->next;
            }
            tok = t;
            ty = type_suffix(ty);
            Token *end = tok;
            tok = start;
            Attr d2 = {0};
            attrs(&d2);
            ty = declarator(ty, name);
            expect(")");
            tok = end;
            return ty;
        }
    }
    if (tok->k == TK_ID && name) *name = expect_id();
    else if (name) *name = NULL;
    return type_suffix(ty);
}

static Type *typename(void) {
    Type *base = declspec(NULL);
    Token *nm = NULL;
    Type *ty = declarator(base, &nm);
    if (nm) kerr_tok(nm, "type name does not allow a declared identifier");
    return ty;
}

/* ---------- узлы и свёртка констант ---------- */
static Node *new_node(NKind k, const Token *t) {
    Node *n = ka_new(sizeof *n);
    n->k = k; n->tok = t;
    return n;
}
static Node *new_num(int64_t v, Type *ty, const Token *t) {
    Node *n = new_node(ND_NUM, t);
    n->val = v; n->ty = ty;
    return n;
}
static int64_t trunc_ty(int64_t v, const Type *ty) {
    if (ty->k == TY_BOOL) return v != 0;
    switch (ty->size) {
    case 1: return ty->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
    case 2: return ty->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
    case 4: return ty->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
    default: return v;
    }
}
static int is_lval(const Node *n) {
    switch (n->k) {
    case ND_VAR: case ND_DEREF: return 1;
    case ND_MEMBER: return is_lval(n->lhs);
    case ND_COMMA: return n->op == 1;
    case ND_ASSIGN: return ty_is_agg(n->ty);
    default: return 0;
    }
}
static Node *rv(Node *n) {
    if (n->ty->k == TY_ARRAY) {
        Node *a = new_node(ND_ADDR, n->tok);
        a->lhs = n; a->ty = ty_ptr(n->ty->base);
        return a;
    }
    if (n->ty->k == TY_FUNC) {
        Node *a = new_node(ND_ADDR, n->tok);
        a->lhs = n; a->ty = ty_ptr(n->ty);
        return a;
    }
    return n;
}
static Node *new_fnum(uint64_t bits, Type *ty, const Token *t) {
    Node *n = new_node(ND_FNUM, t);
    n->val = (int64_t)bits; n->ty = ty;
    return n;
}
static Node *new_cast(Node *e, Type *ty, const Token *t) {
    e = rv(e);
    Type *from = e->ty;
    if (ty->k == TY_VOID) {
        Node *n = new_node(ND_CAST, t);
        n->lhs = e; n->ty = ty;
        return n;
    }
    if (ty_is_agg(ty)) {
        if (ty_is_agg(from) && ty_unqual(from) == ty_unqual(ty)) return e;
        kerr_tok(t, "used type '%s' where arithmetic or pointer type is required", ty_name(ty));
    }
    if (!ty_is_scalar(ty)) kerr_tok(t, "cannot cast to type '%s'", ty_name(ty));
    if (!ty_is_scalar(from)) kerr_tok(t, "operand of type '%s' where arithmetic or pointer type is required", ty_name(from));
    if (ty_is_float(ty) || ty_is_float(from)) {
        if (ty->k == TY_PTR || from->k == TY_PTR) kerr_tok(t, "cannot convert between '%s' and '%s'", ty_name(from), ty_name(ty));
        if (e->k == ND_FNUM || e->k == ND_NUM) {                /* свёртка константы программной арифметикой softfp.c */
            if (ty_is_float(ty)) {
                uint64_t b = e->k == ND_FNUM ? sf_conv((uint64_t)e->val, from->size, ty->size)
                                             : sf_from_int(e->val, from->is_unsigned && from->k != TY_BOOL, ty->size);
                return new_fnum(b, ty, t);
            }
            int ok;
            if (ty->k == TY_BOOL) return new_num(sf_cmp((uint64_t)e->val, 0, from->size) != 0, ty, t);
            int64_t v = sf_to_int((uint64_t)e->val, from->size, ty->is_unsigned && ty->size == 8, &ok);
            return new_num(trunc_ty(v, ty), ty, t);
        }
    }
    if (e->k == ND_NUM) return new_num(trunc_ty(e->val, ty), ty, t);
    Node *n = new_node(ND_CAST, t);
    n->lhs = e; n->ty = ty;
    return n;
}

/* неявное приведение при присваивании/инициализации/передаче аргумента/return */
static Node *conv_assign(Node *e, Type *to, const Token *t, const char *what) {
    e = rv(e);
    Type *from = e->ty;
    if (to->k == TY_VOID || to->k == TY_ARRAY || to->k == TY_FUNC)
        kerr_tok(t, "%s: invalid target type '%s'", what, ty_name(to));
    if (ty_is_agg(to)) {
        if (ty_is_agg(from) && ty_unqual(from) == ty_unqual(to)) return e;
        kerr_tok(t, "%s '%s' from incompatible type '%s'", what, ty_name(to), ty_name(from));
    }
    if (ty_is_arith(to) && ty_is_arith(from)) return new_cast(e, to, t);
    if (to->k == TY_PTR && from->k == TY_PTR) return e->ty == to ? e : new_cast(e, to, t);
    if (to->k == TY_PTR && ty_is_int(from)) {
        if (e->k == ND_NUM && e->val == 0) return new_cast(e, to, t);
        kerr_tok(t, "incompatible integer to pointer conversion %s '%s' from '%s'", what, ty_name(to), ty_name(from));
    }
    if (ty_is_int(to) && from->k == TY_PTR) {
        if (to->k == TY_BOOL) return new_cast(e, to, t);
        kerr_tok(t, "incompatible pointer to integer conversion %s '%s' from '%s'", what, ty_name(to), ty_name(from));
    }
    kerr_tok(t, "%s '%s' from incompatible type '%s'", what, ty_name(to), ty_name(from));
}

static int fold_bin(NKind k, int64_t a, int64_t b, const Type *ty, int64_t *out) {
    int u = ty->is_unsigned;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    int64_t r;
    switch (k) {
    case ND_ADD: r = (int64_t)(ua + ub); break;
    case ND_SUB: r = (int64_t)(ua - ub); break;
    case ND_MUL: r = (int64_t)(ua * ub); break;
    case ND_DIV:
        if (b == 0) return 0;
        if (!u && b == -1) { r = (int64_t)(0 - ua); break; }
        r = u ? (int64_t)(ua / ub) : a / b; break;
    case ND_MOD:
        if (b == 0) return 0;
        if (!u && b == -1) { r = 0; break; }
        r = u ? (int64_t)(ua % ub) : a % b; break;
    case ND_AND: r = a & b; break;
    case ND_OR: r = a | b; break;
    case ND_XOR: r = a ^ b; break;
    case ND_EQ: *out = a == b; return 1;
    case ND_NE: *out = a != b; return 1;
    case ND_LT: *out = u ? ua < ub : a < b; return 1;
    case ND_LE: *out = u ? ua <= ub : a <= b; return 1;
    default: return 0;
    }
    *out = trunc_ty(r, ty);
    return 1;
}

static Node *new_bin(NKind k, Node *l, Node *r, Type *ty, const Token *t) {
    Node *n = new_node(k, t);
    n->lhs = l; n->rhs = r; n->ty = ty;
    return n;
}

static Node *arith(NKind k, Node *l, Node *r, const Token *t) {
    l = rv(l); r = rv(r);
    if (!ty_is_arith(l->ty) || !ty_is_arith(r->ty))
        kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
    Type *ct = ty_common(l->ty, r->ty);
    if (ty_is_float(ct)) {
        if (k != ND_ADD && k != ND_SUB && k != ND_MUL && k != ND_DIV)
            kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
        l = new_cast(l, ct, t); r = new_cast(r, ct, t);
        if (l->k == ND_FNUM && r->k == ND_FNUM)
            return new_fnum(sf_binop(k == ND_ADD ? '+' : k == ND_SUB ? '-' : k == ND_MUL ? '*' : '/', (uint64_t)l->val, (uint64_t)r->val, ct->size), ct, t);
        return new_bin(k, l, r, ct, t);
    }
    l = new_cast(l, ct, t); r = new_cast(r, ct, t);
    int64_t v;
    if (l->k == ND_NUM && r->k == ND_NUM && fold_bin(k, l->val, r->val, ct, &v)) return new_num(v, ct, t);
    return new_bin(k, l, r, ct, t);
}

/* размер элемента, на который указывает pt: константа либо (для указателя на VLA) выражение времени выполнения */
static Node *elem_size(Type *pt, const Token *t) {
    Type *b = pt->base;
    if (b->k == TY_FUNC || b->k == TY_VOID) return NULL;   /* NULL - размер 1 */
    if (b->vm) return new_cast(size_node(b, t), &ty_long, t);
    if (!ty_unqual(b)->complete && b->k != TY_ARRAY) kerr_tok(t, "arithmetic on a pointer to an incomplete type '%s'", ty_name(b));
    if (b->k == TY_ARRAY && b->alen < 0) kerr_tok(t, "arithmetic on a pointer to an incomplete type '%s'", ty_name(b));
    return TSZ(b) == 1 ? NULL : new_num(TSZ(b), &ty_long, t);
}

static Node *scaled(Node *idx, Node *sz, const Token *t) {
    idx = new_cast(idx, &ty_long, t);
    if (!sz) return idx;
    return arith(ND_MUL, idx, sz, t);
}

static Node *new_add(Node *l, Node *r, const Token *t) {
    l = rv(l); r = rv(r);
    if (ty_is_arith(l->ty) && ty_is_arith(r->ty)) return arith(ND_ADD, l, r, t);
    if (l->ty->k != TY_PTR && r->ty->k == TY_PTR) { Node *x = l; l = r; r = x; }
    if (l->ty->k == TY_PTR && ty_is_int(r->ty)) {
        Node *s = scaled(r, elem_size(l->ty, t), t);
        return new_bin(ND_ADD, l, s, l->ty, t);
    }
    kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
}

static Node *new_sub(Node *l, Node *r, const Token *t) {
    l = rv(l); r = rv(r);
    if (ty_is_arith(l->ty) && ty_is_arith(r->ty)) return arith(ND_SUB, l, r, t);
    if (l->ty->k == TY_PTR && ty_is_int(r->ty)) {
        Node *s = scaled(r, elem_size(l->ty, t), t);
        return new_bin(ND_SUB, l, s, l->ty, t);
    }
    if (l->ty->k == TY_PTR && r->ty->k == TY_PTR) {
        if (!ty_compat(l->ty->base, r->ty->base)) kerr_tok(t, "'%s' and '%s' are not pointers to compatible types", ty_name(l->ty), ty_name(r->ty));
        Node *sz = elem_size(l->ty, t);
        Node *d = new_bin(ND_SUB, new_cast(l, &ty_long, t), new_cast(r, &ty_long, t), &ty_long, t);
        return !sz ? d : arith(ND_DIV, d, sz, t);
    }
    kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
}

static Node *new_cmp(NKind k, Node *l, Node *r, const Token *t) {
    l = rv(l); r = rv(r);
    Type *ct;
    if (ty_is_arith(l->ty) && ty_is_arith(r->ty)) {
        ct = ty_common(l->ty, r->ty);
        l = new_cast(l, ct, t); r = new_cast(r, ct, t);
        if (l->k == ND_FNUM && r->k == ND_FNUM) {               /* NaN: любое сравнение, кроме !=, ложно */
            int c = sf_cmp((uint64_t)l->val, (uint64_t)r->val, ct->size);
            return new_num(k == ND_EQ ? c == 0 : k == ND_NE ? c != 0 : k == ND_LT ? c == -1 : (c == -1 || c == 0), &ty_int, t);
        }
    } else if ((l->ty->k == TY_PTR || ty_is_int(l->ty)) && (r->ty->k == TY_PTR || ty_is_int(r->ty))) {
        ct = &ty_ulong;
        l = new_cast(l, ct, t); r = new_cast(r, ct, t);
    } else kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
    int64_t v;
    if (l->k == ND_NUM && r->k == ND_NUM && fold_bin(k, l->val, r->val, ct, &v)) return new_num(v, &ty_int, t);
    return new_bin(k, l, r, &ty_int, t);
}

static Node *new_shift(NKind k, Node *l, Node *r, const Token *t) {
    l = rv(l); r = rv(r);
    if (!ty_is_int(l->ty) || !ty_is_int(r->ty))
        kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
    Type *lt = ty_promote(l->ty);
    l = new_cast(l, lt, t);
    r = new_cast(r, ty_promote(r->ty), t);
    if (l->k == ND_NUM && r->k == ND_NUM && r->val >= 0 && r->val < lt->size * 8) {
        int64_t v = k == ND_SHL ? (int64_t)((uint64_t)l->val << r->val)
                                : (lt->is_unsigned ? (int64_t)((uint64_t)l->val >> r->val) : l->val >> r->val);
        return new_num(trunc_ty(v, lt), lt, t);
    }
    return new_bin(k, l, r, lt, t);
}

static Node *to_cond(Node *e, const Token *t) {
    e = rv(e);
    if (!ty_is_scalar(e->ty)) kerr_tok(t, "statement requires expression of scalar type ('%s' invalid)", ty_name(e->ty));
    if (ty_is_float(e->ty)) return new_cmp(ND_NE, e, new_fnum(0, e->ty, t), t);       /* x != 0.0: для NaN истина, как у clang */
    return e;
}

static Node *new_logic(NKind k, Node *l, Node *r, const Token *t) {
    l = to_cond(l, t); r = to_cond(r, t);
    if (l->k == ND_NUM) {
        if (k == ND_LOGAND && !l->val) return new_num(0, &ty_int, t);
        if (k == ND_LOGOR && l->val) return new_num(1, &ty_int, t);
        if (r->k == ND_NUM) return new_num(r->val != 0, &ty_int, t);
    }
    return new_bin(k, l, r, &ty_int, t);
}

static Member *find_member(Type *st, const Token *name, Node **path_out, Node *base, const Token *t) {
    Type *u = ty_unqual(st);
    for (Member *m = u->mem; m; m = m->next) {
        if (m->name) {
            if ((int)strlen(m->name) == name->len && !memcmp(m->name, name->s, (size_t)name->len)) { *path_out = base; return m; }
        } else if (ty_is_agg(m->ty)) {
            Node *sub = new_node(ND_MEMBER, t);
            sub->lhs = base; sub->mem = m; sub->ty = m->ty;
            Member *r = find_member(m->ty, name, path_out, sub, t);
            if (r) return r;
        }
    }
    return NULL;
}

static Node *new_member(Node *base, const Token *name, const Token *t) {
    if (!ty_is_agg(base->ty)) kerr_tok(t, "member reference base type '%s' is not a structure or union", ty_name(base->ty));
    if (!ty_unqual(base->ty)->complete) kerr_tok(t, "member access into incomplete type '%s'", ty_name(base->ty));
    Node *path;
    Member *m = find_member(base->ty, name, &path, base, t);
    if (!m) kerr_tok(name, "no member named '%.*s' in '%s'", name->len, name->s, ty_name(base->ty));
    Node *n = new_node(ND_MEMBER, name);
    n->lhs = path; n->mem = m;
    Type *mt = m->ty;
    if (base->ty->is_const || path->ty->is_const) mt = ty_qual(mt, 1, 0);
    if (base->ty->is_volatile) mt = ty_qual(mt, 0, 1);
    if (m->bw >= 0 && m->ty->size <= 4 && m->bw < 32) mt = (m->ty->is_unsigned && m->bw == 32) ? m->ty : &ty_int;
    n->ty = mt;
    return n;
}

static Node *new_deref(Node *p, const Token *t) {
    p = rv(p);
    if (p->ty->k != TY_PTR) kerr_tok(t, "indirection requires pointer operand ('%s' invalid)", ty_name(p->ty));
    if (p->ty->base->k == TY_VOID) kerr_tok(t, "indirection on a pointer to void is not supported");
    if (!ty_unqual(p->ty->base)->complete && p->ty->base->k != TY_FUNC && p->ty->base->k != TY_ARRAY)
        kerr_tok(t, "incomplete type '%s' where a complete type is required", ty_name(p->ty->base));
    Node *n = new_node(ND_DEREF, t);
    n->lhs = p; n->ty = p->ty->base;
    return n;
}

static Node *new_addr(Node *e, const Token *t) {
    if (e->k == ND_MEMBER && e->mem->bw >= 0) kerr_tok(t, "address of bit-field requested");
    if (e->ty->k != TY_FUNC && !is_lval(e)) kerr_tok(t, "cannot take the address of an rvalue of type '%s'", ty_name(e->ty));
    Node *n = new_node(ND_ADDR, t);
    n->lhs = e; n->ty = ty_ptr(e->ty);
    return n;
}

static void check_modifiable(Node *l, const Token *t) {
    if (!is_lval(l)) kerr_tok(t, "expression is not assignable");
    if (l->ty->k == TY_ARRAY) kerr_tok(t, "array type '%s' is not assignable", ty_name(l->ty));
    if (l->ty->is_const) kerr_tok(t, "cannot assign to variable with const-qualified type '%s'", ty_name(l->ty));
    if (!ty_unqual(l->ty)->complete && l->ty->k != TY_PTR) kerr_tok(t, "incomplete type '%s' is not assignable", ty_name(l->ty));
}

static Node *new_assign(Node *l, Node *r, const Token *t) {
    check_modifiable(l, t);
    r = conv_assign(r, l->ty, t, "assigning to");
    Node *n = new_bin(ND_ASSIGN, l, r, ty_qual(l->ty, 0, 0) == l->ty ? l->ty : ty_unqual(l->ty), t);
    if (l->ty->is_volatile) n->ty = l->ty;
    return n;
}

static Node *new_opassign(NKind k, Node *l, Node *r, const Token *t) {
    check_modifiable(l, t);
    r = rv(r);
    Type *oty;
    if (l->ty->k == TY_PTR) {
        if ((k != ND_ADD && k != ND_SUB) || !ty_is_int(r->ty)) kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
        oty = l->ty;
        r = scaled(r, elem_size(l->ty, t), t);
    } else if (ty_is_arith(l->ty) && ty_is_arith(r->ty) && (ty_is_float(l->ty) || ty_is_float(r->ty))) {
        if (k != ND_ADD && k != ND_SUB && k != ND_MUL && k != ND_DIV)
            kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
        oty = ty_common(l->ty, r->ty); r = new_cast(r, oty, t);
    } else if (ty_is_int(l->ty) && ty_is_int(r->ty)) {
        if (k == ND_SHL || k == ND_SHR) { oty = ty_promote(l->ty); r = new_cast(r, ty_promote(r->ty), t); }
        else { oty = ty_common(l->ty, r->ty); r = new_cast(r, oty, t); }
    } else kerr_tok(t, "invalid operands to binary expression ('%s' and '%s')", ty_name(l->ty), ty_name(r->ty));
    Node *n = new_bin(ND_OPASSIGN, l, r, l->ty, t);
    n->op = (int)k; n->oty = oty;
    return n;
}

static Node *new_incdec(Node *e, int delta, int post, const Token *t) {
    check_modifiable(e, t);
    if (!ty_is_scalar(e->ty) || e->ty->k == TY_BOOL) {
        if (e->ty->k != TY_BOOL || 1) kerr_tok(t, "cannot increment or decrement value of type '%s'", ty_name(e->ty));
    }
    Node *n = new_node(ND_INCDEC, t);
    n->lhs = e; n->ty = e->ty; n->val = delta; n->op = post;
    if (e->ty->k == TY_PTR) {
        Node *es = elem_size(e->ty, t);
        n->oty = (Type *)(intptr_t)0; n->mem = NULL;
        n->rhs = es ? es : new_num(1, &ty_long, t);
    }
    return n;
}

/* ---------- литералы строк, переменные ---------- */
static Obj *add_global(Obj *o) {
    if (globals_tail) globals_tail->next = o; else globals_head = o;
    globals_tail = o;
    return o;
}

static Obj *new_str_obj(const char *s, int len, const Token *t) {
    Obj *o = ka_new(sizeof *o);
    o->name = kfmt(".Lstr.%d", str_count++);
    o->ty = ty_array(&ty_char, len + 1);
    o->is_str = 1; o->is_ro = 1; o->is_def = 1; o->is_static = 1; o->align = 1; o->line = t->line;
    o->init = ka_new((size_t)len + 1);
    memcpy(o->init, s, (size_t)len);
    add_global(o);
    return o;
}

static Node *var_node(Obj *o, const Token *t) {
    Node *n = new_node(ND_VAR, t);
    n->var = o; n->ty = o->ty;
    o->used = 1;
    return n;
}

static Obj *new_lvar(const Token *nm, Type *ty) {
    Obj *o = ka_new(sizeof *o);
    o->name = nm ? tname(nm) : ".tmp";
    o->ty = ty; o->is_local = 1; o->slot = nlocals++; o->align = ty->align; o->line = nm ? nm->line : 0;
    if (!cur_fn) kerr_tok(nm, "internal: local variable outside function");
    if (fn_locals_tail) fn_locals_tail->next = o; else cur_fn->locals = o;
    fn_locals_tail = o;
    if (nm) {
        if (find_in(scope, nm->s, nm->len)) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
        VS *v = add_vs(nm);
        v->var = o;
    }
    return o;
}


/* ---------- VLA ---------- */
static Obj *hidden_lvar(Type *ty) {
    if (!in_params) return new_lvar(NULL, ty);
    Obj *o = ka_new(sizeof *o);                      /* список параметров разбирается до начала функции: объект привяжется к ней в func_def */
    o->name = ".vla"; o->ty = ty; o->is_local = 1; o->slot = -1; o->align = ty->align;
    o->next = vla_objs; vla_objs = o;
    return o;
}

static Obj *vla_make_len(Node *e, const Token *t) {
    if (!in_params && !cur_fn) kerr_tok(t, "variable length array declaration is not allowed at file scope");
    Obj *o = hidden_lvar(&ty_long);
    Node *as = new_bin(ND_ASSIGN, var_node(o, t), new_cast(rv(e), &ty_long, t), &ty_long, t);
    if (vla_tail) vla_tail->next = as; else vla_head = as;
    vla_tail = as;
    return o;
}

static Node *vla_take(void) {
    Node *h = vla_head;
    vla_head = vla_tail = NULL;
    return h;
}

/* (p1, (p2, ... e)): присваивания длин, затем значение e */
static Node *vla_chain(Node *p, Node *e, const Token *t) {
    if (!p) return e;
    Node *nx = p->next;
    p->next = NULL;
    return new_bin(ND_COMMA, p, vla_chain(nx, e, t), e->ty, t);
}

/* размер типа в байтах (ulong): константа либо произведение скрытых длин и размера элемента */
static Node *size_node(Type *ty, const Token *t) {
    if (!ty->vm) return new_num(TSZ(ty), &ty_ulong, t);
    Node *n = ty->vlen ? new_cast(var_node(ty->vlen, t), &ty_ulong, t) : new_num(ty->alen, &ty_ulong, t);
    return new_bin(ND_MUL, n, size_node(ty->base, t), &ty_ulong, t);
}

static Node *new_expr_stmt(Node *e, const Token *t);
static Node *sp_restore(Obj *sv, const Token *t) {
    Node *n = new_node(ND_SPREST, t);
    n->lhs = var_node(sv, t); n->ty = &ty_void;
    return n;
}
static Node *vla_snapshot(const Token *t) {
    Node *h = NULL, *tl = NULL;
    for (int i = 0; i < vla_depth; i++) {
        Node *v = var_node(vla_stack[i], t);
        if (tl) tl->next = v; else h = v;
        tl = v;
    }
    return h;
}
/* выход из областей VLA до глубины d (break/continue): sp возвращается к значению, сохранённому при входе в первую из них */
static Node *vla_leave(int d, const Token *t) {
    return vla_depth > d ? sp_restore(vla_stack[d], t) : NULL;
}

/* ---------- константные выражения ---------- */
static int64_t const_expr(void) {
    const Token *t = tok;
    Node *e = cond_expr();
    if (e->k != ND_NUM || !(ty_is_int(e->ty))) kerr_tok(t, "expression is not an integer constant expression");
    return e->val;
}

/* ---------- выражения ---------- */
static Obj *find_global_fn(const char *name) {
    for (Obj *o = globals_head; o; o = o->next) if (o->is_func && !strcmp(o->name, name)) return o;
    return NULL;
}

static Obj *builtin_decl(const char *name, const Token *t) {
    Obj *o = find_global_fn(name);
    if (o) return o;
    Param *p1 = ka_new(sizeof *p1), *p2 = ka_new(sizeof *p2), *p3 = ka_new(sizeof *p3);
    Type *vp = ty_ptr(&ty_void), *cvp = ty_ptr(ty_qual(&ty_void, 1, 0));
    Type *ft;
    if (!strcmp(name, "memcpy")) { p1->ty = vp; p2->ty = cvp; p3->ty = &ty_ulong; p1->next = p2; p2->next = p3; ft = ty_func(vp, p1, 0, 0); }
    else if (!strcmp(name, "memset")) { p1->ty = vp; p2->ty = &ty_int; p3->ty = &ty_ulong; p1->next = p2; p2->next = p3; ft = ty_func(vp, p1, 0, 0); }
    else if (!strcmp(name, "memcmp")) { p1->ty = cvp; p2->ty = cvp; p3->ty = &ty_ulong; p1->next = p2; p2->next = p3; ft = ty_func(&ty_int, p1, 0, 0); }
    else { p1->ty = ty_ptr(ty_qual(&ty_char, 1, 0)); ft = ty_func(&ty_ulong, p1, 0, 0); }
    Token synth = *t;
    synth.s = name; synth.len = (int)strlen(name);
    o = new_gvar(&synth, ft);
    o->is_func = 1;
    return o;
}

static Node *funcall(Node *fn, const Token *t) {
    fn = rv(fn);
    Type *ft = fn->ty;
    if (ft->k == TY_PTR && ft->base->k == TY_FUNC) ft = ft->base;
    else kerr_tok(t, "called object type '%s' is not a function or function pointer", ty_name(fn->ty));
    Param *p = ft->params;
    Node head = {0}, *cur = &head;
    int n = 0;
    while (!P(")")) {
        if (n) expect(",");
        const Token *at = tok;
        Node *a = rv(assign());
        if (p) {
            a = conv_assign(a, p->ty, at, "passing to parameter of type");
            p = p->next;
        } else if (ft->noproto || ft->variadic) {
            if (ty_is_agg(a->ty) && !ty_unqual(a->ty)->complete) kerr_tok(at, "passing incomplete type");
            if (ty_is_int(a->ty)) a = new_cast(a, ty_promote(a->ty), at);
            else if (a->ty->k == TY_FLOAT) a = new_cast(a, &ty_double, at);       /* аргумент без прототипа/вариадический: float -> double */
        } else kerr_tok(at, "too many arguments to function call, expected %d, have more", n);
        cur->next = a; cur = a;
        n++;
    }
    expect(")");
    if (p) kerr_tok(t, "too few arguments to function call, have %d", n);
    if (ty_is_agg(ft->base) && !ty_unqual(ft->base)->complete) kerr_tok(t, "calling function with incomplete return type '%s'", ty_name(ft->base));
    Node *c = new_node(ND_CALL, t);
    c->lhs = fn; c->args = head.next; c->ty = ty_unqual(ft->base);
    return c;
}

static Node *stmt_expr(const Token *t) {
    if (!cur_fn) kerr_tok(t, "statement expression not allowed at file scope");
    stmtexpr_next = 1;
    Node *blk = compound(1);
    Node *last = NULL;
    for (Node *s = blk->body; s; s = s->next) last = s;
    Node *n = new_node(ND_STMTEXPR, t);
    n->body = blk; n->ty = &ty_void;
    if (last && last->k == ND_EXPR) {
        last->lhs = rv(last->lhs);
        if (!ty_is_scalar(last->lhs->ty) && last->lhs->ty->k != TY_VOID) unsupported(t, "statement expression with a non-scalar value");
        n->ty = last->lhs->ty;
    }
    return n;
}

static Node *init_block(Init *in, Node *lv, const Token *t);
static void write_data(Init *in, Type *ty, unsigned char *buf, int off, Obj *o, const Token *t);

static Node *complit(Type *ty, const Token *t) {
    Type *nt;
    if (!cur_fn) {
        Obj *o = ka_new(sizeof *o);
        o->name = kfmt(".Lcl.%d", static_count++);
        o->is_static = 1; o->is_def = 1; o->line = t->line;
        Init *in = initializer(ty, &nt);
        o->ty = nt; o->align = nt->align;
        o->init = ka_new((size_t)(nt->size ? nt->size : 1));
        write_data(in, nt, o->init, 0, o, t);
        add_global(o);
        return var_node(o, t);
    }
    Obj *o = new_lvar(NULL, ty);
    Init *in = initializer(ty, &nt);
    o->ty = nt; o->align = nt->align;
    Node *v = var_node(o, t);
    Node *blk = init_block(in, v, t);
    Node *se = new_node(ND_STMTEXPR, t);
    se->body = blk; se->ty = &ty_void;
    Node *c = new_bin(ND_COMMA, se, var_node(o, t), nt, t);
    c->op = 1;
    return c;
}

static Node *offsetof_expr(const Token *t) {
    expect("(");
    Type *ty = typename();
    expect(",");
    if (!ty_is_agg(ty)) kerr_tok(t, "offsetof requires struct or union type");
    int64_t off = 0;
    Type *cur = ty;
    int first = 1;
    for (;;) {
        if (first || eat(".")) {
            Token *nm = expect_id();
            Node *path;
            if (!ty_is_agg(cur) || !ty_unqual(cur)->complete) kerr_tok(nm, "offsetof on incomplete or non-aggregate type");
            Node root = {0};
            root.ty = cur; root.k = ND_VAR;
            Member *m = find_member(cur, nm, &path, &root, nm);
            if (!m) kerr_tok(nm, "no member named '%.*s' in '%s'", nm->len, nm->s, ty_name(cur));
            if (m->bw >= 0) kerr_tok(nm, "cannot compute offset of bit-field");
            for (Node *p = path; p != &root; p = p->lhs) off += p->mem->off;
            off += m->off;
            cur = m->ty;
            first = 0;
        } else if (eat("[")) {
            int64_t i = const_expr();
            expect("]");
            if (cur->k != TY_ARRAY) kerr_tok(t, "offsetof subscript on non-array");
            off += i * TSZ(cur->base);
            cur = cur->base;
        } else break;
    }
    expect(")");
    return new_num(off, &ty_ulong, t);
}


/* ---------- va_list: __builtin_va_start/va_arg/va_end/va_copy ---------- */
/* Указатель на структуру va_list из выражения ap (массив x86-64, структура-lvalue AAPCS64 или уже указатель параметра). */
static Node *va_ptr(Node *ap, const Token *t) {
    va_type();
    Type *at = ty_unqual(ap->ty);
    if (at->k == TY_ARRAY || at->k == TY_PTR) {
        Node *p = rv(ap);
        if (p->ty->k == TY_PTR && ty_unqual(p->ty->base) == va_st) return p;
    }
    if (at == va_st) return new_addr(ap, t);
    kerr_tok(t, "argument is not a va_list");
}

static Node *va_builtin(const Token *t, const char *nm, int l) {
    if (l == 5 && !memcmp(nm, "start", 5)) {
        expect("(");
        Node *ap = assign();
        if (eat(",")) assign();
        expect(")");
        if (!cur_fn || !cur_fn->ty->variadic) kerr_tok(t, "'va_start' used in function with fixed args");
        Node *n = new_node(ND_VASTART, t);
        n->lhs = va_ptr(ap, t); n->ty = &ty_void;
        return n;
    }
    if (l == 3 && !memcmp(nm, "end", 3)) {
        expect("(");
        Node *ap = assign();
        expect(")");
        va_ptr(ap, t);
        return new_cast(new_num(0, &ty_int, t), &ty_void, t);
    }
    if (l == 4 && !memcmp(nm, "copy", 4)) {
        expect("(");
        Node *d = assign(); expect(","); Node *s2 = assign(); expect(")");
        Node *dd = new_deref(va_ptr(d, t), t), *ss = new_deref(va_ptr(s2, t), t);
        return new_assign(dd, ss, t);
    }
    if (l == 3 && !memcmp(nm, "arg", 3)) {
        expect("(");
        Node *ap = assign(); expect(",");
        Type *ty = typename();
        expect(")");
        if (!ty_is_scalar(ty)) unsupported(t, "va_arg of a struct/union type");
        if (ty->k == TY_FLOAT) kerr_tok(t, "second argument to 'va_arg' is of promotable type 'float'; use 'double'");
        Node *n = new_node(ND_VAARG, t);
        n->lhs = va_ptr(ap, t); n->ty = ty_unqual(ty);
        return n;
    }
    kerr_tok(t, "unsupported builtin '%.*s'", t->len, t->s);
}

/* ---------- __atomic_* и __sync_*: все операции выполняются с порядком seq_cst (порядок из аргументов проверяется и принимается) ---------- */
static Node *at_node(int sub, Node *p, Node *v, Node *d, Type *ty, const Token *t) {
    Node *n = new_node(ND_ATOMIC, t);
    n->op = sub; n->lhs = p; n->rhs = v; n->c = d; n->ty = ty;
    return n;
}
static Node *at_ptr(const Token *t) {                 /* аргумент-указатель на 1/2/4/8-байтовое целое или указатель */
    Node *p = rv(assign());
    if (p->ty->k != TY_PTR) kerr_tok(t, "address argument of '%.*s' is not a pointer", t->len, t->s);
    Type *b = ty_unqual(p->ty->base);
    if (!(ty_is_int(b) || b->k == TY_PTR) || (b->size != 1 && b->size != 2 && b->size != 4 && b->size != 8))
        kerr_tok(t, "'%.*s' needs a pointer to an integer or pointer of size 1, 2, 4 or 8", t->len, t->s);
    return p;
}
static void at_order(const Token *t) {
    Node *o = rv(assign());
    if (o->k != ND_NUM || !ty_is_int(o->ty)) kerr_tok(t, "memory order argument of '%.*s' must be a constant", t->len, t->s);
    if (o->val < 0 || o->val > 5) kerr_tok(t, "invalid memory order %lld", (long long)o->val);
}
static Node *at_val(Node *p, int arith, const Token *t) {   /* значение, приведённое к типу объекта (для +,- и т.п. над указателем - как к целому) */
    Type *b = ty_unqual(p->ty->base);
    Node *v = rv(assign());
    return conv_assign(v, arith && b->k == TY_PTR ? &ty_long : b, t, "passing atomic operand of type");
}

static Node *atomic_builtin(const Token *t) {
    char nm[48];
    int l = t->len;
    if (l >= (int)sizeof nm) kerr_tok(t, "unsupported builtin '%.*s'", t->len, t->s);
    memcpy(nm, t->s, (size_t)l); nm[l] = 0;
    int sync = nm[2] == 's';
    const char *b = nm + (sync ? 7 : 9);
    expect("(");
    Node *n = NULL;
    static const struct { const char *s; int sub; } ar[] = { { "add", AT_ADD }, { "sub", AT_SUB }, { "and", AT_AND }, { "or", AT_OR },
                                                           { "xor", AT_XOR }, { "nand", AT_NAND }, { 0, 0 } };
    if (!sync && (!strcmp(b, "thread_fence") || !strcmp(b, "signal_fence"))) {
        at_order(t); expect(")");
        return b[0] == 't' ? at_node(AT_FENCE, NULL, NULL, NULL, &ty_void, t) : new_cast(new_num(0, &ty_int, t), &ty_void, t);
    }
    if (sync && !strcmp(b, "synchronize")) { expect(")"); return at_node(AT_FENCE, NULL, NULL, NULL, &ty_void, t); }
    if (!sync && (!strcmp(b, "always_lock_free") || !strcmp(b, "is_lock_free"))) {
        Node *sz = rv(assign());
        if (eat(",")) assign();
        expect(")");
        if (sz->k != ND_NUM) kerr_tok(t, "size argument of '%.*s' must be a constant", t->len, t->s);
        return new_num(sz->val == 1 || sz->val == 2 || sz->val == 4 || sz->val == 8, &ty_int, t);
    }
    if (!sync && !strcmp(b, "test_and_set")) {
        Node *p = rv(assign());
        if (eat(",")) at_order(t);
        expect(")");
        if (p->ty->k != TY_PTR) kerr_tok(t, "argument of '%.*s' is not a pointer", t->len, t->s);
        p = new_cast(p, ty_ptr(&ty_uchar), t);
        return new_cast(at_node(AT_XCHG, p, new_num(1, &ty_uchar, t), NULL, &ty_uchar, t), &ty_bool, t);
    }
    if (!sync && !strcmp(b, "clear")) {
        Node *p = rv(assign());
        if (eat(",")) at_order(t);
        expect(")");
        if (p->ty->k != TY_PTR) kerr_tok(t, "argument of '%.*s' is not a pointer", t->len, t->s);
        p = new_cast(p, ty_ptr(&ty_uchar), t);
        return at_node(AT_STORE, p, new_num(0, &ty_uchar, t), NULL, &ty_void, t);
    }
    if (sync && !strcmp(b, "lock_release")) {
        Node *p = at_ptr(t);
        while (eat(",")) assign();
        expect(")");
        return at_node(AT_STORE, p, new_num(0, &ty_int, t), NULL, &ty_void, t);
    }
    Node *p = at_ptr(t);
    Type *T = ty_unqual(p->ty->base);
    if (!strcmp(b, "load_n") || !strcmp(b, "load")) {
        int gen = b[4] == 0;
        Node *ret = NULL;
        if (gen) { expect(","); ret = rv(assign()); }
        expect(","); at_order(t); expect(")");
        n = at_node(AT_LOAD, p, NULL, NULL, T, t);
        if (gen) return new_assign(new_deref(ret, t), n, t);
        return n;
    }
    if (!strcmp(b, "store_n") || !strcmp(b, "store")) {
        int gen = b[5] == 0;
        expect(",");
        Node *v;
        if (gen) v = conv_assign(new_deref(rv(assign()), t), T, t, "storing atomic operand of type"); else v = at_val(p, 0, t);
        expect(","); at_order(t); expect(")");
        return at_node(AT_STORE, p, v, NULL, &ty_void, t);
    }
    if (!strcmp(b, "exchange_n") || !strcmp(b, "exchange")) {
        int gen = b[8] == 0;
        expect(",");
        Node *v, *ret = NULL;
        if (gen) { v = conv_assign(new_deref(rv(assign()), t), T, t, "passing atomic operand of type"); expect(","); ret = rv(assign()); }
        else v = at_val(p, 0, t);
        expect(","); at_order(t); expect(")");
        n = at_node(AT_XCHG, p, v, NULL, T, t);
        return gen ? new_assign(new_deref(ret, t), n, t) : n;
    }
    if (!strcmp(b, "compare_exchange_n") || !strcmp(b, "compare_exchange")) {
        int gen = b[16] == 0;
        expect(",");
        Node *ep = rv(assign());
        if (ep->ty->k != TY_PTR) kerr_tok(t, "expected-value argument of '%.*s' is not a pointer", t->len, t->s);
        ep = new_cast(ep, ty_ptr(T), t);
        expect(",");
        Node *d = gen ? conv_assign(new_deref(rv(assign()), t), T, t, "passing atomic operand of type") : at_val(p, 0, t);
        expect(","); (void)rv(assign()); expect(","); at_order(t); expect(","); at_order(t); expect(")");
        n = at_node(AT_CAS, p, ep, d, &ty_int, t);
        return new_cast(n, &ty_bool, t);
    }
    if (sync && (!strcmp(b, "bool_compare_and_swap") || !strcmp(b, "val_compare_and_swap"))) {
        expect(",");
        Node *o = at_val(p, 0, t);
        expect(",");
        Node *d = at_val(p, 0, t);
        while (eat(",")) assign();
        expect(")");
        n = at_node(AT_CAS, p, o, d, b[0] == 'b' ? &ty_int : T, t);
        n->val = b[0] == 'b' ? 2 : 1;
        return n;
    }
    if (sync && !strcmp(b, "lock_test_and_set")) {
        expect(",");
        Node *v = at_val(p, 0, t);
        while (eat(",")) assign();
        expect(")");
        return at_node(AT_XCHG, p, v, NULL, T, t);
    }
    /* fetch_OP / OP_fetch (GCC) и fetch_and_OP / OP_and_fetch (sync) */
    int newv = 0, sub = 0;
    const char *op = NULL;
    if (!strncmp(b, "fetch_and_", 10)) op = b + 10;
    else if (!strncmp(b, "fetch_", 6)) op = b + 6;
    if (op) for (int i = 0; ar[i].s; i++) if (!strcmp(op, ar[i].s)) sub = ar[i].sub;
    if (!sub) {
        for (int i = 0; ar[i].s; i++) {
            size_t k = strlen(ar[i].s);
            if (!strncmp(b, ar[i].s, k) && (!strcmp(b + k, "_fetch") || !strcmp(b + k, "_and_fetch"))) { sub = ar[i].sub; newv = 1; }
        }
    }
    if (!sub) kerr_tok(t, "unsupported builtin '%.*s'", t->len, t->s);
    expect(",");
    Node *v = at_val(p, 1, t);
    if (sync) { while (eat(",")) assign(); } else { expect(","); at_order(t); }
    expect(")");
    n = at_node(sub, p, v, NULL, T, t);
    n->val = newv;
    return n;
}

/* _Generic: совпадение типа по правилам совместимости C11, с учётом квалификаторов указуемых типов. */
static int generic_same(const Type *a, const Type *b) {
    if (!ty_compat(a, b)) return 0;
    while (a->k == TY_PTR && b->k == TY_PTR) {
        a = a->base; b = b->base;
        if (a->is_const != b->is_const || a->is_volatile != b->is_volatile) return 0;
    }
    return 1;
}
static Node *generic_select(void) {
    const Token *t = tok;
    tok = tok->next;
    expect("(");
    Node *ctl = assign();
    Type *ct = ctl->ty;
    if (ct->k == TY_ARRAY) ct = ty_ptr(ct->base);
    else if (ct->k == TY_FUNC) ct = ty_ptr(ct);
    ct = ty_unqual(ct);
    Node *pick = 0, *dflt = 0;
    int ndef = 0;
    while (eat(",")) {
        if (KW("default")) {
            tok = tok->next;
            expect(":");
            if (ndef++) kerr_tok(tok, "duplicate default generic association");
            dflt = assign();
            continue;
        }
        Type *ty = typename();
        expect(":");
        Node *e = assign();
        if (generic_same(ct, ty)) {
            if (pick) kerr_tok(t, "multiple generic associations match the controlling type");
            pick = e;
        }
    }
    expect(")");
    if (!pick) pick = dflt;
    if (!pick) kerr_tok(t, "controlling expression type '%s' not compatible with any generic association type", ty_name(ct));
    return pick;
}

static Node *primary(void) {
    const Token *t = tok;
    if (P("(")) {
        if (tok->next->k == TK_PUNCT && tok_is(tok->next, "{")) {
            tok = tok->next;
            Node *n = stmt_expr(t);
            expect(")");
            return n;
        }
        if (is_typename(tok->next)) {
            tok = tok->next;
            Type *ty = typename();
            expect(")");
            if (!P("{")) kerr_tok(tok, "expected '{' for compound literal");
            return complit(ty, t);
        }
        tok = tok->next;
        Node *e = expr();
        expect(")");
        return e;
    }
    if (tok->k == TK_NUM || tok->k == TK_CHR) {
        tok = tok->next;
        if (ty_is_float(t->ty)) return new_fnum((uint64_t)t->ival, t->ty, t);
        return new_num(t->ival, t->ty, t);
    }
    if (tok->k == TK_STR) {
        tok = tok->next;
        return var_node(new_str_obj(t->str, t->slen, t), t);
    }
    if (tok->k == TK_ID) {
        if (t->len >= 10 && !memcmp(t->s, "__builtin_", 10)) {
            const char *nm = t->s + 10; int l = t->len - 10;
            tok = tok->next;
            if (l == 6 && !memcmp(nm, "expect", 6)) {
                expect("(");
                Node *a = rv(assign()); expect(","); assign(); expect(")");
                return new_cast(a, &ty_long, t);
            }
            if (l == 8 && !memcmp(nm, "offsetof", 8)) return offsetof_expr(t);
            if (l == 6 && !memcmp(nm, "alloca", 6)) {                    /* блок живёт до выхода из функции */
                if (!cur_fn) kerr_tok(t, "'__builtin_alloca' used outside of a function");
                expect("(");
                Node *a = new_cast(rv(assign()), &ty_ulong, t);
                expect(")");
                Node *n = new_node(ND_ALLOCA, t);
                n->lhs = a; n->ty = ty_ptr(&ty_void);
                return n;
            }
            if ((l == 4 || (l == 5 && nm[4] == 'f')) && (!memcmp(nm, "sqrt", 4) || !memcmp(nm, "fabs", 4))) {
                if (kcc_nosse) unsupported(t, "__builtin_sqrt/__builtin_fabs (-mno-sse build)");
                int isf = l == 5;
                expect("(");
                Node *a = rv(assign());
                expect(")");
                if (!ty_is_arith(a->ty)) kerr_tok(t, "invalid argument type '%s' to '%.*s'", ty_name(a->ty), t->len, t->s);
                Type *ft = isf ? &ty_float : &ty_double;
                Node *n = new_node(ND_FP1, t);
                n->lhs = new_cast(a, ft, t); n->op = nm[0] == 's' ? FN_SQRT : FN_ABS; n->ty = ft;
                if (n->op == FN_ABS && n->lhs->k == ND_FNUM) return new_fnum((uint64_t)n->lhs->val & ~(1ULL << (ft->size * 8 - 1)), ft, t);
                return n;
            }
            if (l == 14 && !memcmp(nm, "return_address", 14)) {
                expect("(");
                Node *a0 = rv(assign()); expect(")");
                if (a0->k != ND_NUM || a0->val != 0) kerr_tok(t, "__builtin_return_address supports only argument 0");
                Node *n = new_node(ND_RETADDR, t);
                n->ty = ty_ptr(&ty_void);
                return n;
            }
            if (l == 11 && !memcmp(nm, "unreachable", 11)) {
                expect("("); expect(")");
                Node *n = new_node(ND_UNREACH, t);
                n->ty = &ty_void;
                return n;
            }
            if ((l == 6 && (!memcmp(nm, "memcpy", 6) || !memcmp(nm, "memset", 6) || !memcmp(nm, "memcmp", 6))) || (l == 6 && !memcmp(nm, "strlen", 6))) {
                Obj *o = builtin_decl(kfmt("%.*s", l, nm), t);
                return var_node(o, t);
            }
            if (l >= 3 && !memcmp(nm, "va_", 3)) return va_builtin(t, nm + 3, l - 3);
            kerr_tok(t, "unsupported builtin '%.*s'", t->len, t->s);
        }
        if ((t->len > 9 && !memcmp(t->s, "__atomic_", 9)) || (t->len > 7 && !memcmp(t->s, "__sync_", 7))) {
            tok = tok->next;
            return atomic_builtin(t);
        }
        if ((t->len == 8 && !memcmp(t->s, "__func__", 8)) || (t->len == 12 && !memcmp(t->s, "__FUNCTION__", 12))) {
            if (!cur_fn) kerr_tok(t, "'__func__' used outside of a function");
            tok = tok->next;
            return var_node(new_str_obj(cur_fn->name, (int)strlen(cur_fn->name), t), t);
        }
        VS *v = find_vs(tok);
        if (!v) {
            if (tok->next->k == TK_PUNCT && tok_is(tok->next, "("))
                kerr_tok(t, "call to undeclared function '%.*s'; ISO C99 and later do not support implicit function declarations", t->len, t->s);
            kerr_tok(t, "use of undeclared identifier '%.*s'", t->len, t->s);
        }
        tok = tok->next;
        if (v->var && v->vla) return new_deref(var_node(v->var, t), t);      /* VLA: объект лежит по адресу из скрытого указателя */
        if (v->var) return var_node(v->var, t);
        if (v->ety) return new_num(v->eval, &ty_int, t);
        kerr_tok(t, "unexpected type name '%.*s': expected expression", t->len, t->s);
    }
    if (tok->k == TK_KW && tok_is(tok, "_Generic")) return generic_select();
    kerr_tok(tok, "expected expression before %s", tokdesc(tok));
}

static Node *postfix(void) {
    Node *n = primary();
    for (int nops = 0;; nops++) {
        const Token *t = tok;
        if (nops > 4000) kerr_tok(tok, "expression is too long");
        if (eat("(")) { n = funcall(n, t); continue; }
        if (eat("[")) {
            Node *i = expr();
            expect("]");
            n = new_deref(new_add(n, i, t), t);
            continue;
        }
        if (eat(".")) { Token *nm = expect_id(); n = new_member(n, nm, t); continue; }
        if (eat("->")) { Token *nm = expect_id(); n = new_member(new_deref(n, t), nm, t); continue; }
        if (eat("++")) { n = new_incdec(n, 1, 1, t); continue; }
        if (eat("--")) { n = new_incdec(n, -1, 1, t); continue; }
        return n;
    }
}

static Node *unary_(void) {
    const Token *t = tok;
    if (eat("+")) {
        Node *e = rv(cast_expr());
        if (!ty_is_arith(e->ty)) kerr_tok(t, "invalid argument type '%s' to unary expression", ty_name(e->ty));
        return new_cast(e, ty_promote(e->ty), t);
    }
    if (eat("-")) {
        Node *e = rv(cast_expr());
        if (!ty_is_arith(e->ty)) kerr_tok(t, "invalid argument type '%s' to unary expression", ty_name(e->ty));
        if (ty_is_float(e->ty)) {
            if (e->k == ND_FNUM) return new_fnum(sf_neg((uint64_t)e->val, e->ty->size), e->ty, t);
            Node *n = new_node(ND_NEG, t);
            n->lhs = e; n->ty = e->ty;
            return n;
        }
        Type *pt = ty_promote(e->ty);
        e = new_cast(e, pt, t);
        if (e->k == ND_NUM) return new_num(trunc_ty((int64_t)(0 - (uint64_t)e->val), pt), pt, t);
        Node *n = new_node(ND_NEG, t);
        n->lhs = e; n->ty = pt;
        return n;
    }
    if (eat("~")) {
        Node *e = rv(cast_expr());
        if (!ty_is_int(e->ty)) kerr_tok(t, "invalid argument type '%s' to unary expression", ty_name(e->ty));
        Type *pt = ty_promote(e->ty);
        e = new_cast(e, pt, t);
        if (e->k == ND_NUM) return new_num(trunc_ty(~e->val, pt), pt, t);
        Node *n = new_node(ND_BITNOT, t);
        n->lhs = e; n->ty = pt;
        return n;
    }
    if (eat("!")) {
        Node *e = to_cond(cast_expr(), t);
        if (e->k == ND_NUM) return new_num(e->val == 0, &ty_int, t);
        Node *n = new_node(ND_NOT, t);
        n->lhs = e; n->ty = &ty_int;
        return n;
    }
    if (P("&&")) unsupported(t, "address of label");
    if (eat("*")) return new_deref(cast_expr(), t);
    if (eat("&")) {
        if (P("&")) unsupported(t, "address of label");
        return new_addr(cast_expr(), t);
    }
    if (eat("++")) return new_incdec(unary(), 1, 0, t);
    if (eat("--")) return new_incdec(unary(), -1, 0, t);
    if (KW("sizeof") || KW("__alignof__")) {
        int is_sz = KW("sizeof");
        tok = tok->next;
        Type *ty;
        Node *pend = NULL;
        if (P("(") && is_typename(tok->next)) {
            tok = tok->next;
            ty = typename();
            expect(")");
            pend = vla_take();
        } else {
            Node *e = unary();
            if (e->k == ND_MEMBER && e->mem->bw >= 0) kerr_tok(t, "invalid application of 'sizeof' to bit-field");
            ty = e->ty;
        }
        if (ty->k == TY_FUNC || ty->k == TY_VOID) return vla_chain(pend, new_num(1, &ty_ulong, t), t);
        if (ty->vm) return vla_chain(pend, is_sz ? size_node(ty, t) : new_num(TAL(ty), &ty_ulong, t), t);
        if (!ty_unqual(ty)->complete || (ty->k == TY_ARRAY && ty->alen < 0))
            kerr_tok(t, "invalid application of '%s' to an incomplete type '%s'", is_sz ? "sizeof" : "alignof", ty_name(ty));
        return vla_chain(pend, new_num(is_sz ? TSZ(ty) : TAL(ty), &ty_ulong, t), t);
    }
    return postfix();
}

static Node *cast_expr(void) {
    if (P("(") && is_typename(tok->next)) {
        Token *save = tok;
        const Token *t = tok;
        tok = tok->next;
        Type *ty = typename();
        expect(")");
        if (P("{")) { tok = save; return unary(); }
        Node *pend = vla_take();
        return vla_chain(pend, new_cast(cast_expr(), ty, t), t);
    }
    return unary();
}

static int prec_of(const Token *t) {
    static const struct { const char *s; int p; } tab[] = {
        { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "==", 6 }, { "!=", 6 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "<<", 8 }, { ">>", 8 },
        { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 }, { 0, 0 } };
    if (t->k != TK_PUNCT) return 0;
    for (int i = 0; tab[i].s; i++) if (tok_is(t, tab[i].s)) return tab[i].p;
    return 0;
}

static Node *apply_bin(const Token *op, Node *l, Node *r) {
    const char *s = op->s; int n = op->len;
    #define IS(x) (n == (int)sizeof(x) - 1 && !memcmp(s, x, (size_t)n))
    if (IS("||")) return new_logic(ND_LOGOR, l, r, op);
    if (IS("&&")) return new_logic(ND_LOGAND, l, r, op);
    if (IS("|")) return arith(ND_OR, l, r, op);
    if (IS("^")) return arith(ND_XOR, l, r, op);
    if (IS("&")) return arith(ND_AND, l, r, op);
    if (IS("==")) return new_cmp(ND_EQ, l, r, op);
    if (IS("!=")) return new_cmp(ND_NE, l, r, op);
    if (IS("<")) return new_cmp(ND_LT, l, r, op);
    if (IS("<=")) return new_cmp(ND_LE, l, r, op);
    if (IS(">")) return new_cmp(ND_LT, r, l, op);
    if (IS(">=")) return new_cmp(ND_LE, r, l, op);
    if (IS("<<")) return new_shift(ND_SHL, l, r, op);
    if (IS(">>")) return new_shift(ND_SHR, l, r, op);
    if (IS("+")) return new_add(l, r, op);
    if (IS("-")) return new_sub(l, r, op);
    if (IS("*")) return arith(ND_MUL, l, r, op);
    if (IS("/")) return arith(ND_DIV, l, r, op);
    return arith(ND_MOD, l, r, op);
    #undef IS
}

static Node *binary(int minp) {
    Node *l = cast_expr();
    int nops = 0;
    for (;;) {
        int p = prec_of(tok);
        if (p && ++nops > 4000) kerr_tok(tok, "expression is too long");
        if (!p || p < minp) return l;
        const Token *op = tok;
        tok = tok->next;
        Node *r = binary(p + 1);
        l = apply_bin(op, l, r);
    }
}

static Node *make_cond(Node *c, Node *a, Node *b, const Token *t) {
    c = to_cond(c, t); a = rv(a); b = rv(b);
    Type *ct;
    if (a->ty->k == TY_VOID && b->ty->k == TY_VOID) ct = &ty_void;
    else if (ty_is_arith(a->ty) && ty_is_arith(b->ty)) {
        ct = ty_common(a->ty, b->ty);
        a = new_cast(a, ct, t); b = new_cast(b, ct, t);
    } else if (a->ty->k == TY_PTR && b->ty->k == TY_PTR) {
        ct = a->ty;
        if (a->ty->base->k == TY_VOID) ct = a->ty;
        else if (b->ty->base->k == TY_VOID) ct = b->ty;
        a = new_cast(a, ct, t); b = new_cast(b, ct, t);
    } else if (a->ty->k == TY_PTR && ty_is_int(b->ty) && b->k == ND_NUM && b->val == 0) { ct = a->ty; b = new_cast(b, ct, t); }
    else if (b->ty->k == TY_PTR && ty_is_int(a->ty) && a->k == ND_NUM && a->val == 0) { ct = b->ty; a = new_cast(a, ct, t); }
    else if (ty_is_agg(a->ty) && ty_is_agg(b->ty) && ty_compat(a->ty, b->ty)) ct = a->ty;     /* значение - адрес выбранного объекта */
    else if (ty_is_agg(a->ty) || ty_is_agg(b->ty)) kerr_tok(t, "incompatible operand types in conditional expression");
    else kerr_tok(t, "incompatible operand types ('%s' and '%s')", ty_name(a->ty), ty_name(b->ty));
    if (c->k == ND_NUM) return c->val ? a : b;
    Node *n = new_node(ND_COND, t);
    n->c = c; n->t = a; n->e = b; n->ty = ct;
    return n;
}

static Node *cond_expr_(void) {
    Node *c = binary(1);
    const Token *t = tok;
    if (!eat("?")) return c;
    if (P(":")) {                                   /* a ?: b - a вычисляется один раз */
        tok = tok->next;
        Node *b = cond_expr();
        c = rv(c); b = rv(b);
        Type *ct;
        if (ty_is_arith(c->ty) && ty_is_arith(b->ty)) ct = ty_common(c->ty, b->ty);
        else if (c->ty->k == TY_PTR && b->ty->k == TY_PTR) ct = c->ty->base->k == TY_VOID ? b->ty : c->ty;
        else if (c->ty->k == TY_PTR && ty_is_int(b->ty) && b->k == ND_NUM && b->val == 0) ct = c->ty;
        else if (b->ty->k == TY_PTR && ty_is_int(c->ty) && c->k == ND_NUM && c->val == 0) ct = b->ty;
        else kerr_tok(t, "incompatible operand types ('%s' and '%s') in '?:'", ty_name(c->ty), ty_name(b->ty));
        c = new_cast(c, ct, t); b = new_cast(b, ct, t);
        if (c->k == ND_NUM) return c->val ? c : b;
        if (c->k == ND_FNUM) return sf_cmp((uint64_t)c->val, 0, c->ty->size) != 0 ? c : b;
        Node *n = new_node(ND_COND, t);
        n->c = c; n->e = b; n->ty = ct;             /* t == NULL: значением служит само условие */
        return n;
    }
    Node *a = expr();
    expect(":");
    Node *b = cond_expr();
    return make_cond(c, a, b, t);
}

static Node *assign(void) {
    Node *n = cond_expr();
    const Token *t = tok;
    if (eat("=")) return new_assign(n, assign(), t);
    static const struct { const char *s; NKind k; } ops[] = {
        { "+=", ND_ADD }, { "-=", ND_SUB }, { "*=", ND_MUL }, { "/=", ND_DIV }, { "%=", ND_MOD }, { "&=", ND_AND },
        { "|=", ND_OR }, { "^=", ND_XOR }, { "<<=", ND_SHL }, { ">>=", ND_SHR }, { 0, 0 } };
    if (tok->k == TK_PUNCT)
        for (int i = 0; ops[i].s; i++)
            if (tok_is(tok, ops[i].s)) { tok = tok->next; return new_opassign(ops[i].k, n, assign(), t); }
    return n;
}

static Node *expr(void) {
    Node *n = assign();
    int nops = 0;
    while (P(",")) {
        const Token *t = tok;
        if (++nops > 4000) kerr_tok(tok, "expression is too long");
        tok = tok->next;
        Node *r = rv(assign());
        n = new_bin(ND_COMMA, n, r, r->ty, t);
    }
    return n;
}

/* ---------- инициализаторы ---------- */
static int count_mode;

static Init *new_init(Type *ty, int flex) {
    Init *in = ka_new(sizeof *in);
    in->ty = ty;
    if (ty->k == TY_ARRAY) {
        if (ty->alen < 0) { in->flex = flex; in->nch = 0; return in; }
        in->nch = (int)ty->alen;
        if (in->nch) in->ch = ka_new(sizeof(Init *) * (size_t)in->nch);
    } else if (ty_is_agg(ty)) {
        Type *u = ty_unqual(ty);
        if (!u->complete) kerr(NULL, 0, "variable has incomplete type '%s'", ty_name(ty));
        in->nch = u->nmem;
        if (in->nch) in->ch = ka_new(sizeof(Init *) * (size_t)in->nch);
    }
    return in;
}

static Type *child_type(Init *in, int i) {
    if (in->ty->k == TY_ARRAY) return in->ty->base;
    for (Member *m = ty_unqual(in->ty)->mem; m; m = m->next) if (m->idx == i) return m->ty;
    return &ty_int;
}
static Init *child(Init *in, int i) {
    if (count_mode && in->ty->k == TY_ARRAY) return new_init(in->ty->base, 0);
    if (!in->ch[i]) in->ch[i] = new_init(child_type(in, i), 0);
    return in->ch[i];
}

static void init2(Init *in);

static int at_end(void) {
    if (P("}")) return 1;
    return P(",") && tok->next->k == TK_PUNCT && tok_is(tok->next, "}");
}
static int has_member(Type *st, const char *name, int nl) {
    for (Member *m = ty_unqual(st)->mem; m; m = m->next) {
        if (m->name) { if ((int)strlen(m->name) == nl && !memcmp(m->name, name, (size_t)nl)) return 1; }
        else if (ty_is_agg(m->ty) && has_member(m->ty, name, nl)) return 1;
    }
    return 0;
}

static int desig(Init *in) {
    int idx;
    Init *ch;
    const Token *t = tok;
    if (eat("[")) {
        if (in->ty->k != TY_ARRAY) kerr_tok(t, "array designator used for non-array type '%s'", ty_name(in->ty));
        idx = (int)const_expr();
        if (P("...")) unsupported(tok, "array designator ranges");
        expect("]");
        if (idx < 0 || (!count_mode && idx >= in->nch)) kerr_tok(t, "array designator index %d exceeds array bounds", idx);
        ch = child(in, count_mode ? 0 : idx);
        if (count_mode && ch->ty != in->ty->base) ch = new_init(in->ty->base, 0);
    } else {
        Token *dot = tok;
        tok = tok->next;
        if (!ty_is_agg(in->ty)) kerr_tok(t, "field designator used for non-struct type '%s'", ty_name(in->ty));
        Token *nm = expect_id();
        Member *found = NULL;
        for (Member *m = ty_unqual(in->ty)->mem; m && !found; m = m->next) {
            if (m->name && (int)strlen(m->name) == nm->len && !memcmp(m->name, nm->s, (size_t)nm->len)) found = m;
            else if (!m->name && ty_is_agg(m->ty) && has_member(m->ty, nm->s, nm->len)) { found = m; tok = dot; }
        }
        if (!found) kerr_tok(nm, "field designator '%.*s' does not refer to any field in type '%s'", nm->len, nm->s, ty_name(in->ty));
        idx = found->idx;
        ch = child(in, idx);
        if (in->ty->k == TY_UNION) in->mem = found;
        if (found->name == NULL) { desig(ch); return idx; }
    }
    if (P("[") || P(".")) desig(ch);
    else { expect("="); init2(ch); }
    return idx;
}

static void init_string(Init *in) {
    const Token *t = tok;
    int n = t->slen;
    tok = tok->next;
    if (in->ty->alen < 0) {
        in->ty = ty_array(in->ty->base, n + 1);
        in->nch = n + 1;
        in->ch = ka_new(sizeof(Init *) * (size_t)in->nch);
    }
    if (n > in->nch) kerr_tok(t, "initializer-string for char array is too long");
    for (int i = 0; i < n; i++) {
        Init *c = child(in, i);
        c->expr = new_num((int8_t)t->str[i], &ty_int, t);
        c->expr = conv_assign(c->expr, in->ty->base, t, "initializing");
    }
}

static void arr_braced(Init *in) {
    int flex = in->ty->alen < 0;
    if (flex) {
        if (!in->flex) kerr_tok(tok, "excess elements in array initializer");
        Token *save = tok;
        int saved_mode = count_mode;
        count_mode = 1;
        Init tmp = *in;
        Type *dt = ty_new(TY_ARRAY, 0, in->ty->base->align);
        dt->base = in->ty->base; dt->alen = 0x3fffffff;
        tmp.ty = dt;
        tmp.nch = 0x3fffffff;
        int i = 0, mx = 0, first = 1;
        while (!at_end() && !P("}")) {
            if (!first) { expect(","); if (P("}")) break; }
            first = 0;
            if (P("[")) i = desig(&tmp);
            else { Init *c = new_init(in->ty->base, 0); init2(c); }
            i++;
            if (i > mx) mx = i;
        }
        count_mode = saved_mode;
        tok = save;
        in->ty = ty_array(in->ty->base, mx);
        in->nch = mx;
        if (mx) in->ch = ka_new(sizeof(Init *) * (size_t)mx);
    }
    int i = 0, first = 1;
    while (!P("}")) {
        if (!first) { expect(","); if (P("}")) break; }
        first = 0;
        if (P("[")) i = desig(in);
        else if (i >= in->nch) kerr_tok(tok, "excess elements in array initializer");
        else init2(child(in, i));
        i++;
    }
    expect("}");
}

static void unbraced(Init *in, int n, Member **ms) {
    for (int j = 0; j < n; j++) {
        if (j > 0) {
            if (!P(",")) break;
            Token *s = tok;
            tok = tok->next;
            if (P("}") || P("[") || P(".")) { tok = s; break; }
        }
        init2(child(in, ms ? ms[j]->idx : j));
    }
}

static Member *first_member(Type *u) {
    Member *m = u->mem;
    while (m && !m->name && m->bw >= 0) m = m->next;
    return m;
}
static Member *next_member(Member *m) {
    m = m->next;
    while (m && !m->name && m->bw >= 0) m = m->next;
    return m;
}

static void st_braced(Init *in) {
    Type *u = ty_unqual(in->ty);
    Member *m = first_member(u);
    int first = 1;
    while (!P("}")) {
        if (!first) { expect(","); if (P("}")) break; }
        first = 0;
        if (P("[") || P(".")) {
            int idx = desig(in);
            m = NULL;
            for (Member *x = u->mem; x; x = x->next) if (x->idx == idx) m = x;
            m = m ? next_member(m) : NULL;
            if (u->k == TY_UNION) m = NULL;
        } else if (!m) kerr_tok(tok, "excess elements in %s initializer", u->k == TY_UNION ? "union" : "struct");
        else {
            if (u->k == TY_UNION) in->mem = m;
            init2(child(in, m->idx));
            m = u->k == TY_UNION ? NULL : next_member(m);
        }
    }
    expect("}");
}

static void st_unbraced(Init *in) {
    Type *u = ty_unqual(in->ty);
    Member *ms[256];
    int n = 0;
    for (Member *m = first_member(u); m && n < 256; m = next_member(m)) {
        ms[n++] = m;
        if (u->k == TY_UNION) break;
    }
    if (u->k == TY_UNION && n) in->mem = ms[0];
    unbraced(in, n, ms);
}

static void init2_(Init *in) {
    Type *ty = in->ty;
    if (ty->k == TY_ARRAY && tok->k == TK_STR && ty->base->size == 1 && ty_is_int(ty->base)) { init_string(in); return; }
    if (ty->k == TY_ARRAY) {
        if (eat("{")) { arr_braced(in); return; }
        if (in->flex || ty->alen < 0) kerr_tok(tok, "array initializer must be an initializer list");
        unbraced(in, in->nch, NULL);
        return;
    }
    if (ty_is_agg(ty)) {
        if (eat("{")) { st_braced(in); return; }
        if (!P(".") && !P("[")) {
            Token *s = tok;
            Node *e = rv(assign());
            if (ty_is_agg(e->ty) && ty_unqual(e->ty) == ty_unqual(ty)) { in->expr = e; return; }
            tok = s;
        }
        st_unbraced(in);
        return;
    }
    if (eat("{")) {
        init2(in);
        if (P(",")) tok = tok->next;
        if (!P("}")) kerr_tok(tok, "excess elements in scalar initializer");
        tok = tok->next;
        return;
    }
    in->expr = assign();
}

static Init *initializer(Type *ty, Type **newty) {
    Init *in = new_init(ty, 1);
    if (ty->k == TY_ARRAY && !P("{") && tok->k != TK_STR) kerr_tok(tok, "array initializer must be an initializer list");
    init2(in);
    *newty = in->ty;
    return in;
}

static Node *init_member(Node *lv, Member *m, const Token *t) {
    Node *n = new_node(ND_MEMBER, t);
    n->lhs = lv; n->mem = m;
    n->ty = (m->bw >= 0 && m->bw < 32 && m->ty->size <= 4) ? &ty_int : m->ty;
    return n;
}

static void init_emit(Init *in, Node *lv, Node **tail, const Token *t) {
    Type *ty = in->ty;
    if (ty->k == TY_ARRAY) {
        for (int i = 0; i < in->nch; i++)
            if (in->ch[i]) init_emit(in->ch[i], new_deref(new_add(lv, new_num(i, &ty_int, t), t), t), tail, t);
        return;
    }
    if (ty_is_agg(ty)) {
        if (in->expr) {
            Node *a = new_bin(ND_ASSIGN, lv, conv_assign(in->expr, ty, t, "initializing"), ty, t);
            Node *s = new_node(ND_EXPR, t); s->lhs = a;
            (*tail)->next = s; *tail = s;
            return;
        }
        Type *u = ty_unqual(ty);
        for (Member *m = u->mem; m; m = m->next) {
            if (u->k == TY_UNION && in->mem != m) continue;
            if (in->ch[m->idx]) init_emit(in->ch[m->idx], init_member(lv, m, t), tail, t);
        }
        return;
    }
    if (!in->expr) return;
    Node *a = new_bin(ND_ASSIGN, lv, conv_assign(in->expr, lv->ty->is_const ? ty_unqual(lv->ty) : lv->ty, t, "initializing"), lv->ty, t);
    Node *s = new_node(ND_EXPR, t); s->lhs = a;
    (*tail)->next = s; *tail = s;
}

static Node *init_block(Init *in, Node *lv, const Token *t) {
    Node *blk = new_node(ND_BLOCK, t);
    Node head = {0}, *tail = &head;
    Node *z = new_node(ND_ZERO, t);
    z->lhs = lv; z->ty = &ty_void;
    Node *zs = new_node(ND_EXPR, t); zs->lhs = z;
    tail->next = zs; tail = zs;
    init_emit(in, lv, &tail, t);
    blk->body = head.next;
    return blk;
}

/* ---------- статические данные ---------- */
static int64_t eval_addr(Node *n, const char **sym, const Token *t);

static int64_t addr_of(Node *n, const char **sym, const Token *t) {
    switch (n->k) {
    case ND_VAR:
        if (n->var->is_local) kerr_tok(t, "initializer element is not a compile-time constant");
        *sym = n->var->name;
        return 0;
    case ND_MEMBER:
        if (n->mem->bw >= 0) kerr_tok(t, "initializer element is not a compile-time constant");
        return addr_of(n->lhs, sym, t) + n->mem->off;
    case ND_DEREF:
        return eval_addr(n->lhs, sym, t);
    default:
        kerr_tok(t, "initializer element is not a compile-time constant");
    }
}

static int64_t eval_addr(Node *n, const char **sym, const Token *t) {
    *sym = NULL;
    switch (n->k) {
    case ND_NUM: return n->val;
    case ND_CAST: {
        int64_t v = eval_addr(n->lhs, sym, t);
        if (*sym) {
            if (n->ty->size != 8) kerr_tok(t, "initializer element is not a compile-time constant");
            return v;
        }
        return trunc_ty(v, n->ty);
    }
    case ND_ADDR: return addr_of(n->lhs, sym, t);
    case ND_ADD: case ND_SUB: {
        const char *s1, *s2;
        int64_t a = eval_addr(n->lhs, &s1, t), b = eval_addr(n->rhs, &s2, t);
        if (s2 && n->k == ND_SUB) kerr_tok(t, "initializer element is not a compile-time constant");
        if (s1 && s2) kerr_tok(t, "initializer element is not a compile-time constant");
        *sym = s1 ? s1 : s2;
        if (*sym && n->ty->size != 8) kerr_tok(t, "initializer element is not a compile-time constant");
        return n->k == ND_ADD ? (int64_t)((uint64_t)a + (uint64_t)b) : (int64_t)((uint64_t)a - (uint64_t)b);
    }
    default:
        kerr_tok(t, "initializer element is not a compile-time constant");
    }
}

static void put_le(unsigned char *p, int64_t v, int size) {
    for (int i = 0; i < size; i++) p[i] = (unsigned char)((uint64_t)v >> (8 * i));
}

static void write_data(Init *in, Type *ty, unsigned char *buf, int off, Obj *o, const Token *t) {
    if (ty->k == TY_ARRAY) {
        for (int i = 0; i < in->nch; i++)
            if (in->ch[i]) write_data(in->ch[i], ty->base, buf, off + i * TSZ(ty->base), o, t);
        return;
    }
    if (ty_is_agg(ty)) {
        if (in->expr) kerr_tok(t, "initializer element is not a compile-time constant");
        Type *u = ty_unqual(ty);
        for (Member *m = u->mem; m; m = m->next) {
            if (u->k == TY_UNION && in->mem != m) continue;
            Init *c = in->ch[m->idx];
            if (!c) continue;
            if (m->bw >= 0) {
                if (!c->expr) continue;
                Node *e = conv_assign(c->expr, m->ty, t, "initializing");
                const char *sym;
                int64_t v = eval_addr(e, &sym, t);
                if (sym) kerr_tok(t, "initializer element is not a compile-time constant");
                int sz = m->ty->size;
                uint64_t cur = 0;
                for (int i = 0; i < sz; i++) cur |= (uint64_t)buf[off + m->off + i] << (8 * i);
                uint64_t mask = (m->bw >= 64 ? ~0ULL : ((1ULL << m->bw) - 1)) << m->bo;
                cur = (cur & ~mask) | (((uint64_t)v << m->bo) & mask);
                put_le(buf + off + m->off, (int64_t)cur, sz);
            } else write_data(c, m->ty, buf, off + m->off, o, t);
        }
        return;
    }
    if (!in->expr) return;
    Node *e = conv_assign(in->expr, ty, t, "initializing");
    if (ty_is_float(ty)) {
        if (e->k != ND_FNUM) kerr_tok(t, "initializer element is not a compile-time constant");
        put_le(buf + off, e->val, ty->size);
        return;
    }
    const char *sym;
    int64_t v = eval_addr(e, &sym, t);
    if (sym) {
        if (ty->size != 8) kerr_tok(t, "initializer element is not a compile-time constant");
        Reloc *r = ka_new(sizeof *r);
        r->off = off; r->sym = sym; r->addend = v;
        r->next = o->relocs; o->relocs = r;
        return;
    }
    put_le(buf + off, v, ty->size);
}

/* ---------- встроенный ассемблер (минимальный набор шаблонов) ---------- */
typedef struct { const char *t; int nout, nin; const char *outs[4], *ins[4]; const char *clob; } AsmT;
static const AsmT asm_tab[] = {
    { "", 0, 0, {0}, {0}, "" }, { "mfence", 0, 0, {0}, {0}, "" }, { "lfence", 0, 0, {0}, {0}, "" }, { "sfence", 0, 0, {0}, {0}, "" },
    { "pause", 0, 0, {0}, {0}, "" }, { "hlt", 0, 0, {0}, {0}, "" }, { "cli", 0, 0, {0}, {0}, "" }, { "sti", 0, 0, {0}, {0}, "" },
    { "nop", 0, 0, {0}, {0}, "" }, { "rdtsc", 2, 0, { "=a", "=d" }, {0}, "" },
    { "outb %0, %1", 0, 2, {0}, { "a", "Nd" }, "" }, { "outw %0, %1", 0, 2, {0}, { "a", "Nd" }, "" }, { "outl %0, %1", 0, 2, {0}, { "a", "Nd" }, "" },
    { "inb %1, %0", 1, 1, { "=a" }, { "Nd" }, "" }, { "inw %1, %0", 1, 1, { "=a" }, { "Nd" }, "" }, { "inl %1, %0", 1, 1, { "=a" }, { "Nd" }, "" },
    { "syscall", 4, 1, { "=a", "+D", "+S", "+d" }, { "a" }, ",rcx,r11,r8,r9,r10," },
    { "dmb ish", 0, 0, {0}, {0}, "" }, { "dmb ishst", 0, 0, {0}, {0}, "" }, { "dmb ishld", 0, 0, {0}, {0}, "" },
    { "dmb sy", 0, 0, {0}, {0}, "" }, { "dmb st", 0, 0, {0}, {0}, "" }, { "dsb ish", 0, 0, {0}, {0}, "" }, { "dsb st", 0, 0, {0}, {0}, "" },
    { "dsb sy", 0, 0, {0}, {0}, "" }, { "sev", 0, 0, {0}, {0}, "" },
    { "mrs %0, cntvct_el0", 1, 0, { "=r" }, {0}, "" }, { "mrs %0, cntfrq_el0", 1, 0, { "=r" }, {0}, "" },
    { "svc #0", 3, 1, { "+x0", "+x1", "+x2" }, { "x8" }, "" }, { "isb", 0, 0, {0}, {0}, "" }, { "wfi", 0, 0, {0}, {0}, "" },
    { "wfe", 0, 0, {0}, {0}, "" }, { "yield", 0, 0, {0}, {0}, "" },
    { "cli; hlt", 0, 0, {0}, {0}, "" },
    { "sti; hlt", 0, 0, {0}, {0}, "" },
    { "wrmsr", 0, 3, {0}, { "c", "a", "d" }, "" },
    { "rdmsr", 2, 1, { "=a", "=d" }, { "c" }, "" },
    { "cpuid", 4, 2, { "=a", "=b", "=c", "=d" }, { "a", "c" }, "" },
    { "mov %0, %%cr0", 0, 1, {0}, { "r" }, "" },
    { "mov %0, %%cr3", 0, 1, {0}, { "r" }, "" },
    { "mov %0, %%cr4", 0, 1, {0}, { "r" }, "" },
    { "mov %%cr0, %0", 1, 0, { "=r" }, {0}, "" },
    { "mov %%cr2, %0", 1, 0, { "=r" }, {0}, "" },
    { "mov %%cr3, %0", 1, 0, { "=r" }, {0}, "" },
    { "mov %%cr4, %0", 1, 0, { "=r" }, {0}, "" },
    { "mov %%cr3, %0; mov %0, %%cr3", 1, 0, { "=r" }, {0}, "" },
    { "ltr %0", 0, 1, {0}, { "r" }, "" },
    { "invlpg (%0)", 0, 1, {0}, { "r" }, "" },
    { "clflush (%0)", 0, 1, {0}, { "r" }, "" },
    { "clts", 0, 0, {0}, {0}, "" },
    { "fninit", 0, 0, {0}, {0}, "" },
    { "ud2", 0, 0, {0}, {0}, "" },
    { "rep movsb", 3, 0, { "+D", "+S", "+c" }, {0}, "" },
    { "rep movsq", 3, 0, { "+D", "+S", "+c" }, {0}, "" },
    { "std rep movsb cld", 3, 0, { "+D", "+S", "+c" }, {0}, "" },
    { "rep stosb", 2, 1, { "+D", "+c" }, { "a" }, "" },
    { "rep stosq", 2, 1, { "+D", "+c" }, { "a" }, "" },
    { "rep insw", 2, 1, { "+D", "+c" }, { "d" }, "" },
    { "rep outsw", 2, 1, { "+S", "+c" }, { "d" }, "" },
    { "mov %%gs:0, %0", 1, 0, { "=r" }, {0}, "" },
    { "xor %%rsp, %%rsp ud2", 0, 0, {0}, {0}, "" },
    { "fxsave (%0)", 0, 1, {0}, { "r" }, "" },
    { "fxrstor (%0)", 0, 1, {0}, { "r" }, "" },
    { "pushfq cli popq %0", 1, 0, { "=r" }, {0}, "" },
    { "pushfq; pop %0", 1, 0, { "=r" }, {0}, "" },
    { "outb %%al, $0x80", 0, 1, {0}, { "a" }, "" },
    { "ldmxcsr %0", 0, 1, {0}, { "m" }, "" },
    { "lidt %0 int3", 0, 1, {0}, { "m" }, "" },
    { "rdrand %0; setc %1", 2, 0, { "=r", "=qm" }, {0}, "" },
    { "rdseed %0; setc %1", 2, 0, { "=r", "=qm" }, {0}, "" },
    { "lgdt %0 pushq $0x08 leaq 1f(%%rip), %%rax pushq %%rax lretq 1: mov $0x10, %%ax mov %%ax, %%ds mov %%ax, %%es mov %%ax, %%fs mov %%ax, %%gs mov %%ax, %%ss", 0, 1, {0}, { "m" }, ",rax," },
    { "int $18", 0, 0, {0}, {0}, "" },
    { "bsfl %1, %0", 1, 1, { "=a" }, { "a" }, "" },
    { "bsfq %1, %0", 1, 1, { "=a" }, { "a" }, "" },
    { "bsrl %1, %0", 1, 1, { "=a" }, { "a" }, "" },
    { 0, 0, 0, {0}, {0}, 0 } };

static char *asm_norm(const char *s, int n) {
    char *o = ka_new((size_t)n + 1);
    int k = 0, pend = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n && (s[i + 1] == 'b' || s[i + 1] == 'w' || s[i + 1] == 'k' || s[i + 1] == 'q') && s[i + 2] >= '0' && s[i + 2] <= '9') {
            if (pend && k) o[k++] = ' ';
            pend = 0; o[k++] = '%'; i++; continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') { pend = 1; continue; }
        if (pend && k) o[k++] = ' ';
        pend = 0;
        o[k++] = c;
    }
    o[k] = 0;
    return o;
}

static int str_is(const Token *t, const char *s) { return t->slen == (int)strlen(s) && !memcmp(t->str, s, (size_t)t->slen); }

/* asm [volatile] ("шаблон" : выходы : входы : клобберы). Список входов IR: сначала значения "+"-выходов, затем явные входы. */
static Node *asm_stmt(void) {
    const Token *t = tok;
    tok = tok->next;
    while (KW("volatile") || KW("inline")) tok = tok->next;
    if (KW("goto")) unsupported(tok, "asm goto");
    expect("(");
    if (tok->k != TK_STR) kerr_tok(tok, "expected string literal in 'asm'");
    const Token *st = tok;
    char *tmpl = asm_norm(tok->str, tok->slen);
    tok = tok->next;
    int idx = -1;
    for (int i = 0; asm_tab[i].t; i++) if (!strcmp(asm_tab[i].t, tmpl)) idx = i;
    if (idx < 0) kerr_tok(st, "unsupported inline asm template '%s'", tmpl);
    const AsmT *at = &asm_tab[idx];
    Node *n = new_node(ND_ASM, t);
    n->name = tmpl; n->ty = &ty_void;
    Node ohead = {0}, ihead = {0}, xhead = {0}, *oc = &ohead, *ic = &ihead, *xc = &xhead;
    int sect = 0;
    while (P(":")) {
        tok = tok->next;
        if (++sect > 3) kerr_tok(tok, "too many asm sections");
        int cnt = 0;
        while (tok->k == TK_STR) {
            const Token *ct = tok;
            tok = tok->next;
            if (sect == 3) {
                int ok = str_is(ct, "memory") || str_is(ct, "cc");
                if (!ok && ct->slen > 0 && ct->slen < 8) {
                    char pat[12];
                    pat[0] = ','; memcpy(pat + 1, ct->str, (size_t)ct->slen); pat[ct->slen + 1] = ','; pat[ct->slen + 2] = 0;
                    ok = strstr(at->clob, pat) != NULL;
                }
                if (!ok) kerr_tok(ct, "unsupported asm clobber '%.*s'", ct->slen, ct->str);
            } else {
                expect("(");
                Node *e = expr();
                expect(")");
                const char *want = sect == 1 ? (cnt < at->nout ? at->outs[cnt] : NULL) : (cnt < at->nin ? at->ins[cnt] : NULL);
                int ok = want && str_is(ct, want);
                if (!ok && want && sect == 2 && !strcmp(want, "Nd") && str_is(ct, "d")) ok = 1;
                if (!ok) kerr_tok(ct, "asm %s constraint '%.*s' does not fit template '%s'", sect == 1 ? "output" : "input", ct->slen, ct->str, tmpl);
                if (sect == 1) {
                    if (!is_lval(e)) kerr_tok(ct, "asm output operand must be an lvalue");
                    if (!ty_is_scalar(e->ty) || ty_is_float(e->ty)) kerr_tok(ct, "asm output operand must be an integer or pointer");
                    Node *cp = ka_new(sizeof *cp);
                    *cp = *e;
                    cp->next = NULL;
                    oc->next = e; oc = e; n->nao++;
                    if (ct->str[0] == '+') { Node *rd = rv(cp); Node *w = ka_new(sizeof *w); *w = *rd; w->next = NULL; xc->next = w; xc = w; n->nai++; }
                } else {
                    if (str_is(ct, "m")) e = new_addr(e, ct);
                    e = rv(e);
                    if (!ty_is_scalar(e->ty) || ty_is_float(e->ty)) kerr_tok(ct, "asm input operand must be an integer or pointer");
                    ic->next = e; ic = e; n->nai++;
                }
            }
            cnt++;
            if (!eat(",")) break;
        }
    }
    expect(")"); expect(";");
    int nplus = 0;
    for (int i = 0; i < at->nout; i++) if (at->outs[i][0] == '+') nplus++;
    if (n->nao != at->nout || n->nai != at->nin + nplus)
        kerr_tok(t, "asm template '%s' needs %d output and %d input operands", tmpl, at->nout, at->nin);
    n->ao = ohead.next;
    xc->next = ihead.next;
    n->ai = xhead.next;
    return n;
}

/* ---------- объявления ---------- */
static Obj *new_gvar(const Token *nm, Type *ty) {
    Obj *o = ka_new(sizeof *o);
    o->name = tname(nm); o->ty = ty; o->align = ty->align; o->line = nm->line;
    add_global(o);
    VS *v = add_vs(nm);
    v->var = o;
    return o;
}

static Obj *declare_global(Token *nm, Type *ty, const Attr *a, int has_init) {
    VS *prev = find_in(gscope, nm->s, nm->len);
    int is_fn = ty->k == TY_FUNC;
    Obj *o;
    if (prev && prev->var) {
        o = prev->var;
        if (!prev->var->is_func != !is_fn) kerr_tok(nm, "redefinition of '%.*s' as different kind of symbol", nm->len, nm->s);
        if (!ty_compat(o->ty, ty)) kerr_tok(nm, "conflicting types for '%.*s'", nm->len, nm->s);
        if (o->ty->k == TY_ARRAY && o->ty->alen < 0 && ty->alen >= 0) o->ty = ty;
        if (is_fn && o->ty->noproto && !ty->noproto) o->ty = ty;
        if (is_fn && o->ty->params && ty->params) {
            /* имена параметров берём из последней декларации */
        }
        if (a->is_static && !o->is_static && !a->is_extern) kerr_tok(nm, "static declaration of '%.*s' follows non-static declaration", nm->len, nm->s);
        if (has_init && o->is_def && !o->tentative) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
    } else if (prev) {
        kerr_tok(nm, "redefinition of '%.*s' as different kind of symbol", nm->len, nm->s);
    } else {
        o = new_gvar(nm, ty);
        o->is_static = (unsigned)a->is_static;
        o->is_func = (unsigned)is_fn;
    }
    if (a->is_inline) o->is_inline = 1;
    if (a->noret) o->noret = 1;
    if (a->weak) o->is_weak = 1;
    if (a->align) o->align = a->align;
    if (!is_fn) {
        if (has_init) { o->is_def = 1; o->tentative = 0; }
        else if (!a->is_extern && !o->is_def) { o->is_def = 1; o->tentative = 1; }
    }
    return o;
}

static void check_object_type(Type *ty, const Token *nm, int allow_incomplete_array) {
    if (ty->k == TY_VOID) kerr_tok(nm, "variable has incomplete type 'void'");
    if (ty->k == TY_ARRAY && ty->alen < 0 && allow_incomplete_array) return;
    if (!ty_unqual(ty)->complete && ty->k != TY_FUNC) kerr_tok(nm, "variable has incomplete type '%s'", ty_name(ty));
}

static void do_static_assert(void) {
    const Token *t = tok;
    tok = tok->next;
    expect("(");
    int64_t v = const_expr();
    if (eat(",")) { if (tok->k != TK_STR) kerr_tok(tok, "expected string literal in _Static_assert"); tok = tok->next; }
    expect(")"); expect(";");
    if (!v) kerr_tok(t, "static assertion failed");
}

static Node *local_decl(void) {
    Attr a = {0};
    Type *base = declspec(&a);
    Node head = {0}, *tail = &head;
    const Token *t = tok;
    if (eat(";")) return NULL;
    int first = 1;
    while (!eat(";")) {
        if (!first) expect(",");
        first = 0;
        Token *nm = NULL;
        Type *ty = declarator(base, &nm);
        attrs(&a);
        if (!nm) kerr_tok(tok, "expected identifier or '(' before %s", tokdesc(tok));
        for (Node *pn = vla_take(), *nx; pn; pn = nx) {      /* длины VLA из этого объявления вычисляются здесь, до самого объекта */
            nx = pn->next; pn->next = NULL;
            Node *es = new_expr_stmt(pn, nm);
            tail->next = es; tail = es;
        }
        if (a.is_typedef) {
            if (find_in(scope, nm->s, nm->len)) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
            VS *v = add_vs(nm);
            v->tdef = ty;
            continue;
        }
        if (ty->k == TY_FUNC) {
            Obj *o = declare_global(nm, ty, &a, 0);
            if (!find_in(scope, nm->s, nm->len)) { VS *v = add_vs(nm); v->var = o; }
            if (P("{")) kerr_tok(tok, "function definition is not allowed here");
            continue;
        }
        if (ty->vm && (a.is_extern || a.is_static)) kerr_tok(nm, "variable length array declaration cannot have static or external storage duration");
        if (a.is_extern) {
            Obj *o = declare_global(nm, ty, &a, 0);
            if (!find_in(scope, nm->s, nm->len)) { VS *v = add_vs(nm); v->var = o; }
            continue;
        }
        if (a.is_static) {
            check_object_type(ty, nm, 1);
            Obj *o = ka_new(sizeof *o);
            o->name = kfmt("%s.%.*s.%d", cur_fn->name, nm->len, nm->s, static_count++);
            o->ty = ty; o->is_static = 1; o->is_def = 1; o->align = a.align ? a.align : ty->align; o->line = nm->line;
            if (find_in(scope, nm->s, nm->len)) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
            VS *v = add_vs(nm);
            v->var = o;
            if (eat("=")) {
                Type *nt;
                Init *in = initializer(ty, &nt);
                o->ty = nt;
                check_object_type(nt, nm, 0);
                o->init = ka_new((size_t)(nt->size ? nt->size : 1));
                write_data(in, nt, o->init, 0, o, nm);
            } else check_object_type(ty, nm, 0);
            add_global(o);
            continue;
        }
        if (ty->k == TY_ARRAY && ty->vm) {                   /* объект-VLA: блок стека, адрес в скрытом указателе */
            if (P("=")) kerr_tok(tok, "variable-sized object may not be initialized");
            if (blk_is_se) unsupported(nm, "variable length array directly in a statement expression");
            if (a.align) unsupported(nm, "alignment attribute on a variable length array");
            Obj *po = new_lvar(nm, ty_ptr(ty));
            find_in(scope, nm->s, nm->len)->vla = 1;
            if (vla_depth == blk_vla_base) {                 /* первое VLA блока: запомнить sp, конец блока освободит все */
                if (vla_depth >= 64) kerr_tok(nm, "too many nested scopes with variable length arrays");
                Obj *sv = hidden_lvar(&ty_long);
                Node *sn = new_node(ND_SPSAVE, nm);
                sn->ty = &ty_long;
                Node *es = new_expr_stmt(new_bin(ND_ASSIGN, var_node(sv, nm), sn, &ty_long, nm), nm);
                tail->next = es; tail = es;
                vla_stack[vla_depth++] = sv;
            }
            Node *an = new_node(ND_ALLOCA, nm);
            an->lhs = size_node(ty, nm); an->ty = po->ty;
            Node *es = new_expr_stmt(new_bin(ND_ASSIGN, var_node(po, nm), an, po->ty, nm), nm);
            tail->next = es; tail = es;
            continue;
        }
        check_object_type(ty, nm, 1);
        Obj *o = new_lvar(nm, ty);
        if (a.align) o->align = a.align;
        const Token *et = tok;
        if (eat("=")) {
            Type *nt;
            Init *in = initializer(ty, &nt);
            o->ty = nt; o->align = a.align ? a.align : nt->align;
            check_object_type(nt, nm, 0);
            Node *blk = init_block(in, var_node(o, nm), et);
            tail->next = blk; tail = blk;
        } else check_object_type(ty, nm, 0);
    }
    if (!head.next) return NULL;
    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
}

/* ---------- операторы ---------- */
static int new_lbl(void) { return ++cur_fn->nlabels; }

static Node *new_expr_stmt(Node *e, const Token *t) {
    Node *n = new_node(ND_EXPR, t);
    n->lhs = e;
    return n;
}

static Node *compound(int new_sc) {
    const Token *t = tok;
    expect("{");
    Node *blk = new_node(ND_BLOCK, t);
    Node head = {0}, *cur = &head;
    int vbase = blk_vla_base, vse = blk_is_se;
    blk_vla_base = vla_depth; blk_is_se = stmtexpr_next; stmtexpr_next = 0;
    if (new_sc) new_scope(0);
    while (!P("}")) {
        if (tok->k == TK_EOF) kerr_tok(t, "expected '}' at end of input");
        Node *s;
        if (KW("_Static_assert")) { do_static_assert(); continue; }
        if (is_typename(tok) && !(tok->k == TK_ID && tok->next->k == TK_PUNCT && tok_is(tok->next, ":"))) s = local_decl();
        else s = stmt();
        if (vla_head) unsupported(t, "variably modified type in this context");
        if (!s) continue;
        cur->next = s;
        while (cur->next) cur = cur->next;
    }
    const Token *et = tok;
    expect("}");
    if (vla_depth > blk_vla_base) {                          /* конец блока с VLA: стек возвращается к состоянию до первого из них */
        if (new_sc) cur->next = new_expr_stmt(sp_restore(vla_stack[blk_vla_base], et), et);
        vla_depth = blk_vla_base;
    }
    blk_vla_base = vbase; blk_is_se = vse;
    if (new_sc) leave_scope();
    blk->body = head.next;
    return blk;
}

static Node *stmt_(void) {
    const Token *t = tok;
    if (P("{")) return compound(1);
    if (eat(";")) return new_node(ND_BLOCK, t);
    if (KW("asm")) return asm_stmt();
    if (KW("return")) {
        tok = tok->next;
        Node *n = new_node(ND_RETURN, t);
        Type *rt = cur_fn->ty->base;
        if (eat(";")) {
            if (rt->k != TY_VOID) kerr_tok(t, "non-void function '%s' should return a value", cur_fn->name);
            return n;
        }
        const Token *et = tok;
        Node *e = expr();
        expect(";");
        if (rt->k == TY_VOID) {
            if (rv(e)->ty->k != TY_VOID) kerr_tok(et, "void function '%s' should not return a value", cur_fn->name);
            n->lhs = new_cast(e, &ty_void, et);
            return n;
        }
        n->lhs = conv_assign(e, rt, et, "returning");
        return n;
    }
    if (KW("if")) {
        tok = tok->next;
        expect("(");
        Node *n = new_node(ND_IF, t);
        n->c = to_cond(expr(), t);
        expect(")");
        n->t = stmt();
        if (eat_kw("else")) n->e = stmt();
        return n;
    }
    if (KW("switch")) {
        tok = tok->next;
        expect("(");
        Node *n = new_node(ND_SWITCH, t);
        Node *c = rv(expr());
        if (!ty_is_int(c->ty)) kerr_tok(t, "statement requires expression of integer type ('%s' invalid)", ty_name(c->ty));
        n->c = new_cast(c, ty_promote(c->ty), t);
        expect(")");
        Node *sv = cur_switch; int sb = brk_lbl, sbv = brk_vd;
        cur_switch = n; n->lbl = brk_lbl = new_lbl(); brk_vd = vla_depth;
        n->body = stmt();
        cur_switch = sv; brk_lbl = sb; brk_vd = sbv;
        return n;
    }
    if (KW("case")) {
        tok = tok->next;
        if (!cur_switch) kerr_tok(t, "'case' statement not in switch statement");
        int64_t v = const_expr();
        int64_t hi = v;
        int range = 0;
        if (eat("...")) { hi = const_expr(); range = 1; }
        expect(":");
        v = trunc_ty(v, cur_switch->c->ty);
        hi = trunc_ty(hi, cur_switch->c->ty);
        if (range && (cur_switch->c->ty->is_unsigned ? (uint64_t)hi < (uint64_t)v : hi < v)) kerr_tok(t, "empty case range specified");
        for (Node *c = cur_switch->cases; c; c = c->cases) {
            int64_t clo = c->val, chi = c->op == 2 ? c->val2 : c->val;
            int ov = cur_switch->c->ty->is_unsigned ? ((uint64_t)v <= (uint64_t)chi && (uint64_t)clo <= (uint64_t)hi) : (v <= chi && clo <= hi);
            if (ov) kerr_tok(t, range || c->op == 2 ? "duplicate case value in range" : "duplicate case value '%lld'", (long long)v);
        }
        Node *n = new_node(ND_CASE, t);
        n->val = v; n->val2 = hi; n->op = range ? 2 : 0; n->lbl = new_lbl();
        n->cases = cur_switch->cases; cur_switch->cases = n;
        n->lhs = stmt();
        return n;
    }
    if (KW("default")) {
        tok = tok->next;
        if (!cur_switch) kerr_tok(t, "'default' statement not in switch statement");
        expect(":");
        if (cur_switch->dflt) kerr_tok(t, "multiple default labels in one switch");
        Node *n = new_node(ND_CASE, t);
        n->lbl = new_lbl(); n->op = 1;
        cur_switch->dflt = n;
        n->lhs = stmt();
        return n;
    }
    if (KW("for")) {
        tok = tok->next;
        expect("(");
        Node *n = new_node(ND_FOR, t);
        new_scope(0);
        if (is_typename(tok)) n->ini = local_decl();
        else if (!eat(";")) { n->ini = new_expr_stmt(expr(), t); expect(";"); }
        if (!P(";")) n->c = to_cond(expr(), t);
        expect(";");
        if (!P(")")) n->inc = new_expr_stmt(expr(), t);
        expect(")");
        int sb = brk_lbl, sc = cont_lbl, sbv = brk_vd, scv = cont_vd;
        brk_vd = cont_vd = vla_depth;
        n->lbl = brk_lbl = new_lbl(); n->lbl2 = cont_lbl = new_lbl();
        n->body = stmt();
        brk_lbl = sb; cont_lbl = sc; brk_vd = sbv; cont_vd = scv;
        leave_scope();
        return n;
    }
    if (KW("while")) {
        tok = tok->next;
        expect("(");
        Node *n = new_node(ND_FOR, t);
        n->c = to_cond(expr(), t);
        expect(")");
        int sb = brk_lbl, sc = cont_lbl, sbv = brk_vd, scv = cont_vd;
        brk_vd = cont_vd = vla_depth;
        n->lbl = brk_lbl = new_lbl(); n->lbl2 = cont_lbl = new_lbl();
        n->body = stmt();
        brk_lbl = sb; cont_lbl = sc; brk_vd = sbv; cont_vd = scv;
        return n;
    }
    if (KW("do")) {
        tok = tok->next;
        Node *n = new_node(ND_DO, t);
        int sb = brk_lbl, sc = cont_lbl, sbv = brk_vd, scv = cont_vd;
        brk_vd = cont_vd = vla_depth;
        n->lbl = brk_lbl = new_lbl(); n->lbl2 = cont_lbl = new_lbl();
        n->body = stmt();
        brk_lbl = sb; cont_lbl = sc; brk_vd = sbv; cont_vd = scv;
        if (!eat_kw("while")) kerr_tok(tok, "expected 'while' in do/while statement");
        expect("(");
        n->c = to_cond(expr(), t);
        expect(")"); expect(";");
        return n;
    }
    if (KW("break")) {
        tok = tok->next; expect(";");
        if (!brk_lbl) kerr_tok(t, "'break' statement not in loop or switch statement");
        Node *n = new_node(ND_BREAK, t);
        n->lbl = brk_lbl;
        n->lhs = vla_leave(brk_vd, t);
        return n;
    }
    if (KW("continue")) {
        tok = tok->next; expect(";");
        if (!cont_lbl) kerr_tok(t, "'continue' statement not in loop statement");
        Node *n = new_node(ND_CONTINUE, t);
        n->lbl = cont_lbl;
        n->lhs = vla_leave(cont_vd, t);
        return n;
    }
    if (KW("goto")) {
        tok = tok->next;
        if (P("*")) unsupported(tok, "computed goto");
        Token *nm = expect_id();
        expect(";");
        Node *n = new_node(ND_GOTO, t);
        n->name = tname(nm);
        n->val = vla_depth; n->args = vla_snapshot(t);        /* какие области VLA активны: при переходе из них sp восстанавливается (func_def) */
        n->gnext = fn_gotos; fn_gotos = n;
        return n;
    }
    if (tok->k == TK_ID && tok->next->k == TK_PUNCT && tok_is(tok->next, ":")) {
        Token *nm = tok;
        tok = tok->next->next;
        for (Node *l = fn_labels; l; l = l->gnext) if (!strcmp(l->name, tname(nm))) kerr_tok(nm, "redefinition of label '%.*s'", nm->len, nm->s);
        Node *n = new_node(ND_LABEL, t);
        n->name = tname(nm); n->lbl = new_lbl();
        n->val = vla_depth; n->args = vla_snapshot(t);
        n->gnext = fn_labels; fn_labels = n;
        if (P("}")) { n->lhs = new_node(ND_BLOCK, t); return n; }     /* метка перед '}' (расширение C2x, как в clang) */
        n->lhs = stmt();
        return n;
    }
    Node *n = new_expr_stmt(expr(), t);
    expect(";");
    return n;
}

/* ---------- верхний уровень ---------- */
static void func_def(Token *nm, Type *ty, Attr *a) {
    Obj *fn = declare_global(nm, ty, a, 0);
    if (fn->body) kerr_tok(nm, "redefinition of '%.*s'", nm->len, nm->s);
    fn->is_def = 1; fn->ty = ty;
    cur_fn = fn; fn_locals_tail = NULL; nlocals = 0;
    fn_gotos = NULL; fn_labels = NULL; cur_switch = NULL; brk_lbl = cont_lbl = 0;
    fn->locals = NULL; fn->nlabels = 0;
    new_scope(0);
    for (Param *p = ty->params; p; p = p->next) {
        if (!p->name) kerr_tok(nm, "parameter name omitted");
        Token pt = *nm;
        pt.s = p->name; pt.len = (int)strlen(p->name);
        Obj *o;
        Obj **pp = &ty->vobjs;
        while (*pp && !((*pp)->is_param && (*pp)->name == p->name)) pp = &(*pp)->next;
        if (*pp) {                                           /* объект создан при разборе списка параметров (см. func_params_) */
            o = *pp; *pp = o->next; o->next = NULL;
            o->slot = nlocals++;
            if (fn_locals_tail) fn_locals_tail->next = o; else cur_fn->locals = o;
            fn_locals_tail = o;
            VS *v = add_vs(&pt);
            v->var = o;
        } else o = new_lvar(&pt, p->ty);
        o->is_param = 1;
        fn->params = fn->params ? fn->params : o;
        fn->nparams++;
    }
    for (Obj *o = ty->vobjs, *nx; o; o = nx) {              /* скрытые длины VLA из типов параметров */
        nx = o->next;
        o->next = NULL; o->slot = nlocals++;
        if (fn_locals_tail) fn_locals_tail->next = o; else cur_fn->locals = o;
        fn_locals_tail = o;
    }
    vla_depth = 0; blk_vla_base = 0; blk_is_se = 0; vla_head = vla_tail = NULL;
    fn->body = compound(0);
    if (ty->vinit) {                                         /* длины VLA параметров вычисляются при входе, до тела */
        Node *vh = NULL, *vt = NULL;
        for (Node *pn = ty->vinit, *nx; pn; pn = nx) {
            nx = pn->next; pn->next = NULL;
            Node *es = new_expr_stmt(pn, pn->tok);
            if (vt) vt->next = es; else vh = es;
            vt = es;
        }
        vt->next = fn->body->body;
        fn->body->body = vh;
    }
    for (Node *g = fn_gotos; g; g = g->gnext) {
        Node *l = fn_labels;
        while (l && strcmp(l->name, g->name)) l = l->gnext;
        if (!l) kerr_tok(g->tok, "use of undeclared label '%s'", g->name);
        g->lbl = l->lbl;
        if (l->val > g->val) kerr_tok(g->tok, "cannot jump to label '%s': it is in the scope of a variable length array", g->name);
        Node *gv = g->args, *lv = l->args;
        for (int i = 0; i < l->val; i++, gv = gv->next, lv = lv->next)
            if (gv->var != lv->var) kerr_tok(g->tok, "cannot jump to label '%s': it is in the scope of a variable length array", g->name);
        if (g->val > l->val) {                              /* выход из областей VLA: освободить их */
            Node *f = g->args;
            for (int i = 0; i < l->val; i++) f = f->next;
            g->lhs = sp_restore(f->var, g->tok);
        }
    }
    leave_scope();
    cur_fn = NULL;
}

Obj *parse_program(Token *t) {
    tok = t;
    globals_head = globals_tail = NULL; va_ty = va_st = NULL;
    cur_fn = NULL; scope = NULL; str_count = static_count = type_count = 0;
    gscope = new_scope(1);
    while (tok->k != TK_EOF) {
        if (eat(";")) continue;
        if (KW("_Static_assert")) { do_static_assert(); continue; }
        if (KW("asm")) unsupported(tok, "file-scope asm");
        Attr a = {0};
        Type *base = declspec(&a);
        if (eat(";")) continue;
        int first = 1;
        while (!eat(";")) {
            if (!first) expect(",");
            first = 0;
            Token *nm = NULL;
            Type *ty = declarator(base, &nm);
            attrs(&a);
            if (!nm) kerr_tok(tok, "expected identifier or '(' before %s", tokdesc(tok));
            if (a.is_typedef) {
                VS *prev = find_in(gscope, nm->s, nm->len);
                if (prev && !(prev->tdef && ty_compat(prev->tdef, ty))) kerr_tok(nm, "redefinition of '%.*s' as different kind of symbol", nm->len, nm->s);
                if (!prev) { VS *v = add_vs(nm); v->tdef = ty; }
                continue;
            }
            if (ty->k == TY_FUNC) {
                if (P("{")) {
                    func_def(nm, ty, &a);
                    goto next_decl;
                }
                declare_global(nm, ty, &a, 0);
                continue;
            }
            check_object_type(ty, nm, 1);
            if (P("=")) {
                tok = tok->next;
                Obj *o = declare_global(nm, ty, &a, 1);
                Type *nt;
                Init *in = initializer(ty, &nt);
                o->ty = nt;
                check_object_type(nt, nm, 0);
                if (o->align < nt->align) o->align = nt->align;
                o->init = ka_new((size_t)(nt->size ? nt->size : 1));
                o->relocs = NULL;
                write_data(in, nt, o->init, 0, o, nm);
                o->is_extern = 0;
            } else {
                Obj *o = declare_global(nm, ty, &a, 0);
                if (!a.is_extern && o->is_def) check_object_type(o->ty, nm, 0);
                o->is_extern = (unsigned)(a.is_extern && !o->is_def);
            }
        }
        continue;
    next_decl:;
    }
    return globals_head;
}
