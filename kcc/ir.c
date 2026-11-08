/* kcc: линейный IR (виртуальные регистры, не SSA) и его текстовый вывод (kcc -S-ir).
 * Семантика: каждый vreg хранит 64 бита; значение целого типа размера <8 всегда в каноническом виде
 * (знаковое или беззнаковое расширение по типу). Все переменные лежат в стековых слотах (load/store). */
#include "kcc.h"
#include <string.h>

static Ins *ihead, *itail;
static int nreg, nlab;
static Obj *fn, *ltail;
static int maxslot;
static SB *out;

static Ins *emit(IrOp op) {
    Ins *i = ka_new(sizeof *i);
    i->op = op;
    if (itail) itail->next = i; else ihead = i;
    itail = i;
    return i;
}
static int newreg(void) { return ++nreg; }
static int newlab(void) { return ++nlab; }

static void E_label(int l) { emit(IR_LABEL)->imm = l; }
static void E_jmp(int l) { emit(IR_JMP)->imm = l; }
static int E_const(int64_t v) { Ins *i = emit(IR_CONST); i->dst = newreg(); i->imm = v; return i->dst; }
static int E_bin(int sub, int a, int b, int w, int sgn) {
    Ins *i = emit(IR_BIN);
    i->sub = sub; i->a = a; i->b = b; i->w = w; i->sgn = sgn; i->dst = newreg();
    return i->dst;
}
static int E_cmp(int sub, int a, int b, int sgn) {
    Ins *i = emit(IR_CMP);
    i->sub = sub; i->a = a; i->b = b; i->sgn = sgn; i->dst = newreg();
    return i->dst;
}
static int E_ext(int a, int w, int sgn) {
    Ins *i = emit(IR_EXT);
    i->a = a; i->w = w; i->sgn = sgn; i->dst = newreg();
    return i->dst;
}
static void E_br(IrOp op, int reg, int lab) { Ins *i = emit(op); i->a = reg; i->imm = lab; }
static void E_mov(int dst, int src) { Ins *i = emit(IR_MOV); i->dst = dst; i->a = src; }
static int E_load(int addr, int w, int sgn, int vol) {
    Ins *i = emit(IR_LOAD);
    i->a = addr; i->w = w; i->sgn = sgn; i->vol = vol; i->dst = newreg();
    return i->dst;
}
static void E_store(int addr, int val, int w, int vol) {
    Ins *i = emit(IR_STORE);
    i->a = addr; i->b = val; i->w = w; i->vol = vol;
}

/* безымянная локальная переменная функции (буфер агрегата) и её адрес */
static int new_temp(Type *ty) {
    Obj *o = ka_new(sizeof *o);
    Type *tt = ka_new(sizeof *tt);                  /* буфер дополнен до 16 байт: результат вызова пишется регистрами целиком */
    *tt = *ty; tt->size = (ty->size + 15) & ~15; tt->orig = NULL;
    ty = tt;
    o->name = ".tmp"; o->ty = ty; o->is_local = 1; o->slot = ++maxslot; o->align = ty->align; o->line = fn->line;
    if (ltail) ltail->next = o; else fn->locals = o;
    ltail = o;
    Ins *i = emit(IR_LADDR);
    i->dst = newreg(); i->imm = o->slot;
    return i->dst;
}

static int gen_expr(Node *n);
static int gen_addr(Node *n);
static void gen_vastart(int ap);
static int gen_vaarg(int ap, Type *t);
static int ir_gen_atomic(Node *n);
static void gen_stmt(Node *n);

static int add_const(int a, int64_t off) {
    if (!off) return a;
    return E_bin(ND_ADD, a, E_const(off), 8, 0);
}

/* агрегат по значению: ограничения ABI бэкендов */
static void agg_ok(const Type *t, const Token *tok) {
    Type *u = ty_unqual((Type *)t);
    if (u->size <= 0) kerr_tok(tok, "unsupported: zero-sized struct/union passed by value");
    if (u->align > 8) kerr_tok(tok, "unsupported: struct/union with alignment %d passed by value", u->align);
    if (u->packed) kerr_tok(tok, "unsupported: packed struct/union passed by value");
}

static int is_agg_like(const Type *t) { return t->k == TY_ARRAY || t->k == TY_STRUCT || t->k == TY_UNION || t->k == TY_FUNC; }

