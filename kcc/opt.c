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
    if (o->phase == 0 || o->phase == 2) { if (kind == 0 && o->cp[v]) *r = o->cp[v]; }
    else if (kind != 0) {                       /* переопределение переменной v: копии с неё недействительны */
        for (int t = o->head[v]; t; t = o->nextt[t]) o->cp[t] = 0;
        o->head[v] = 0;
    }
}



/* Приведение 64-битного результата к значению типа ширины w (1,2,4,8) со знаком sgn - как normr/norm в бэкенде. */
static int64_t op_wrap(int64_t v, int w, int sgn) {
    switch (w) {
    case 1: return sgn ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
    case 2: return sgn ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
    case 4: return sgn ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
    default: return v;
    }
}
/* Свёртка константных операций (операнды - vreg с одним определением IR_CONST). Возвращает 1, если инструкция стала IR_CONST. */
static int op_fold(Ins *i, const OpCnt *c, const unsigned char *md) {
#define OP_CST(v, out) ((v) > 0 && (v) <= c->nv && c->ndef[v] == 1 && !md[v] && c->defins[v] && c->defins[v]->op == IR_CONST && ((out) = c->defins[v]->imm, 1))
    int64_t x = 0, y = 0, r = 0;
    if (i->op == IR_EXT && OP_CST(i->a, x)) { i->imm = op_wrap(x, i->w, i->sgn); }
    else if (i->op == IR_NEG && OP_CST(i->a, x)) { i->imm = op_wrap((int64_t)(0 - (uint64_t)x), i->w, i->sgn); }
    else if (i->op == IR_NOT && OP_CST(i->a, x)) { i->imm = op_wrap(~x, i->w, i->sgn); }
    else if (i->op == IR_BIN && OP_CST(i->a, x) && OP_CST(i->b, y)) {
        switch (i->sub) {
        case ND_ADD: r = (int64_t)((uint64_t)x + (uint64_t)y); break;
        case ND_SUB: r = (int64_t)((uint64_t)x - (uint64_t)y); break;
        case ND_MUL: r = (int64_t)((uint64_t)x * (uint64_t)y); break;
        case ND_AND: r = x & y; break;
        case ND_OR: r = x | y; break;
        case ND_XOR: r = x ^ y; break;
        case ND_SHL: r = (int64_t)((uint64_t)x << (y & 63)); break;
        case ND_SHR: r = i->sgn ? (x >> (y & 63)) : (int64_t)((uint64_t)x >> (y & 63)); break;
        default: return 0;                      /* деление, остаток: не сворачиваем (деление на ноль) */
        }
        i->imm = op_wrap(r, i->w, i->sgn);
    } else if (i->op == IR_CMP && OP_CST(i->a, x) && OP_CST(i->b, y)) {
        switch (i->sub) {
        case ND_EQ: r = x == y; break;
        case ND_NE: r = x != y; break;
        case ND_LT: r = i->sgn ? x < y : (uint64_t)x < (uint64_t)y; break;
        case ND_LE: r = i->sgn ? x <= y : (uint64_t)x <= (uint64_t)y; break;
        default: return 0;
        }
        i->imm = r;
    } else return 0;
#undef OP_CST
    i->op = IR_CONST; i->a = i->b = 0; i->sub = 0;
    return 1;
}

/* ---------- граф потока управления и доминаторы ---------- */
/* Переменная (vreg с пометкой mdef) с единственным определением, которое доминирует над всеми чтениями, ведёт себя как SSA-значение:
 * ей можно снять пометку mdef, и константы/копии через неё распространяются глобально. Доминаторы - итеративный алгоритм
 * Купера-Харви-Кеннеди по блокам, выделенным в линейном IR (лидеры: начало, метка, инструкция после перехода/ret). */
