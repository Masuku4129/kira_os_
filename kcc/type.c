/* kcc: типы C (LP64: int 4, long 8, указатель 8). */
#include "kcc.h"
#include <string.h>

Type ty_void = { .k = TY_VOID, .size = 1, .align = 1 };
Type ty_bool = { .k = TY_BOOL, .size = 1, .align = 1, .is_unsigned = 1, .complete = 1 };
Type ty_char = { .k = TY_CHAR, .size = 1, .align = 1, .complete = 1 };
Type ty_uchar = { .k = TY_CHAR, .size = 1, .align = 1, .is_unsigned = 1, .complete = 1 };
Type ty_short = { .k = TY_SHORT, .size = 2, .align = 2, .complete = 1 };
Type ty_ushort = { .k = TY_SHORT, .size = 2, .align = 2, .is_unsigned = 1, .complete = 1 };
Type ty_int = { .k = TY_INT, .size = 4, .align = 4, .complete = 1 };
Type ty_uint = { .k = TY_INT, .size = 4, .align = 4, .is_unsigned = 1, .complete = 1 };
Type ty_long = { .k = TY_LONG, .size = 8, .align = 8, .complete = 1 };
Type ty_ulong = { .k = TY_LONG, .size = 8, .align = 8, .is_unsigned = 1, .complete = 1 };
Type ty_llong = { .k = TY_LLONG, .size = 8, .align = 8, .complete = 1 };
Type ty_ullong = { .k = TY_LLONG, .size = 8, .align = 8, .is_unsigned = 1, .complete = 1 };

Type *ty_new(TyKind k, int size, int align) {
    Type *t = ka_new(sizeof *t);
    t->k = k; t->size = size; t->align = align;
    return t;
}

Type *ty_unqual(Type *t) { return t->orig ? t->orig : t; }

Type *ty_qual(Type *t, int c, int v) {
    if ((!c || t->is_const) && (!v || t->is_volatile)) return t;
    Type *n = ka_new(sizeof *n);
    *n = *t;
    n->orig = ty_unqual(t);
    n->is_const |= (unsigned)c; n->is_volatile |= (unsigned)v;
    return n;
}

Type *ty_ptr(Type *base) {
    Type *t = ty_new(TY_PTR, 8, 8);
    t->base = base; t->is_unsigned = 1; t->complete = 1;
    return t;
}

Type *ty_array(Type *base, int64_t len) {
    Type *t = ty_new(TY_ARRAY, 0, base->align);
    t->base = base; t->alen = len;
    if (len >= 0) {
        if (base->size && len > (int64_t)0x7fffffff / base->size) kerr(NULL, 0, "array is too large");
        t->size = (int)(base->size * len); t->complete = 1;
    }
    return t;
}

Type *ty_func(Type *ret, Param *params, int variadic, int noproto) {
    Type *t = ty_new(TY_FUNC, 1, 1);
    t->base = ret; t->params = params; t->variadic = (unsigned)variadic; t->noproto = (unsigned)noproto; t->complete = 1;
    return t;
}

int ty_is_int(const Type *t) {
    switch (t->k) {
    case TY_BOOL: case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG: case TY_ENUM: return 1;
    default: return 0;
    }
}
int ty_is_scalar(const Type *t) { return ty_is_int(t) || t->k == TY_PTR; }
int ty_is_agg(const Type *t) { return t->k == TY_STRUCT || t->k == TY_UNION; }
int ty_bits(const Type *t) { return t->size * 8; }

static Type *canon(const Type *t) {
    int u = t->is_unsigned;
    switch (t->k) {
    case TY_BOOL: return &ty_bool;
    case TY_CHAR: return u ? &ty_uchar : &ty_char;
    case TY_SHORT: return u ? &ty_ushort : &ty_short;
    case TY_INT: case TY_ENUM: return u ? &ty_uint : &ty_int;
    case TY_LONG: return u ? &ty_ulong : &ty_long;
    case TY_LLONG: return u ? &ty_ullong : &ty_llong;
    default: return (Type *)t;
    }
}