/* преобразование целого/указателя из типа from в тип to */
static int convert(int v, Type *from, Type *to) {
    if (to->k == TY_VOID) return v;
    if (to->k == TY_BOOL) {
        if (from->k == TY_BOOL) return v;
        return E_cmp(ND_NE, v, E_const(0), 1);
    }
    if (to->size == 8) return v;
    int ss = from->size, sg = !from->is_unsigned;
    int ts = to->size, tg = !to->is_unsigned;
    if (from->k == TY_PTR) { ss = 8; sg = 0; }
    if ((ss < ts && (!sg || tg)) || (ss == ts && sg == tg)) return v;
    return E_ext(v, ts, tg);
}

static int load_scalar(int addr, Type *t) {
    int sg = !t->is_unsigned && t->k != TY_PTR && t->k != TY_BOOL;
    return E_load(addr, t->size, sg, t->is_volatile);
}

static int bf_get(int unit, Member *m, int sgn) {
    int bw = m->bw, bo = m->bo;
    if (sgn) {
        int x = E_bin(ND_SHL, unit, E_const(64 - bo - bw), 8, 1);
        return E_bin(ND_SHR, x, E_const(64 - bw), 8, 1);
    }
    int x = E_bin(ND_SHR, unit, E_const(bo), 8, 0);
    return E_bin(ND_AND, x, E_const((int64_t)(bw >= 64 ? ~0ULL : (1ULL << bw) - 1)), 8, 0);
}

static int bf_signed(Member *m) { return !m->ty->is_unsigned && m->ty->k != TY_BOOL; }

static int bf_put(int addr, Member *m, int val) {   /* возвращает записанное значение (после усечения) */
    int bw = m->bw, bo = m->bo, sz = m->ty->size;
    uint64_t mask = (bw >= 64 ? ~0ULL : (1ULL << bw) - 1);
    int unit = E_load(addr, sz, 0, m->ty->is_volatile);
    int keep = E_bin(ND_AND, unit, E_const((int64_t)~(mask << bo)), 8, 0);
    /* порядок вычисления аргументов в C не задан: каждое выражение, порождающее IR, - отдельным оператором (одинаковый IR при любом компиляторе kcc) */
    int mk = E_const((int64_t)mask);
    int msk = E_bin(ND_AND, val, mk, 8, 0);
    int sh = E_const(bo);
    int ins = E_bin(ND_SHL, msk, sh, 8, 0);
    E_store(addr, E_bin(ND_OR, keep, ins, 8, 0), sz, m->ty->is_volatile);
    int x = E_bin(ND_AND, val, E_const((int64_t)mask), 8, 0);
    if (bf_signed(m)) {
        x = E_bin(ND_SHL, x, E_const(64 - bw), 8, 1);
        x = E_bin(ND_SHR, x, E_const(64 - bw), 8, 1);
    }
    return x;
}

static int gen_addr(Node *n) {
    switch (n->k) {
    case ND_VAR: {
        Ins *i = emit(n->var->is_local ? IR_LADDR : IR_GADDR);
        i->dst = newreg();
        if (n->var->is_local) i->imm = n->var->slot; else i->sym = n->var->name;
        return i->dst;
    }
    case ND_DEREF: return gen_expr(n->lhs);
    case ND_MEMBER: return add_const(gen_addr(n->lhs), n->mem->off);
    case ND_COMMA:
        gen_expr(n->lhs);
        return gen_addr(n->rhs);
    case ND_CALL:
        if (ty_is_agg(n->ty)) return gen_expr(n);
        kerr_tok(n->tok, "internal: cannot take address of a call result");
    case ND_ASSIGN:
        gen_expr(n);
        return gen_addr(n->lhs);
    default:
        kerr_tok(n->tok, "internal: cannot take address of expression (kind %d)", (int)n->k);
    }
}

static int gen_incdec_value(int cur, Type *t, Node *n, int delta_reg, int dir) {
    (void)n;
    int sub = dir > 0 ? ND_ADD : ND_SUB;
    if (t->k == TY_PTR) return E_bin(sub, cur, delta_reg, 8, 0);
    Type *pt = ty_promote(t);
    int r = E_bin(sub, cur, E_const(1), pt->size, !pt->is_unsigned);
    return convert(r, pt, t);
}