typedef struct { const int *bof; const int *rpo; const int *idom; const int *defpos; unsigned char *ok; const unsigned char *md; const int *ndef; int pos; int nv; } OpDom;
static int op_dominates(const OpDom *d, int a, int b) {              /* блок a доминирует над блоком b */
    if (d->rpo[b] < 0) return 1;                                      /* недостижимый блок: чтения там безвредны */
    int x = b;
    while (d->rpo[x] > d->rpo[a]) x = d->idom[x];
    return x == a;
}
static void op_domuse(Ins *i, int *r, int kind, void *c) {
    OpDom *d = c;
    (void)i;
    int v = *r;
    if (kind == 1 || v <= 0 || v > d->nv || !d->md[v] || d->ndef[v] != 1 || !d->ok[v]) return;
    int dp = d->defpos[v];
    if (dp < 0) { d->ok[v] = 0; return; }
    int ub = d->bof[d->pos], db = d->bof[dp];
    if (ub == db) { if (d->pos <= dp) d->ok[v] = 0; }
    else if (!op_dominates(d, db, ub)) d->ok[v] = 0;
}
typedef struct { int *defpos; const int *ndef; int pos; int nv; } OpDefPos;
static void op_defpos(Ins *i, int *r, int kind, void *c) {
    OpDefPos *d = c;
    (void)i;
    if (kind != 0 && *r > 0 && *r <= d->nv && d->ndef[*r] == 1) d->defpos[*r] = d->pos;
}

static void op_ssa_refine(Ins **a, int n, int nv, unsigned char *mdef, const int *ndef, int nlab) {
    if (n == 0) return;
    int any = 0;
    for (int v = 1; v <= nv; v++) if (mdef[v] && ndef[v] == 1) { any = 1; break; }
    if (!any) return;
    int *bof = ka_new(sizeof(int) * (size_t)(n + 1)), *bs = ka_new(sizeof(int) * (size_t)(n + 2)), *be = ka_new(sizeof(int) * (size_t)(n + 2));
    int nb = 0;
    for (int k = 0; k < n; k++) {
        int lead = k == 0 || a[k]->op == IR_LABEL ||
                   a[k - 1]->op == IR_JMP || a[k - 1]->op == IR_BRZ || a[k - 1]->op == IR_BRNZ || a[k - 1]->op == IR_RET || a[k - 1]->op == IR_UNREACH;
        if (lead) { bs[nb] = k; nb++; }
        bof[k] = nb - 1;
        be[nb - 1] = k;
    }
    int *labblk = ka_new(sizeof(int) * (size_t)(nlab + 3));
    for (int l = 0; l <= nlab + 2; l++) labblk[l] = -1;
    for (int k = 0; k < n; k++) if (a[k]->op == IR_LABEL && a[k]->imm >= 0 && a[k]->imm <= nlab + 2) labblk[a[k]->imm] = bof[k];
    int *su = ka_new(sizeof(int) * (size_t)(2 * nb + 2)), *npred = ka_new(sizeof(int) * (size_t)(nb + 1));   /* su[2b], su[2b+1], -1 - нет */
    for (int b = 0; b < nb; b++) {
        const Ins *l = a[be[b]];
        int s0 = -1, s1 = -1;
        int tgt = (l->imm >= 0 && l->imm <= nlab + 2) ? labblk[l->imm] : -1;
        if (l->op == IR_JMP) s0 = tgt;
        else if (l->op == IR_BRZ || l->op == IR_BRNZ) { s0 = tgt; s1 = b + 1 < nb ? b + 1 : -1; }
        else if (l->op == IR_RET || l->op == IR_UNREACH) { }
        else s0 = b + 1 < nb ? b + 1 : -1;
        su[2 * b] = s0; su[2 * b + 1] = s1;
        if (s0 >= 0) npred[s0]++;
        if (s1 >= 0) npred[s1]++;
    }
    int *pst = ka_new(sizeof(int) * (size_t)(nb + 2)), *pl = ka_new(sizeof(int) * (size_t)(2 * nb + 2)), *pfill = ka_new(sizeof(int) * (size_t)(nb + 1));
    for (int b = 0; b < nb; b++) pst[b + 1] = pst[b] + npred[b];
    for (int b = 0; b < nb; b++)
        for (int e = 0; e < 2; e++) { int t = su[2 * b + e]; if (t >= 0) pl[pst[t] + pfill[t]++] = b; }
    /* обратный порядок обхода в глубину (итеративно) */
    int *rpo = ka_new(sizeof(int) * (size_t)(nb + 1)), *post = ka_new(sizeof(int) * (size_t)(nb + 1));
    int *stk = ka_new(sizeof(int) * (size_t)(nb + 1)), *sidx = ka_new(sizeof(int) * (size_t)(nb + 1));
    unsigned char *seen = ka_new((size_t)nb + 1);
    for (int b = 0; b < nb; b++) rpo[b] = -1;
    int sp = 0, np = 0;
    stk[sp] = 0; sidx[sp] = 0; sp++; seen[0] = 1;
    while (sp) {
        int b = stk[sp - 1];
        if (sidx[sp - 1] < 2) {
            int t = su[2 * b + sidx[sp - 1]++];
            if (t >= 0 && !seen[t]) { seen[t] = 1; stk[sp] = t; sidx[sp] = 0; sp++; }
        } else { post[np++] = b; sp--; }
    }
    for (int q = 0; q < np; q++) rpo[post[q]] = np - 1 - q;
    int *order = ka_new(sizeof(int) * (size_t)(np + 1));
    for (int q = 0; q < np; q++) order[rpo[post[q]]] = post[q];
    int *idom = ka_new(sizeof(int) * (size_t)(nb + 1));
    for (int b = 0; b < nb; b++) idom[b] = -1;
    idom[0] = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int q = 1; q < np; q++) {
            int b = order[q], nd = -1;
            for (int e = pst[b]; e < pst[b + 1]; e++) {
                int pr = pl[e];
                if (rpo[pr] < 0 || idom[pr] < 0) continue;
                if (nd < 0) { nd = pr; continue; }
                int x = pr, y = nd;
                while (x != y) {
                    while (rpo[x] > rpo[y]) x = idom[x];
                    while (rpo[y] > rpo[x]) y = idom[y];
                }
                nd = x;
            }
            if (nd >= 0 && idom[b] != nd) { idom[b] = nd; changed = 1; }
        }
    }
    int *defpos = ka_new(sizeof(int) * (size_t)(nv + 2));
    for (int v = 0; v <= nv; v++) defpos[v] = -1;
    OpDefPos dp = { defpos, ndef, 0, nv };
    for (int k = 0; k < n; k++) { dp.pos = k; op_walk(a[k], op_defpos, &dp); }
    unsigned char *ok = ka_new((size_t)nv + 2);
    for (int v = 1; v <= nv; v++) ok[v] = 1;
    OpDom od = { bof, rpo, idom, defpos, ok, mdef, ndef, 0, nv };
    for (int k = 0; k < n; k++) { od.pos = k; op_walk(a[k], op_domuse, &od); }
    for (int v = 1; v <= nv; v++) if (mdef[v] && ndef[v] == 1 && ok[v] && defpos[v] >= 0) mdef[v] = 0;
}

