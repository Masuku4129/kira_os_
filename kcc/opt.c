/* kcc: машинно-независимые оптимизации линейного IR одной функции (общие для x86-64 и AArch64).
 *   1. Продвижение скалярных локальных переменных (целые, указатели) из слотов стека в виртуальные регистры.
 *      Слот продвигается, только если его адрес нигде не уходит: каждый LADDR слота используется исключительно
 *      как адрес прямого LOAD/STORE/ZERO того же размера без volatile. Иначе слот остаётся в памяти.
 *      Переменная получает один vreg с несколькими определениями (fi->mdef), живущий в нормализованном виде
 *      (значение уже обрезано и расширено по типу слота, как это делает загрузка из памяти).
 *   2. Локальное (в пределах базового блока) распространение копий переменная -> временный vreg.
 *   3. Слияние: вычисление, результат которого сразу записывается в переменную, пишет в переменную напрямую.
 *   4. Удаление мёртвого кода (чистые операции без читателей).
 * Функции с setjmp не трогаем: после longjmp значения неизменяемых в регистрах переменных нельзя гарантировать. */
#include "kcc.h"
#include <string.h>

typedef void (*OpVisit)(Ins *i, int *r, int kind, void *ctx);   /* kind: 0 - чтение, 1 - запись, 2 - чтение и запись (выход asm) */

static void op_walk(Ins *i, OpVisit fn, void *ctx) {
    switch (i->op) {
    case IR_CONST: case IR_GADDR: case IR_LADDR: case IR_VAINFO: fn(i, &i->dst, 1, ctx); break;
    case IR_PARAM: fn(i, &i->a, 0, ctx); fn(i, &i->dst, 1, ctx); break;
    case IR_ATOMIC: fn(i, &i->a, 0, ctx); fn(i, &i->b, 0, ctx); fn(i, &i->c, 0, ctx); fn(i, &i->dst, 1, ctx); break;
    case IR_LOAD: case IR_MOV: case IR_NEG: case IR_NOT: case IR_EXT: fn(i, &i->a, 0, ctx); fn(i, &i->dst, 1, ctx); break;
    case IR_STORE: case IR_COPY: fn(i, &i->a, 0, ctx); fn(i, &i->b, 0, ctx); break;
    case IR_BIN: case IR_CMP: fn(i, &i->a, 0, ctx); fn(i, &i->b, 0, ctx); fn(i, &i->dst, 1, ctx); break;
    case IR_BRZ: case IR_BRNZ: case IR_ZERO: case IR_RET: fn(i, &i->a, 0, ctx); break;
    case IR_CALL:
        if (!i->sym) fn(i, &i->a, 0, ctx);
        for (int k = 0; k < i->nargs; k++) fn(i, &i->args[k], 0, ctx);
        fn(i, &i->rt, 0, ctx);
        fn(i, &i->dst, 1, ctx);
        break;
    case IR_ASM: {
        int *o = (int *)(intptr_t)i->imm;
        for (int k = 0; k < i->nargs; k++) fn(i, &i->args[k], 0, ctx);
        for (int k = 0; k < i->nout; k++) fn(i, &o[k], 2, ctx);
        break;
    }
    default: break;
    }
}

typedef struct { int nv; int *ndef, *nuse; Ins **defins; } OpCnt;
static void op_count(Ins *i, int *r, int kind, void *c) {
    OpCnt *k = c;
    int v = *r;
    if (v <= 0 || v > k->nv) return;
    if (kind != 0) { k->ndef[v]++; k->defins[v] = i; }
    if (kind != 1) k->nuse[v]++;
}
static void op_recount(OpCnt *c, Ins **a, int n) {
    memset(c->ndef, 0, sizeof(int) * (size_t)(c->nv + 1));
    memset(c->nuse, 0, sizeof(int) * (size_t)(c->nv + 1));
    memset(c->defins, 0, sizeof(Ins *) * (size_t)(c->nv + 1));
    for (int k = 0; k < n; k++) if (a[k]) op_walk(a[k], op_count, c);
}

typedef struct { const int *ladslot; unsigned char *ok; int *ref; const int *sz; const unsigned char *expsgn; } OpSlots;
static void op_slotuse(Ins *i, int *r, int kind, void *c) {
    OpSlots *s = c;
    int v = *r;
    if (kind == 1 || v <= 0) return;
    int sl = s->ladslot[v] - 1;
    if (sl < 0) return;
    int good = 0;
    if (r == &i->a && !i->vol) {
        if (i->op == IR_LOAD) good = i->w == s->sz[sl] && (i->w == 8 || i->sgn == s->expsgn[sl]);
        else if (i->op == IR_STORE) good = i->w == s->sz[sl] && i->b != v;
        else if (i->op == IR_ZERO) good = i->imm == s->sz[sl];
    }
    if (good) s->ref[sl]++;
    else s->ok[sl] = 0;
}