static int gen_expr(Node *n) {
    Type *t = n->ty;
    switch (n->k) {
    case ND_NUM: return E_const(n->val);
    case ND_VAR:
        if (is_agg_like(t)) return gen_addr(n);
        return load_scalar(gen_addr(n), t);
    case ND_MEMBER: {
        if (is_agg_like(t)) return gen_addr(n);
        int a = gen_addr(n);
        if (n->mem->bw >= 0) return bf_get(E_load(a, n->mem->ty->size, 0, n->mem->ty->is_volatile), n->mem, bf_signed(n->mem));
        return load_scalar(a, t);
    }
    case ND_DEREF:
        if (is_agg_like(t)) return gen_addr(n);
        return load_scalar(gen_addr(n), t);
    case ND_ADDR: return gen_addr(n->lhs);
    case ND_CAST: {
        if (t->k == TY_VOID) { gen_expr(n->lhs); return -1; }
        int v = gen_expr(n->lhs);
        return convert(v, n->lhs->ty, t);
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD: case ND_AND: case ND_OR: case ND_XOR: case ND_SHL: case ND_SHR: {
        int a = gen_expr(n->lhs), b = gen_expr(n->rhs);
        int w = t->k == TY_PTR ? 8 : t->size;
        return E_bin(n->k, a, b, w, !t->is_unsigned && t->k != TY_PTR);
    }
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
        int a = gen_expr(n->lhs), b = gen_expr(n->rhs);
        return E_cmp(n->k, a, b, !n->lhs->ty->is_unsigned && n->lhs->ty->k != TY_PTR);
    }
    case ND_NEG: { int a = gen_expr(n->lhs); Ins *i = emit(IR_NEG); i->a = a; i->w = t->size; i->sgn = !t->is_unsigned; i->dst = newreg(); return i->dst; }
    case ND_BITNOT: { int a = gen_expr(n->lhs); Ins *i = emit(IR_NOT); i->a = a; i->w = t->size; i->sgn = !t->is_unsigned; i->dst = newreg(); return i->dst; }
    case ND_NOT: { int a = gen_expr(n->lhs); int z = E_const(0); return E_cmp(ND_EQ, a, z, 1); }
    case ND_LOGAND: case ND_LOGOR: {
        int r = newreg(), l_end = newlab(), l_sc = newlab();
        int a = gen_expr(n->lhs);
        E_br(n->k == ND_LOGAND ? IR_BRZ : IR_BRNZ, a, l_sc);
        int b = gen_expr(n->rhs);
        E_mov(r, E_cmp(ND_NE, b, E_const(0), 1));
        E_jmp(l_end);
        E_label(l_sc);
        E_mov(r, E_const(n->k == ND_LOGOR));
        E_label(l_end);
        return r;
    }
    case ND_COND: {
        int l_else = newlab(), l_end = newlab();
        int r = t->k == TY_VOID ? -1 : newreg();
        int c = gen_expr(n->c);
        if (!n->t) {                                /* a ?: b */
            E_mov(r, c);
            E_br(IR_BRNZ, c, l_end);
            E_mov(r, gen_expr(n->e));
            E_label(l_end);
            return r;
        }
        E_br(IR_BRZ, c, l_else);
        int x = gen_expr(n->t);
        if (r > 0) E_mov(r, x);
        E_jmp(l_end);
        E_label(l_else);
        x = gen_expr(n->e);
        if (r > 0) E_mov(r, x);
        E_label(l_end);
        return r;
    }
    case ND_COMMA:
        if (n->op == 1) return is_agg_like(t) ? gen_addr(n) : load_scalar(gen_addr(n), t);
        gen_expr(n->lhs);
        return gen_expr(n->rhs);
    case ND_ASSIGN: {
        if (ty_is_agg(t)) {
            int s = gen_addr(n->rhs), d = gen_addr(n->lhs);
            Ins *i = emit(IR_COPY);
            i->a = d; i->b = s; i->imm = ty_unqual(t)->size;
            return d;
        }
        int v = gen_expr(n->rhs);
        int a = gen_addr(n->lhs);
        if (n->lhs->k == ND_MEMBER && n->lhs->mem->bw >= 0) return bf_put(a, n->lhs->mem, v);
        E_store(a, v, n->lhs->ty->size, n->lhs->ty->is_volatile);
        return v;
    }
    case ND_OPASSIGN: {
        Node *l = n->lhs;
        int a = gen_addr(l);
        int bf = l->k == ND_MEMBER && l->mem->bw >= 0;
        int cur = bf ? bf_get(E_load(a, l->mem->ty->size, 0, l->mem->ty->is_volatile), l->mem, bf_signed(l->mem)) : load_scalar(a, l->ty);
        Type *oty = n->oty;
        int b = gen_expr(n->rhs);
        int res;
        if (l->ty->k == TY_PTR) res = E_bin(n->op, cur, b, 8, 0);
        else {
            int c2 = convert(cur, l->ty, oty);
            int r2 = E_bin(n->op, c2, b, oty->size, !oty->is_unsigned);
            res = convert(r2, oty, l->ty);
        }
        if (bf) return bf_put(a, l->mem, res);
        E_store(a, res, l->ty->size, l->ty->is_volatile);
        return res;
    }
    case ND_INCDEC: {
        Node *l = n->lhs;
        int a = gen_addr(l);
        int bf = l->k == ND_MEMBER && l->mem->bw >= 0;
        int cur = bf ? bf_get(E_load(a, l->mem->ty->size, 0, l->mem->ty->is_volatile), l->mem, bf_signed(l->mem)) : load_scalar(a, l->ty);
        int step = n->rhs ? E_const(n->rhs->val) : 0;
        int nv = gen_incdec_value(cur, l->ty, n, step, (int)n->val);
        int old = cur;
        if (n->op) { int keep = newreg(); E_mov(keep, cur); old = keep; }
        int stored = nv;
        if (bf) stored = bf_put(a, l->mem, nv);
        else E_store(a, nv, l->ty->size, l->ty->is_volatile);
        return n->op ? old : stored;
    }
    case ND_CALL: {
        int nargs = 0;
        for (Node *x = n->args; x; x = x->next) nargs++;
        int *regs = ka_new(sizeof(int) * (size_t)(nargs + 1)), *asz = ka_new(sizeof(int) * (size_t)(nargs + 1));
        int k = 0;
        for (Node *x = n->args; x; x = x->next) {
            regs[k] = gen_expr(x);
            if (ty_is_agg(x->ty)) {
                agg_ok(x->ty, x->tok);
                int sz = ty_unqual(x->ty)->size;
                asz[k] = sz;
                if (sz > 16) {                      /* большой агрегат передаётся копией: адрес копии - аргумент */
                    int tmp = new_temp(ty_unqual(x->ty));
                    Ins *c = emit(IR_COPY);
                    c->a = tmp; c->b = regs[k]; c->imm = sz;
                    regs[k] = tmp;
                }
            }
            k++;
        }
        Node *f = n->lhs;
        Type *ft = f->ty->base;
        int direct = f->k == ND_ADDR && f->lhs->k == ND_VAR && f->lhs->var->is_func;
        int fr = direct ? 0 : gen_expr(f);
        int rt = 0;
        if (ty_is_agg(t)) rt = new_temp(ty_unqual(t));
        Ins *i = emit(IR_CALL);
        if (direct) i->sym = f->lhs->var->name; else i->a = fr;
        i->args = regs; i->nargs = nargs; i->asz = asz;
        i->nout = (int)(ft->variadic || ft->noproto);
        if (ty_is_agg(t)) {
            agg_ok(t, n->tok);
            i->rsz = ty_unqual(t)->size;
            i->rt = rt;
            i->dst = -1;
            return rt;
        }
        i->w = t->k == TY_VOID ? 0 : t->size;
        i->sgn = !t->is_unsigned && t->k != TY_PTR;
        i->dst = t->k == TY_VOID ? -1 : newreg();
        return i->dst;
    }
    case ND_STMTEXPR: {
        int r = -1;
        Node *last = NULL;
        for (Node *s = n->body->body; s; s = s->next) last = s;
        for (Node *s = n->body->body; s; s = s->next) {
            if (s == last && s->k == ND_EXPR) r = gen_expr(s->lhs);
            else gen_stmt(s);
        }
        return r;
    }
    case ND_ZERO: {
        int a = gen_addr(n->lhs);
        Ins *i = emit(IR_ZERO);
        i->a = a; i->imm = ty_unqual(n->lhs->ty)->size;
        return -1;
    }
    case ND_ASM: {
        int nout = n->nao, nin = n->nai;
        int *in = ka_new(sizeof(int) * (size_t)(nin + 1));
        int k = 0;
        for (Node *x = n->ai; x; x = x->next) in[k++] = gen_expr(x);
        Ins *i = emit(IR_ASM);
        i->sym = n->name; i->args = in; i->nargs = nin; i->nout = nout;
        i->line = n->tok ? n->tok->line : 0; i->file = n->tok ? n->tok->file : NULL;
        int *outs = ka_new(sizeof(int) * (size_t)(nout + 1));
        for (int j = 0; j < nout; j++) outs[j] = newreg();
        i->imm = (int64_t)(intptr_t)outs;
        k = 0;
        for (Node *x = n->ao; x; x = x->next) {
            int a = gen_addr(x);
            E_store(a, outs[k++], x->ty->size, x->ty->is_volatile);
        }
        return -1;
    }
    case ND_UNREACH: emit(IR_UNREACH); return -1;
    case ND_VASTART: gen_vastart(gen_expr(n->lhs)); return -1;
    case ND_VAARG: return gen_vaarg(gen_expr(n->lhs), t);
    case ND_ATOMIC: return ir_gen_atomic(n);
    default:
        kerr_tok(n->tok, "internal: unsupported expression kind %d in lowering", (int)n->k);
    }
}