void ir_optimize(IrFn *fi) {
    int nv = fi->nreg, n = 0;
    for (Ins *i = fi->head; i; i = i->next) n++;
    if (n == 0) return;
    int maxslot0 = -1;
    for (Obj *l = fi->f->locals; l; l = l->next) if (l->slot > maxslot0) maxslot0 = l->slot;
    /* mdef живёт дольше прохода (читают бэкенды): размер с запасом под новые vreg переменных (не больше числа слотов) */
    unsigned char *mdef = ka_new((size_t)nv + (size_t)(maxslot0 + 1) + 2);
    size_t mk = ka_mark();                      /* всё ниже, кроме mdef, временное: освобождается в конце */
    Ins **a = ka_new(sizeof(Ins *) * (size_t)(n + 1));
    n = 0;
    for (Ins *i = fi->head; i; i = i->next) a[n++] = i;
    int has_setjmp = 0;
    for (int k = 0; k < n; k++) if (a[k]->op == IR_CALL && a[k]->sym && strstr(a[k]->sym, "setjmp")) has_setjmp = 1;

    OpCnt cnt = { nv, ka_new(sizeof(int) * (size_t)(nv + 2)), ka_new(sizeof(int) * (size_t)(nv + 2)), ka_new(sizeof(Ins *) * (size_t)(nv + 2)) };
    op_recount(&cnt, a, n);

    int nv2 = nv;
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

    /* упрощение расширений нормализованных значений и глобальное распространение копий между vreg с одним определением */
    for (int round = 0; round < 4; round++) {
        {
            int m = 0;
            for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k];
            n = m;
        }
        op_recount(&c2, a, n);
        op_ssa_refine(a, n, nvt, mdef, c2.ndef, fi->nlab);
        int ch = 0;
        for (int k = 0; k < n; k++) if (op_fold(a[k], &c2, mdef)) ch = 1;
        if (ch) op_recount(&c2, a, n);
        for (int k = 0; k < n; k++) {
            Ins *i = a[k];
            if (i->op == IR_EXT && i->a > 0 && i->a <= nvt && c2.ndef[i->a] == 1 && !mdef[i->a] &&
                (i->w >= 8 || op_norm_ok(c2.defins[i->a], i->w, i->sgn))) { i->op = IR_MOV; ch = 1; }
        }
        int *rep = ka_new(sizeof(int) * (size_t)(nvt + 2));
        int any = 0;
        for (int k = 0; k < n; k++) {
            const Ins *i = a[k];
            if (i->op == IR_MOV && i->a > 0 && i->dst > 0 && i->a <= nvt && i->dst <= nvt && i->dst != i->a &&
                c2.ndef[i->dst] == 1 && c2.ndef[i->a] == 1 && !mdef[i->dst] && !mdef[i->a]) { rep[i->dst] = i->a; any = 1; }
        }
        if (any) {
            for (int v = 1; v <= nvt; v++) { int r = rep[v], g = 0; while (r && rep[r] && g++ < 64) r = rep[r]; rep[v] = r; }
            OpCopy oc2 = { rep, NULL, NULL, NULL, 0, nvt, mdef, 2, 0 };
            for (int k = 0; k < n; k++) op_walk(a[k], op_cpvisit, &oc2);
            ch = 1;
        }
        if (!ch) break;
        /* мёртвые копии убираются общим проходом ниже; здесь только удаляем уже нечитаемые */
        op_recount(&c2, a, n);
        for (int k = 0; k < n; k++) {
            Ins *i = a[k];
            if (op_pure(i) && i->dst > 0 && i->dst <= nvt && c2.nuse[i->dst] == 0) a[k] = NULL;
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
    /* слияние "t = op ...; v = t" в "v = op ...", если t живёт ровно между ними */
    op_recount(&c2, a, n);
    for (int k = 0; k + 1 < n; k++) {
        Ins *p = a[k], *q = a[k + 1];
        if (!p || !q || q->op != IR_MOV || p->dst <= 0 || p->dst > nvt || q->a != p->dst || q->dst == p->dst) continue;
        if (p->op != IR_BIN && p->op != IR_CMP && p->op != IR_EXT && p->op != IR_NEG && p->op != IR_NOT) continue;
        if (c2.ndef[p->dst] != 1 || c2.nuse[p->dst] != 1) continue;
        p->dst = q->dst;
        a[k + 1] = NULL;
        k++;
    }
    { int m2 = 0; for (int k = 0; k < n; k++) if (a[k]) a[m2++] = a[k]; n = m2; }
    for (int k = 0; k + 1 < n; k++)
        if (a[k]->op == IR_JMP && a[k + 1]->op == IR_LABEL && a[k]->imm == a[k + 1]->imm) a[k] = NULL;
    { int m = 0; for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k]; n = m; }
    for (int k = 0; k < n; k++) a[k]->next = k + 1 < n ? a[k + 1] : NULL;
    fi->head = n ? a[0] : NULL;
    fi->nreg = nvt;
    fi->mdef = mdef;
    ka_release(mk);
}