/* Значение, полученное инструкцией d, уже нормализовано по ширине w и знаковости sgn (повторное расширение не нужно). */
static int op_norm_ok(const Ins *d, int w, int sgn) {
    if (!d) return 0;
    switch (d->op) {
    case IR_CMP: return 1;
    case IR_BIN: case IR_NEG: case IR_NOT: case IR_LOAD: case IR_EXT: return d->w == w && d->sgn == sgn;
    case IR_CONST: {
        int64_t v = d->imm;
        int b = w * 8;
        if (sgn) return v >= -((int64_t)1 << (b - 1)) && v < ((int64_t)1 << (b - 1));
        return v >= 0 && v < ((int64_t)1 << b);
    }
    default: return 0;
    }
}

static int op_pure(const Ins *i) {
    switch (i->op) {
    case IR_CONST: case IR_GADDR: case IR_LADDR: case IR_MOV: case IR_EXT: case IR_NEG: case IR_NOT: case IR_CMP: return 1;
    case IR_BIN: return i->sub != ND_DIV && i->sub != ND_MOD;
    default: return 0;
    }
}

typedef struct { int *cp; int *head; int *nextt; int *act; int nact; int nv; const unsigned char *md; int phase; int dv; } OpCopy;
static void op_cpvisit(Ins *i, int *r, int kind, void *c) {
    OpCopy *o = c;
    (void)i;
    int v = *r;
    if (v <= 0 || v > o->nv) return;
    if (o->phase == 0) { if (kind == 0 && o->cp[v]) *r = o->cp[v]; }
    else if (kind != 0) {                       /* переопределение переменной v: копии с неё недействительны */
        for (int t = o->head[v]; t; t = o->nextt[t]) o->cp[t] = 0;
        o->head[v] = 0;
    }
}