/* ---------- va_list ---------- */
static int is_a64(void) { return K.cfg->target && !strcmp(K.cfg->target, "aarch64"); }
static int vainfo(int sub) { Ins *i = emit(IR_VAINFO); i->sub = sub; i->dst = newreg(); return i->dst; }
static void st_at(int ptr, int64_t off, int v, int w) { E_store(add_const(ptr, off), v, w, 0); }

static void gen_vastart(int ap) {
    int ngrn = vainfo(2);
    if (!is_a64()) {                                /* x86-64: gp_offset, fp_offset, overflow_arg_area, reg_save_area */
        st_at(ap, 0, E_bin(ND_MUL, ngrn, E_const(8), 8, 0), 4);
        st_at(ap, 4, E_const(176), 4);
        st_at(ap, 8, vainfo(1), 8);
        st_at(ap, 16, vainfo(0), 8);
    } else {                                        /* AAPCS64: stack, gr_top, vr_top, gr_offs, vr_offs */
        st_at(ap, 0, vainfo(1), 8);
        int vi = vainfo(0);
        int c64 = E_const(64);
        st_at(ap, 8, E_bin(ND_ADD, vi, c64, 8, 0), 8);
        st_at(ap, 16, E_const(0), 8);
        int c8 = E_const(8);
        int goff = E_bin(ND_MUL, ngrn, c8, 8, 0);
        int c64b = E_const(64);
        st_at(ap, 24, E_bin(ND_SUB, goff, c64b, 8, 1), 4);
        st_at(ap, 28, E_const(0), 4);
    }
}