/* Числа определений/чтений каждого vreg и (для одиночного определения) инструкция-определение. Массивы размера fi->nreg + 1. */
void ir_usecount(IrFn *fi, int *ndef, int *nuse, Ins **defins) {
    OpCnt c = { fi->nreg, ndef, nuse, defins };
    for (Ins *i = fi->head; i; i = i->next) op_walk(i, op_count, &c);
}

/* ---------- инлайнинг ---------- */
/* Прямой вызов небольшой функции этой же единицы трансляции заменяется копией её IR (до ir_optimize): vreg, метки и слоты
 * переносятся со сдвигом, IR_PARAM становится копией аргумента, ret - копией значения и переходом в конец тела.
 * Не встраиваются: variadic, агрегаты в параметрах и результате, asm, atomic, va_*, setjmp, рекурсия на себя, тела больше INL_MAX.
 * Один уровень: вставленный код повторно не просматривается (вложенность даёт порядок обхода: callee уже мог встроить своё). */
#define INL_MAX 100
#define INL_CALLER_MAX 20000

static void op_shift(Ins *i, int *r, int kind, void *c) {
    (void)i; (void)kind;
    if (*r > 0) *r += *(int *)c;
}

static int inl_ok(const Obj *callee, const Obj *caller, const Ins *call) {
    if (!callee || callee == caller || !callee->is_func || !callee->body || !callee->ir) return 0;
    if (callee->ty->variadic || callee->ty->noproto || callee->nparams > 6 || callee->nparams != call->nargs) return 0;
    if (!strcmp(callee->name, "main") || ty_is_agg(callee->ty->base)) return 0;
    for (const Obj *p = callee->params; p; p = p->next) if (ty_is_agg(p->ty)) return 0;
    const IrFn *ci = callee->ir;
    int n = 0;
    for (const Ins *i = ci->head; i; i = i->next) {
        if (++n > INL_MAX) return 0;
        switch (i->op) {
        case IR_ASM: case IR_VAINFO: case IR_ATOMIC: return 0;
        case IR_PARAM: case IR_RET: if (i->sub) return 0; break;
        case IR_CALL:
            if (i->rsz || (i->sym && strstr(i->sym, "setjmp"))) return 0;
            for (int k = 0; k < i->nargs; k++) if (i->asz && i->asz[k]) return 0;
            break;
        default: break;
        }
    }
    return 1;
}