void ir_optimize(IrFn *fi) {
    int nv = fi->nreg, n = 0;
    for (Ins *i = fi->head; i; i = i->next) n++;
    if (n == 0) return;
    Ins **a = ka_new(sizeof(Ins *) * (size_t)(n + 1));
    n = 0;
    for (Ins *i = fi->head; i; i = i->next) a[n++] = i;
    int has_setjmp = 0;
    for (int k = 0; k < n; k++) if (a[k]->op == IR_CALL && a[k]->sym && strstr(a[k]->sym, "setjmp")) has_setjmp = 1;

    OpCnt cnt = { nv, ka_new(sizeof(int) * (size_t)(nv + 2)), ka_new(sizeof(int) * (size_t)(nv + 2)), ka_new(sizeof(Ins *) * (size_t)(nv + 2)) };
    op_recount(&cnt, a, n);

    int nv2 = nv;
    unsigned char *mdef = ka_new((size_t)nv + 1);
    Obj *f = fi->f;
    int maxslot = -1;
    for (Obj *l = f->locals; l; l = l->next) if (l->slot > maxslot) maxslot = l->slot;

    if (!has_setjmp && maxslot >= 0) {
        int *ladslot = ka_new(sizeof(int) * (size_t)(nv + 2));
        for (int k = 0; k < n; k++)
            if (a[k]->op == IR_LADDR && a[k]->dst > 0 && a[k]->dst <= nv && cnt.ndef[a[k]->dst] == 1) ladslot[a[k]->dst] = (int)a[k]->imm + 1;
        int ns = maxslot + 1;
        unsigned char *ok = ka_new((size_t)ns), *expsgn = ka_new((size_t)ns);
        int *sz = ka_new(sizeof(int) * (size_t)ns), *ref = ka_new(sizeof(int) * (size_t)ns), *vsl = ka_new(sizeof(int) * (size_t)ns);
        for (Obj *l = f->locals; l; l = l->next) {
            if (l->slot < 0 || l->slot >= ns || !l->ty) continue;
            const Type *t = ty_unqual(l->ty);
            int scalar = t->k == TY_BOOL || t->k == TY_CHAR || t->k == TY_SHORT || t->k == TY_INT || t->k == TY_LONG ||
                         t->k == TY_LLONG || t->k == TY_ENUM || t->k == TY_PTR;
            sz[l->slot] = t->size;
            expsgn[l->slot] = (unsigned char)!(t->is_unsigned || t->k == TY_BOOL);
            ok[l->slot] = (unsigned char)(scalar && !l->ty->is_volatile && !t->is_volatile && (t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8));
        }
        /* LADDR с несколькими определениями или чужими использованиями делает слот непродвигаемым */
        for (int k = 0; k < n; k++)
            if (a[k]->op == IR_LADDR && a[k]->imm >= 0 && a[k]->imm < ns && a[k]->dst > 0 && a[k]->dst <= nv && cnt.ndef[a[k]->dst] != 1) ok[a[k]->imm] = 0;
        OpSlots ss = { ladslot, ok, ref, sz, expsgn };
        for (int k = 0; k < n; k++) op_walk(a[k], op_slotuse, &ss);
        int any = 0;
        for (int s = 0; s < ns; s++) if (ok[s] && ref[s] > 0) { vsl[s] = ++nv2; any = 1; }
        if (any) {
            unsigned char *md2 = ka_new((size_t)nv2 + 1);
            memcpy(md2, mdef, (size_t)nv + 1);
            mdef = md2;
            for (int s = 0; s < ns; s++) if (vsl[s]) mdef[vsl[s]] = 1;
            for (int k = 0; k < n; k++) {
                Ins *i = a[k];
                if (i->op == IR_LADDR && i->dst > 0 && ladslot[i->dst]) {
                    int s = ladslot[i->dst] - 1;
                    if (vsl[s]) { a[k] = NULL; continue; }
                }
                int s = -1;
                if ((i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_ZERO) && i->a > 0 && ladslot[i->a]) s = ladslot[i->a] - 1;
                if (s < 0 || !vsl[s]) continue;
                int V = vsl[s];
                if (i->op == IR_LOAD) { i->op = IR_MOV; i->a = V; }
                else if (i->op == IR_ZERO) { i->op = IR_CONST; i->dst = V; i->imm = 0; i->a = 0; }
                else {
                    int x = i->b, w = i->w, sg = expsgn[s];
                    int need = w < 8 && !op_norm_ok(cnt.defins[x], w, sg);
                    i->op = need ? IR_EXT : IR_MOV; i->dst = V; i->a = x; i->b = 0;
                    i->w = w; i->sgn = sg; i->vol = 0;
                }
            }
        }
    }
    int nvt = nv2;
    OpCnt c2 = { nvt, ka_new(sizeof(int) * (size_t)(nvt + 2)), ka_new(sizeof(int) * (size_t)(nvt + 2)), ka_new(sizeof(Ins *) * (size_t)(nvt + 2)) };
    /* сжатие массива: удалённые LADDR */
    {
        int m = 0;
        for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k];
        n = m;
    }
    op_recount(&c2, a, n);

    /* распространение копий и слияние в пределах базового блока */
    {
        OpCopy oc = { ka_new(sizeof(int) * (size_t)(nvt + 2)), ka_new(sizeof(int) * (size_t)(nvt + 2)), ka_new(sizeof(int) * (size_t)(nvt + 2)),
                      ka_new(sizeof(int) * (size_t)(nvt + 2)), 0, nvt, mdef, 0, 0 };
        int prev = -1;
        for (int k = 0; k < n; k++) {
            Ins *i = a[k];
            if (!i) continue;
            if (i->op == IR_LABEL || i->op == IR_JMP || i->op == IR_RET) {
                for (int q = 0; q < oc.nact; q++) {
                    int t = oc.act[q];
                    if (oc.cp[t]) oc.head[oc.cp[t]] = 0;
                    oc.cp[t] = 0;
                }
                oc.nact = 0;
                prev = -1;
            }
            oc.phase = 0; op_walk(i, op_cpvisit, &oc);
            oc.phase = 1; op_walk(i, op_cpvisit, &oc);
            if (i->op == IR_MOV && i->a > 0 && i->a <= nvt && mdef[i->a] && i->dst > 0 && c2.ndef[i->dst] == 1 && !mdef[i->dst]) {
                int t = i->dst, V = i->a;
                oc.cp[t] = V; oc.nextt[t] = oc.head[V]; oc.head[V] = t; oc.act[oc.nact++] = t;
            }
            /* слияние: p: x = op ...; i: V = mov x  =>  p: V = op ... */
            if (i->op == IR_MOV && i->dst > 0 && mdef[i->dst] && i->a > 0 && i->a <= nvt && !mdef[i->a] && prev >= 0) {
                Ins *p = a[prev];
                int x = i->a;
                if (p->dst == x && c2.ndef[x] == 1 && c2.nuse[x] == 1 &&
                    (p->op == IR_BIN || p->op == IR_NEG || p->op == IR_NOT || p->op == IR_CMP || p->op == IR_LOAD || p->op == IR_EXT || p->op == IR_CONST)) {
                    p->dst = i->dst;
                    a[k] = NULL;
                    continue;
                }
            }
            prev = k;
        }
    }

    /* удаление мёртвого кода */
    for (int changed = 1; changed;) {
        changed = 0;
        {
            int m = 0;
            for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k];
            n = m;
        }
        op_recount(&c2, a, n);
        for (int k = 0; k < n; k++) {
            Ins *i = a[k];
            if (op_pure(i) && i->dst > 0 && i->dst <= nvt && c2.nuse[i->dst] == 0) { a[k] = NULL; changed = 1; }
        }
    }
    {
        int m = 0;
        for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k];
        n = m;
    }
    for (int k = 0; k < n; k++) a[k]->next = k + 1 < n ? a[k + 1] : NULL;
    fi->head = n ? a[0] : NULL;
    fi->nreg = nvt;
    fi->mdef = mdef;
}

/* Числа определений/чтений каждого vreg и (для одиночного определения) инструкция-определение. Массивы размера fi->nreg + 1. */
void ir_usecount(IrFn *fi, int *ndef, int *nuse, Ins **defins) {
    OpCnt c = { fi->nreg, ndef, nuse, defins };
    for (Ins *i = fi->head; i; i = i->next) op_walk(i, op_count, &c);
}