static int gen_vaarg(int ap, Type *t) {
    int r = newreg(), l_stk = newlab(), l_end = newlab();
    int stk_off = is_a64() ? 0 : 8;
    if (!is_a64()) {
        int gp = E_load(ap, 4, 0, 0);
        E_br(IR_BRZ, E_cmp(ND_LT, gp, E_const(48), 0), l_stk);
        int base = E_load(add_const(ap, 16), 8, 0, 0);
        E_mov(r, E_bin(ND_ADD, base, gp, 8, 0));
        st_at(ap, 0, E_bin(ND_ADD, gp, E_const(8), 4, 0), 4);
    } else {
        int off = E_load(add_const(ap, 24), 4, 1, 0);
        E_br(IR_BRZ, E_cmp(ND_LT, off, E_const(0), 1), l_stk);
        int top = E_load(add_const(ap, 8), 8, 0, 0);
        E_mov(r, E_bin(ND_ADD, top, off, 8, 0));
        st_at(ap, 24, E_bin(ND_ADD, off, E_const(8), 4, 1), 4);
    }
    E_jmp(l_end);
    E_label(l_stk);
    int sp = E_load(add_const(ap, stk_off), 8, 0, 0);
    E_mov(r, sp);
    st_at(ap, stk_off, E_bin(ND_ADD, sp, E_const(8), 8, 0), 8);
    E_label(l_end);
    return load_scalar(r, t);
}

/* ---------- атомарные операции ---------- */
static int atomic_ins(int sub, int a, int b, int c, int w, int sgn, int want) {
    Ins *i = emit(IR_ATOMIC);
    i->sub = sub; i->a = a; i->b = b; i->c = c; i->w = w; i->sgn = sgn;
    i->dst = want ? newreg() : -1;
    return i->dst;
}