Type *ty_promote(Type *t) {
    t = canon(t);
    if (t->k == TY_BOOL || t->k == TY_CHAR || t->k == TY_SHORT) return &ty_int;
    return t;
}

static int rank(const Type *t) { return t->k == TY_LLONG ? 3 : t->k == TY_LONG ? 2 : 1; }

Type *ty_common(Type *a, Type *b) {
    a = ty_promote(a); b = ty_promote(b);
    if (a->size != b->size) return a->size > b->size ? a : b;
    if (a->is_unsigned == b->is_unsigned) return rank(a) >= rank(b) ? a : b;
    Type *u = a->is_unsigned ? a : b, *s = a->is_unsigned ? b : a;
    if (rank(u) >= rank(s)) return u;
    return s->k == TY_LONG ? &ty_ulong : s->k == TY_LLONG ? &ty_ullong : &ty_uint;
}

int ty_compat(const Type *a, const Type *b) {
    if (a == b) return 1;
    if (a->orig) a = a->orig;
    if (b->orig) b = b->orig;
    if (a == b) return 1;
    /* Enum types are distinct nominal types. Treating every enum as int here
       incorrectly made enum A* and enum B* compatible in pointer operations. */
    if (a->k != b->k) return 0;
    switch (a->k) {
    case TY_VOID: return 1;
    case TY_ENUM: case TY_STRUCT: case TY_UNION: return 0;
    case TY_PTR: return ty_compat(a->base, b->base);
    case TY_ARRAY: return ty_compat(a->base, b->base) && (a->alen < 0 || b->alen < 0 || a->alen == b->alen);
    case TY_FUNC: {
        if (!ty_compat(a->base, b->base)) return 0;
        if (a->noproto || b->noproto) return 1;
        if (a->variadic != b->variadic) return 0;
        Param *p = a->params, *q = b->params;
        for (; p && q; p = p->next, q = q->next) if (!ty_compat(p->ty, q->ty)) return 0;
        return !p && !q;
    }
    default:
        return a->is_unsigned == b->is_unsigned;
    }
}

const char *ty_name(const Type *t) {
    const char *q = t->is_const ? "const " : "";
    switch (t->k) {
    case TY_VOID: return kfmt("%svoid", q);
    case TY_BOOL: return kfmt("%s_Bool", q);
    case TY_CHAR: return kfmt("%s%schar", q, t->is_unsigned ? "unsigned " : "");
    case TY_SHORT: return kfmt("%s%sshort", q, t->is_unsigned ? "unsigned " : "");
    case TY_INT: return kfmt("%s%sint", q, t->is_unsigned ? "unsigned " : "");
    case TY_LONG: return kfmt("%s%slong", q, t->is_unsigned ? "unsigned " : "");
    case TY_LLONG: return kfmt("%s%slong long", q, t->is_unsigned ? "unsigned " : "");
    case TY_ENUM: return kfmt("%senum %s", q, ty_unqual((Type *)t)->tag ? ty_unqual((Type *)t)->tag : "<anonymous>");
    case TY_PTR: return kfmt("%s *", ty_name(t->base));
    case TY_ARRAY: return t->alen >= 0 ? kfmt("%s[%lld]", ty_name(t->base), (long long)t->alen) : kfmt("%s[]", ty_name(t->base));
    case TY_FUNC: return kfmt("%s (...)", ty_name(t->base));
    case TY_STRUCT: return kfmt("%sstruct %s", q, ty_unqual((Type *)t)->tag ? ty_unqual((Type *)t)->tag : "<anonymous>");
    case TY_UNION: return kfmt("%sunion %s", q, ty_unqual((Type *)t)->tag ? ty_unqual((Type *)t)->tag : "<anonymous>");
    }
    return "?";
}