void ir_inline(Obj *caller) {
    IrFn *fi = caller->ir;
    int total = 0;
    for (Ins *i = fi->head; i; i = i->next) total++;
    Ins *prev = NULL;
    for (Ins *c = fi->head; c;) {
        Ins *nextc = c->next;
        int site_ok = c->op == IR_CALL && c->sym && !c->rsz;
        for (int k = 0; site_ok && c->asz && k < c->nargs; k++) if (c->asz[k]) site_ok = 0;   /* агрегаты по значению - не встраиваем */
        Obj *callee = site_ok ? gen_lookup(c->sym) : NULL;
        if (!callee || total >= INL_CALLER_MAX || !inl_ok(callee, caller, c)) { prev = c; c = nextc; continue; }
        const IrFn *ci = callee->ir;
        int vbase = fi->nreg, lbase = fi->nlab + 1, lend = lbase + ci->nlab + 1;
        int cmax = -1;
        Obj *tail = NULL;
        for (Obj *l = caller->locals; l; l = l->next) { if (l->slot > cmax) cmax = l->slot; tail = l; }
        int sbase = cmax + 1;
        for (const Obj *l = callee->locals; l; l = l->next) {
            Obj *nl = ka_new(sizeof(Obj));
            *nl = *l; nl->slot = l->slot + sbase; nl->next = NULL;
            if (tail) tail->next = nl; else caller->locals = nl;
            tail = nl;
        }
        Ins *head = NULL, *tl = NULL;
        const Ins *last = NULL;
        for (const Ins *i = ci->head; i; i = i->next) last = i;
#define INL_PUSH(x) do { Ins *q_ = (x); q_->next = NULL; if (tl) tl->next = q_; else head = q_; tl = q_; } while (0)
        for (const Ins *i = ci->head; i; i = i->next) {
            Ins *n = ka_new(sizeof(Ins));
            *n = *i;
            if (i->op == IR_PARAM) {
                n->op = IR_MOV; n->a = c->args[i->imm]; n->b = 0; n->sub = 0; n->imm = 0;
                n->dst = i->dst > 0 ? i->dst + vbase : 0;
                INL_PUSH(n);
                continue;
            }
            if (i->op == IR_CALL && i->nargs) {
                n->args = ka_new(sizeof(int) * (size_t)i->nargs);
                memcpy(n->args, i->args, sizeof(int) * (size_t)i->nargs);
            }
            op_walk(n, op_shift, &vbase);
            if (n->op == IR_LADDR) n->imm += sbase;
            if (n->op == IR_LABEL || n->op == IR_JMP || n->op == IR_BRZ || n->op == IR_BRNZ) n->imm += lbase;
            if (n->op == IR_RET) {
                if (c->dst > 0 && n->a > 0) {
                    Ins *m = ka_new(sizeof(Ins));
                    m->op = IR_MOV; m->dst = c->dst; m->a = n->a;
                    INL_PUSH(m);
                }
                if (i != last) {
                    Ins *j = ka_new(sizeof(Ins));
                    j->op = IR_JMP; j->imm = lend;
                    INL_PUSH(j);
                }
                continue;
            }
            INL_PUSH(n);
        }
        Ins *le = ka_new(sizeof(Ins));
        le->op = IR_LABEL; le->imm = lend;
        INL_PUSH(le);
#undef INL_PUSH
        if (prev) prev->next = head; else fi->head = head;
        tl->next = nextc;
        for (const Ins *i = head; i != nextc; i = i->next) total++;
        fi->nreg = vbase + ci->nreg;
        fi->nlab = lend;
        prev = tl;
        c = nextc;
    }
}

void ir_walk(Ins *i, OpVisit fn, void *ctx) { op_walk(i, fn, ctx); }