static int ir_gen_atomic(Node *n) {
    if (n->op == AT_FENCE) { atomic_ins(AT_FENCE, 0, 0, 0, 8, 0, 0); return -1; }
    Type *T = ty_unqual(n->lhs->ty->base);
    int w = T->size, sgn = !T->is_unsigned && T->k != TY_PTR && T->k != TY_BOOL;
    int p = gen_expr(n->lhs);
    switch (n->op) {
    case AT_LOAD: return atomic_ins(AT_LOAD, p, 0, 0, w, sgn, 1);
    case AT_STORE: atomic_ins(AT_STORE, p, gen_expr(n->rhs), 0, w, sgn, 0); return -1;
    case AT_XCHG: return atomic_ins(AT_XCHG, p, gen_expr(n->rhs), 0, w, sgn, 1);
    case AT_CAS: {
        int exp, d, ep = 0;
        if (n->val) { exp = gen_expr(n->rhs); d = gen_expr(n->c); }
        else { ep = gen_expr(n->rhs); exp = load_scalar(ep, T); d = gen_expr(n->c); }
        int old = atomic_ins(AT_CAS, p, exp, d, w, sgn, 1);
        int ok = E_cmp(ND_EQ, old, exp, 1);
        if (n->val == 1) return old;
        if (n->val == 2) return ok;
        int l_ok = newlab();
        E_br(IR_BRNZ, ok, l_ok);
        E_store(ep, old, w, 0);
        E_label(l_ok);
        return ok;
    }
    default: {
        int v = gen_expr(n->rhs);
        int old = atomic_ins(n->op, p, v, 0, w, sgn, 1);
        if (!n->val) return old;
        int bw = T->k == TY_PTR ? 8 : w;
        switch (n->op) {
        case AT_ADD: return E_bin(ND_ADD, old, v, bw, sgn);
        case AT_SUB: return E_bin(ND_SUB, old, v, bw, sgn);
        case AT_AND: return E_bin(ND_AND, old, v, bw, sgn);
        case AT_OR: return E_bin(ND_OR, old, v, bw, sgn);
        case AT_XOR: return E_bin(ND_XOR, old, v, bw, sgn);
        default: {
            int an = E_bin(ND_AND, old, v, bw, sgn);
            Ins *i = emit(IR_NOT);
            i->a = an; i->w = bw; i->sgn = sgn; i->dst = newreg();
            return i->dst;
        }
        }
    }
    }
}

static void gen_stmt(Node *n) {
    switch (n->k) {
    case ND_BLOCK: for (Node *s = n->body; s; s = s->next) gen_stmt(s); break;
    case ND_EXPR: gen_expr(n->lhs); break;
    case ND_ASM: gen_expr(n); break;
    case ND_IF: {
        int l_else = newlab(), l_end = newlab();
        int c = gen_expr(n->c);
        E_br(IR_BRZ, c, l_else);
        gen_stmt(n->t);
        if (n->e) { E_jmp(l_end); E_label(l_else); gen_stmt(n->e); E_label(l_end); }
        else E_label(l_else);
        break;
    }
    case ND_FOR: {
        int top = newlab();
        if (n->ini) gen_stmt(n->ini);
        E_label(top);
        if (n->c) { int c = gen_expr(n->c); E_br(IR_BRZ, c, n->lbl); }
        gen_stmt(n->body);
        E_label(n->lbl2);
        if (n->inc) gen_stmt(n->inc);
        E_jmp(top);
        E_label(n->lbl);
        break;
    }
    case ND_DO: {
        int top = newlab();
        E_label(top);
        gen_stmt(n->body);
        E_label(n->lbl2);
        int c = gen_expr(n->c);
        E_br(IR_BRNZ, c, top);
        E_label(n->lbl);
        break;
    }
    case ND_SWITCH: {
        int v = gen_expr(n->c);
        for (Node *c = n->cases; c; c = c->cases) {
            int t;
            if (c->op == 2) {                       /* lo...hi: (v - lo) <= (hi - lo) без знака */
                int d = E_bin(ND_SUB, v, E_const(c->val), 8, 0);
                t = E_cmp(ND_LE, d, E_const((int64_t)((uint64_t)c->val2 - (uint64_t)c->val)), 0);
            } else t = E_cmp(ND_EQ, v, E_const(c->val), 1);
            E_br(IR_BRNZ, t, c->lbl);
        }
        E_jmp(n->dflt ? n->dflt->lbl : n->lbl);
        gen_stmt(n->body);
        E_label(n->lbl);
        break;
    }
    case ND_CASE: E_label(n->lbl); gen_stmt(n->lhs); break;
    case ND_BREAK: case ND_CONTINUE: case ND_GOTO: E_jmp(n->lbl); break;
    case ND_LABEL: E_label(n->lbl); gen_stmt(n->lhs); break;
    case ND_RETURN: {
        int v = -1, w = 0;
        if (n->lhs) {
            v = gen_expr(n->lhs);
            if (n->lhs->ty->k != TY_VOID) w = ty_unqual(n->lhs->ty)->size; else v = -1;
        }
        Ins *i = emit(IR_RET);
        i->a = v; i->w = w;
        if (v > 0 && ty_is_agg(n->lhs->ty)) i->sub = 1;
        break;
    }
    default:
        gen_expr(n);
    }
}

/* ---------- печать ---------- */
static const char *opname(int k) {
    switch (k) {
    case ND_ADD: return "add"; case ND_SUB: return "sub"; case ND_MUL: return "mul"; case ND_DIV: return "div";
    case ND_MOD: return "mod"; case ND_AND: return "and"; case ND_OR: return "or"; case ND_XOR: return "xor";
    case ND_SHL: return "shl"; case ND_SHR: return "shr"; case ND_EQ: return "eq"; case ND_NE: return "ne";
    case ND_LT: return "lt"; case ND_LE: return "le"; default: return "?";
    }
}

static void print_ins(const Ins *i) {
    const char *sg = i->sgn ? "s" : "u";
    switch (i->op) {
    case IR_LABEL: sb_printf(out, ".L%lld:\n", (long long)i->imm); break;
    case IR_CONST: sb_printf(out, "  %%%d = const %lld\n", i->dst, (long long)i->imm); break;
    case IR_GADDR: sb_printf(out, "  %%%d = gaddr %s\n", i->dst, i->sym); break;
    case IR_LADDR: sb_printf(out, "  %%%d = laddr slot%lld\n", i->dst, (long long)i->imm); break;
    case IR_PARAM:
        if (i->sub) sb_printf(out, "  param.agg %lld size=%d into [%%%d]\n", (long long)i->imm, i->w, i->a);
        else sb_printf(out, "  %%%d = param %lld\n", i->dst, (long long)i->imm);
        break;
    case IR_LOAD: sb_printf(out, "  %%%d = load%s.%d%s [%%%d]\n", i->dst, i->vol ? ".v" : "", i->w, sg, i->a); break;
    case IR_STORE: sb_printf(out, "  store%s.%d [%%%d], %%%d\n", i->vol ? ".v" : "", i->w, i->a, i->b); break;
    case IR_MOV: sb_printf(out, "  %%%d = mov %%%d\n", i->dst, i->a); break;
    case IR_BIN: sb_printf(out, "  %%%d = %s.%d%s %%%d, %%%d\n", i->dst, opname(i->sub), i->w, sg, i->a, i->b); break;
    case IR_NEG: sb_printf(out, "  %%%d = neg.%d%s %%%d\n", i->dst, i->w, sg, i->a); break;
    case IR_NOT: sb_printf(out, "  %%%d = not.%d%s %%%d\n", i->dst, i->w, sg, i->a); break;
    case IR_CMP: sb_printf(out, "  %%%d = cmp.%s%s %%%d, %%%d\n", i->dst, opname(i->sub), sg, i->a, i->b); break;
    case IR_EXT: sb_printf(out, "  %%%d = ext.%d%s %%%d\n", i->dst, i->w, sg, i->a); break;
    case IR_JMP: sb_printf(out, "  jmp .L%lld\n", (long long)i->imm); break;
    case IR_BRZ: sb_printf(out, "  brz %%%d, .L%lld\n", i->a, (long long)i->imm); break;
    case IR_BRNZ: sb_printf(out, "  brnz %%%d, .L%lld\n", i->a, (long long)i->imm); break;
    case IR_CALL:
        sb_printf(out, "  ");
        if (i->dst > 0) sb_printf(out, "%%%d = ", i->dst);
        if (i->sym) sb_printf(out, "call %s(", i->sym); else sb_printf(out, "call *%%%d(", i->a);
        for (int k = 0; k < i->nargs; k++) sb_printf(out, "%s%%%d", k ? ", " : "", i->args[k]);
        sb_printf(out, ")%s ret=%d%s", i->nout ? " varargs" : "", i->w, i->w ? sg : "");
        if (i->rsz) sb_printf(out, " retagg=%d into [%%%d]", i->rsz, i->rt);
        for (int k = 0; k < i->nargs; k++) if (i->asz && i->asz[k]) sb_printf(out, " arg%d=agg%d", k, i->asz[k]);
        sb_printf(out, "\n");
        break;
    case IR_RET:
        if (i->a > 0) sb_printf(out, "  ret%s %%%d\n", i->sub ? ".agg" : "", i->a); else sb_printf(out, "  ret\n");
        break;
    case IR_COPY: sb_printf(out, "  copy [%%%d], [%%%d], %lld\n", i->a, i->b, (long long)i->imm); break;
    case IR_ZERO: sb_printf(out, "  zero [%%%d], %lld\n", i->a, (long long)i->imm); break;
    case IR_ASM: {
        sb_printf(out, "  asm \"%s\" out(", i->sym);
        const int *o = (const int *)(intptr_t)i->imm;
        for (int k = 0; k < i->nout; k++) sb_printf(out, "%s%%%d", k ? ", " : "", o[k]);
        sb_printf(out, ") in(");
        for (int k = 0; k < i->nargs; k++) sb_printf(out, "%s%%%d", k ? ", " : "", i->args[k]);
        sb_printf(out, ")\n");
        break;
    }
    case IR_UNREACH: sb_printf(out, "  unreachable\n"); break;
    case IR_ATOMIC: {
        static const char *const nm[] = { "?", "load", "store", "xchg", "cas", "add", "sub", "and", "or", "xor", "nand", "fence" };
        sb_printf(out, "  ");
        if (i->dst > 0) sb_printf(out, "%%%d = ", i->dst);
        sb_printf(out, "atomic.%s.%d%s [%%%d], %%%d, %%%d\n", nm[i->sub], i->w, sg, i->a, i->b, i->c);
        break;
    }
    case IR_VAINFO: sb_printf(out, "  %%%d = vainfo %d\n", i->dst, i->sub); break;
    }
}

IrFn *ir_lower(Obj *f) {
    fn = f;
    ihead = itail = NULL;
    nreg = 0; nlab = f->nlabels;
    maxslot = -1; ltail = NULL;
    for (Obj *l = f->locals; l; l = l->next) { if (l->slot > maxslot) maxslot = l->slot; ltail = l; }
    int idx = 0;
    for (Obj *p = f->params; p && idx < f->nparams; p = p->next, idx++) {
        if (ty_is_agg(p->ty)) {                     /* агрегат-параметр: бэкенд копирует пришедшее значение в локальный слот */
            agg_ok(p->ty, f->body ? f->body->tok : NULL);
            Ins *a = emit(IR_LADDR);
            a->dst = newreg(); a->imm = p->slot;
            Ins *i = emit(IR_PARAM);
            i->sub = 1; i->a = a->dst; i->imm = idx; i->w = ty_unqual(p->ty)->size;
            continue;
        }
        Ins *i = emit(IR_PARAM);
        i->dst = newreg(); i->imm = idx;
        Ins *a = emit(IR_LADDR);
        a->dst = newreg(); a->imm = p->slot;
        E_store(a->dst, i->dst, p->ty->size, 0);
    }
    if (ty_is_agg(f->ty->base)) agg_ok(f->ty->base, f->body ? f->body->tok : NULL);
    gen_stmt(f->body);
    Ins *r;
    if (!strcmp(f->name, "main") && f->ty->base->k == TY_INT) {
        int z = E_const(0);
        r = emit(IR_RET);
        r->a = z; r->w = 4;
    } else {
        r = emit(IR_RET);
        r->a = -1;
    }
    IrFn *res = ka_new(sizeof *res);
    res->f = f; res->head = ihead; res->nreg = nreg; res->nlab = nlab;
    return res;
}

static void lower_func(Obj *f) {
    IrFn *fi = ir_lower(f);
    sb_printf(out, "func %s%s params=%d\n", f->name, f->is_static ? " static" : "", f->nparams);
    for (Obj *l = f->locals; l; l = l->next)
        sb_printf(out, "  slot%d: size=%d align=%d %s%s\n", l->slot, l->ty->size, l->align ? l->align : 1, l->name, l->is_param ? " (param)" : "");
    for (Ins *i = fi->head; i; i = i->next) print_ins(i);
    sb_printf(out, "end\n");
}

static void dump_data(Obj *o) {
    int size = o->ty->size;
    sb_printf(out, "data %s size=%d align=%d%s %s", o->name, size, o->align ? o->align : 1, o->is_static ? " static" : "",
              o->init ? (o->is_ro ? "rodata" : "data") : "bss");
    if (o->init && size == 8 && !o->relocs) {
        int64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (int64_t)((uint64_t)v << 8 | o->init[i]);
        sb_printf(out, " val=%lld", (long long)v);
    }
    sb_printf(out, "\n");
    if (o->init) {
        for (int i = 0; i < size; i += 32) {
            sb_printf(out, "  bytes");
            for (int j = i; j < size && j < i + 32; j++) sb_printf(out, " %02x", o->init[j]);
            sb_printf(out, "\n");
        }
        for (Reloc *r = o->relocs; r; r = r->next) sb_printf(out, "  reloc +%d -> %s%+lld\n", r->off, r->sym, (long long)r->addend);
    }
}

void ir_dump(Obj *globals, SB *o) {
    out = o;
    sb_printf(out, "; kcc IR\n");
    for (Obj *g = globals; g; g = g->next) {
        if (g->is_func) {
            if (g->body) lower_func(g);
            else sb_printf(out, "extern func %s\n", g->name);
        } else if (g->is_def) dump_data(g);
        else sb_printf(out, "extern data %s size=%d\n", g->name, g->ty->size);
    }
}
