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
        else if (l->op != IR_RET && l->op != IR_UNREACH) s0 = b + 1 < nb ? b + 1 : -1;     /* RET/UNREACH: преемников нет */
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


/* ---------- вынос инвариантов из циклов ----------
 * Цикл - диапазон [l, e] линейного IR: метка L на позиции l и самый поздний переход на L (позиция e >= l). Вход в цикл - только
 * проходом сверху через L (переходов на метки диапазона снаружи нет). Выносятся чистые операции без ловушек (не DIV/MOD, не CMP:
 * сравнения остаются рядом с переходом ради слияния), у которых единственное определение, а операнды не меняются в цикле.
 * Загрузки не выносятся: анализа псевдонимов нет. Выполнение вне цикла лишь спекулятивно, побочных эффектов у таких операций нет. */
typedef struct { int *dcnt; const OpCnt *c; const unsigned char *md; int bad; int nv; } OpLicm;
static void op_licm_def(Ins *i, int *r, int kind, void *x) {
    (void)i;
    OpLicm *L = x;
    if (kind != 0 && *r > 0 && *r <= L->nv) L->dcnt[*r]++;
}
static int op_licm_cheap(const OpLicm *L, int v) {
    const Ins *d = L->c->defins[v];
    return L->c->ndef[v] == 1 && !L->md[v] && d && (d->op == IR_CONST || d->op == IR_GADDR || d->op == IR_LADDR);
}
static void op_licm_use(Ins *i, int *r, int kind, void *x) {
    (void)i;
    OpLicm *L = x;
    int v = *r;
    if (kind != 0 || v <= 0) return;
    if (v > L->nv) { L->bad = 1; return; }
    if (L->dcnt[v] == 0 || op_licm_cheap(L, v)) return;
    L->bad = 1;
}
static void op_licm_unreg(Ins *i, int *r, int kind, void *x) {
    (void)i;
    OpLicm *L = x;
    if (kind != 0 && *r > 0 && *r <= L->nv) L->dcnt[*r] = 0;
}
static int op_licm_loop(Ins **a, int n, int nlab, int lab, OpLicm *L, Ins **tmp, unsigned char *hk) {
    int l = -1, e = -1;
    for (int k = 0; k < n; k++) {
        if (a[k]->op == IR_LABEL && a[k]->imm == lab) l = k;
        else if ((a[k]->op == IR_JMP || a[k]->op == IR_BRZ || a[k]->op == IR_BRNZ) && a[k]->imm == lab) e = k;
    }
    if (l < 0 || e < l) return 0;
    /* вход только сверху: на метки диапазона не ведёт ни один переход снаружи; внутри - ни asm, ни вызовов setjmp */
    for (int k = 0; k < n; k++) {
        const Ins *j = a[k];
        if (k >= l && k <= e) { if (j->op == IR_ASM) return 0; continue; }
        if (j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) continue;
        if (j->imm < 0 || j->imm > nlab) return 0;
        for (int q = l; q <= e; q++) if (a[q]->op == IR_LABEL && a[q]->imm == j->imm) return 0;
    }
    for (int k = l; k <= e; k++) op_walk(a[k], op_licm_def, L);
    int moved = 0;
    memset(hk, 0, (size_t)n);
    for (int pass = 0; pass < 8; pass++) {
        int ch = 0;
        for (int k = l + 1; k <= e; k++) {
            Ins *i = a[k];
            if (hk[k]) continue;
            if (i->op != IR_BIN && i->op != IR_NEG && i->op != IR_NOT && i->op != IR_EXT && i->op != IR_MOV) continue;
            if (i->op == IR_BIN && (i->sub == ND_DIV || i->sub == ND_MOD)) continue;
            int d = i->dst;
            if (d <= 0 || d > L->nv || L->c->ndef[d] != 1 || L->md[d] || L->dcnt[d] != 1) continue;
            L->bad = 0;
            op_walk(i, op_licm_use, L);
            if (L->bad) continue;
            hk[k] = 1; L->dcnt[d] = 0; ch = 1; moved++;
        }
        if (!ch) break;
    }
    if (moved) {
        /* определения констант и адресов, нужные вынесенным операциям, выносятся вместе с ними */
        for (int k = l + 1; k <= e; k++) {
            if (!hk[k]) continue;
            for (int q = l + 1; q <= e; q++) {
                const Ins *c = a[q];
                if (hk[q] || (c->op != IR_CONST && c->op != IR_GADDR && c->op != IR_LADDR)) continue;
                int dv = c->dst, used = 0;
                if (dv > 0 && (a[k]->a == dv || a[k]->b == dv)) used = 1;
                if (used && L->c->ndef[dv] == 1 && !L->md[dv]) hk[q] = 1;
            }
        }
        int m = 0;
        for (int k = 0; k < l; k++) tmp[m++] = a[k];
        for (int k = l + 1; k <= e; k++) if (hk[k]) tmp[m++] = a[k];
        for (int k = l; k <= e; k++) if (!hk[k]) tmp[m++] = a[k];
        for (int k = e + 1; k < n; k++) tmp[m++] = a[k];
        memcpy(a, tmp, sizeof(Ins *) * (size_t)n);
    }
    for (int k = 0; k < n; k++) op_walk(a[k], op_licm_unreg, L);
    return moved;
}
static void op_licm(Ins **a, int n, int nlab, const OpCnt *c, const unsigned char *md, int nv) {
    if (n < 4) return;
    Ins **tmp = ka_new(sizeof(Ins *) * (size_t)(n + 1));
    unsigned char *hk = ka_new((size_t)n + 1);
    int *dcnt = ka_new(sizeof(int) * (size_t)(nv + 2));
    OpLicm L = { dcnt, c, md, 0, nv };
    int *labs = ka_new(sizeof(int) * (size_t)(n + 1)), *len = ka_new(sizeof(int) * (size_t)(n + 1)), nl = 0;
    int *lp = ka_new(sizeof(int) * (size_t)(nlab + 2));
    for (int k = 0; k < n; k++) if (a[k]->op == IR_LABEL && a[k]->imm >= 0 && a[k]->imm <= nlab) lp[a[k]->imm] = k + 1;
    for (int k = 0; k < n; k++) {
        const Ins *j = a[k];
        if ((j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) || j->imm < 0 || j->imm > nlab || !lp[j->imm] || lp[j->imm] - 1 > k) continue;
        int found = -1;
        for (int q = 0; q < nl; q++) if (labs[q] == (int)j->imm) found = q;
        int span = k - (lp[j->imm] - 1);
        if (found < 0) { labs[nl] = (int)j->imm; len[nl++] = span; }
        else if (span > len[found]) len[found] = span;
    }
    for (int x = 1; x < nl; x++) {                        /* внутренние циклы первыми */
        int tl = labs[x], tn = len[x], y = x - 1;
        while (y >= 0 && len[y] > tn) { labs[y + 1] = labs[y]; len[y + 1] = len[y]; y--; }
        labs[y + 1] = tl; len[y + 1] = tn;
    }
    for (int q = 0; q < nl; q++) op_licm_loop(a, n, nlab, labs[q], &L, tmp, hk);
}



/* ---------- понижение силы умножения по индукционной переменной ----------
 * Цикл - как в op_licm_loop. Индукционная переменная v: единственное определение внутри цикла "v = v +/- K" (K - константа; ширина 8, либо
 * ширина 4 со знаком - переполнение знакового int не определено, как и у других компиляторов), определение стоит вне вложенных циклов.
 * Произведение m = v * C (ширина 8, C - константа, не 0/1/2/4/8: степени двойки кодируются масштабом адреса), стоящее до обновления v
 * и все использования которого лежат между ним и обновлением, заменяется накопителем p: перед циклом p = v * C, сразу после v = v + K
 * добавляется p = p + K*C. Инвариант p == v*C держится везде, кроме промежутка между двумя соседними командами. Для ширины 8 это
 * равенство по модулю 2^64 - точное. Циклы с лишними обратными переходами (кроме вложенных циклов целиком) не трогаются. */
static Ins *op_newins(void);
typedef struct { int v, cnt; } OpSrDef;
static void op_sr_defcnt(Ins *i, int *r, int kind, void *x) { (void)i; OpSrDef *d = x; if (kind != 0 && *r == d->v) d->cnt++; }
typedef struct { int m, p; } OpSrRen;
static void op_sr_ren(Ins *i, int *r, int kind, void *x) { (void)i; OpSrRen *d = x; if (kind == 0 && *r == d->m) *r = d->p; }
typedef struct { int m, cnt, lo, hi, bad; const unsigned char *cov; int pos; } OpSrUse;
static void op_sr_use(Ins *i, int *r, int kind, void *x) {
    (void)i;
    OpSrUse *d = x;
    if (*r != d->m) return;
    if (kind != 0) { d->bad = 1; return; }
    d->cnt++;
    if (d->pos <= d->lo || d->pos >= d->hi || d->cov[d->pos]) d->bad = 1;
}
static int op_sr_isconst(const OpCnt *c, const unsigned char *md, int v, int64_t *out) {
    if (v <= 0 || v > c->nv || c->ndef[v] != 1 || md[v] || !c->defins[v] || c->defins[v]->op != IR_CONST) return 0;
    *out = c->defins[v]->imm;
    return 1;
}
static int op_sr_loop(Ins **a, int *pn, Ins **b, int lab, int nlab, OpCnt *c, unsigned char *mdef, int *pnv, int vcap) {
    int n = *pn, l = -1, e = -1;
    for (int k = 0; k < n; k++) {
        if (a[k]->op == IR_LABEL && a[k]->imm == lab) l = k;
        else if ((a[k]->op == IR_JMP || a[k]->op == IR_BRZ || a[k]->op == IR_BRNZ) && a[k]->imm == lab) e = k;
    }
    if (l < 0 || e <= l) return 0;
    int *lp = ka_new(sizeof(int) * (size_t)(nlab + 2));
    for (int k = 0; k < n; k++) if (a[k]->op == IR_LABEL && a[k]->imm >= 0 && a[k]->imm <= nlab) lp[a[k]->imm] = k + 1;
    for (int k = 0; k < n; k++) {                            /* вход только сверху */
        const Ins *j = a[k];
        if (k >= l && k <= e) { if (j->op == IR_ASM) return 0; continue; }
        if (j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) continue;
        if (j->imm < 0 || j->imm > nlab) return 0;
        if (lp[j->imm] && lp[j->imm] - 1 >= l && lp[j->imm] - 1 <= e) return 0;
    }
    unsigned char *cov = ka_new((size_t)n + 1);
    for (int k = l + 1; k < e; k++) {
        const Ins *j = a[k];
        if (j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) continue;
        if (j->imm < 0 || j->imm > nlab || !lp[j->imm]) return 0;
        int t = lp[j->imm] - 1;
        if (t > k) continue;                                  /* вперёд, в том числе выход из цикла */
        if (t <= l) return 0;                                 /* второй обратный переход на заголовок или за него */
        for (int q = t; q <= k; q++) cov[q] = 1;              /* вложенный цикл */
    }
    for (int k = l + 1; k < e; k++) {                         /* переход вперёд не должен вести внутрь вложенного цикла снаружи него */
        const Ins *j = a[k];
        if ((j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) || cov[k]) continue;
        int t = lp[j->imm] - 1;
        if (t > k && t < e && cov[t] && t > 0 && !cov[t - 1]) return 0;
        if (t > k && t < e && cov[t] && !(a[t]->op == IR_LABEL)) return 0;
    }
    int made = 0;
    int pre_n = 0, post_n = 0;
    Ins *pre[24], *post[12]; int post_at[12];
    int tv[12], tc[12], tp[12], nt = 0;
    OpCnt *cc = c;
    for (int pu = l + 1; pu < e && made < 3; pu++) {
        Ins *u = a[pu];
        if (!u || cov[pu] || u->op != IR_BIN || (u->sub != ND_ADD && u->sub != ND_SUB) || u->dst <= 0 || u->dst != u->a) continue;
        int v = u->dst, bad = 0;
        if (v > cc->nv || !mdef[v]) continue;
        if (!((u->w == 8) || (u->w == 4 && u->sgn))) continue;
        int64_t k1;
        if (!op_sr_isconst(cc, mdef, u->b, &k1)) continue;
        if (k1 > (1 << 20) || k1 < -(1 << 20)) continue;
        int64_t step = u->sub == ND_ADD ? k1 : -k1;
        OpSrDef dd = { v, 0 };
        for (int k = l; k <= e; k++) if (a[k]) op_walk(a[k], op_sr_defcnt, &dd);
        if (dd.cnt != 1) continue;
        (void)bad;
        for (int pm = l + 1; pm < pu; pm++) {
            Ins *m = a[pm];
            if (cov[pm] || !m || m->op != IR_BIN || m->sub != ND_MUL || m->w != 8) continue;
            int o = m->a == v ? m->b : m->b == v ? m->a : 0;
            int64_t C;
            if (!o || m->a == m->b || !op_sr_isconst(cc, mdef, o, &C)) continue;
            if (C == 0 || C == 1 || C == 2 || C == 4 || C == 8) continue;
            int md = m->dst;
            if (md <= 0 || md > cc->nv || cc->ndef[md] != 1 || mdef[md]) continue;
            OpSrUse us = { md, 0, pm, pu, 0, cov, 0 };
            for (int k = 0; k < n; k++) { if (!a[k] || a[k] == m) continue; us.pos = k; op_walk(a[k], op_sr_use, &us); }
            if (us.bad || us.cnt == 0) continue;
            int p = 0;
            for (int q = 0; q < nt; q++) if (tv[q] == v && tc[q] == (int)C) p = tp[q];
            if (!p) {
                if (nt >= 12 || pre_n + 2 > 24 || post_n >= 12 || *pnv + 3 > vcap) continue;
                Ins *i1 = op_newins(), *i2 = op_newins(), *i3 = op_newins(), *i4 = op_newins();
                if (!i1 || !i2 || !i3 || !i4) continue;
                int cv = ++*pnv, pv = ++*pnv, sv = ++*pnv;
                mdef[cv] = 0; mdef[sv] = 0; mdef[pv] = 1;
                memset(i1, 0, sizeof *i1); i1->op = IR_CONST; i1->dst = cv; i1->imm = C; i1->w = 8;
                memset(i2, 0, sizeof *i2); i2->op = IR_BIN; i2->sub = ND_MUL; i2->dst = pv; i2->a = v; i2->b = cv; i2->w = 8; i2->sgn = 1;
                memset(i3, 0, sizeof *i3); i3->op = IR_CONST; i3->dst = sv; i3->imm = (int64_t)((uint64_t)step * (uint64_t)C); i3->w = 8;
                memset(i4, 0, sizeof *i4); i4->op = IR_BIN; i4->sub = ND_ADD; i4->dst = pv; i4->a = pv; i4->b = sv; i4->w = 8;
                pre[pre_n++] = i1; pre[pre_n++] = i2; pre[pre_n++] = i3;
                post[post_n] = i4; post_at[post_n++] = pu;
                p = pv; tv[nt] = v; tc[nt] = (int)C; tp[nt++] = pv;
            }
            OpSrRen rn = { md, p };
            for (int k = 0; k < n; k++) if (a[k] && a[k] != m) op_walk(a[k], op_sr_ren, &rn);
            a[pm] = NULL;
            made++;
        }
    }
    if (!made) return 0;
    int t = 0;
    for (int k = 0; k < n; k++) {
        if (k == l) for (int q = 0; q < pre_n; q++) b[t++] = pre[q];
        if (a[k]) b[t++] = a[k];
        for (int q = 0; q < post_n; q++) if (post_at[q] == k) b[t++] = post[q];
    }
    memcpy(a, b, sizeof(Ins *) * (size_t)t);
    *pn = t;
    op_recount(c, a, t);
    return made;
}
static void op_sr(Ins **a, int *pn, int nlab, OpCnt *c, unsigned char *mdef, int *pnv, int vcap) {
    int n = *pn;
    if (n < 6) return;
    Ins **b = ka_new(sizeof(Ins *) * (size_t)(n + 64));
    int *labs = ka_new(sizeof(int) * (size_t)(n + 1)), *len = ka_new(sizeof(int) * (size_t)(n + 1)), nl = 0;
    int *lp = ka_new(sizeof(int) * (size_t)(nlab + 2));
    for (int k = 0; k < n; k++) if (a[k]->op == IR_LABEL && a[k]->imm >= 0 && a[k]->imm <= nlab) lp[a[k]->imm] = k + 1;
    for (int k = 0; k < n; k++) {
        const Ins *j = a[k];
        if ((j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) || j->imm < 0 || j->imm > nlab || !lp[j->imm] || lp[j->imm] - 1 > k) continue;
        int found = -1;
        for (int q = 0; q < nl; q++) if (labs[q] == (int)j->imm) found = q;
        int span = k - (lp[j->imm] - 1);
        if (found < 0) { labs[nl] = (int)j->imm; len[nl++] = span; }
        else if (span > len[found]) len[found] = span;
    }
    for (int x = 1; x < nl; x++) {
        int tl = labs[x], tn = len[x], y = x - 1;
        while (y >= 0 && len[y] > tn) { labs[y + 1] = labs[y]; len[y + 1] = len[y]; y--; }
        labs[y + 1] = tl; len[y + 1] = tn;
    }
    op_recount(c, a, n);
    for (int q = 0; q < nl; q++) {
        size_t mk2 = ka_mark();
        op_sr_loop(a, pn, b, labs[q], nlab, c, mdef, pnv, vcap);
        ka_release(mk2);
        n = *pn;
        c->nv = *pnv;
    }
}


/* ---------- пропуск нормализации 32-битных результатов (только x86-64; a64 игнорирует Ins.nn) ----------
 * (1) Результат 32-битной операции читают только те, кому нужны младшие 32 бита (операнды сложения/вычитания/умножения/логики/сдвига влево,
 *     значение store.4, копия в такой же vreg): верхняя половина регистра не нужна, movsxd/mov r32 не выдаётся. Это точно, без допущений.
 * (2) Счётчик цикла v = v +/- K (K мал, знаковый int): шаг не нормализуется, значение остаётся точным; переполнение знакового int не определено
 *     (clang тоже его исключает). -fwrapv отключает пункт 2. */
static void op_nn_use(Ins *i, int *r, int kind, void *x) {
    unsigned char *low = x;
    if (kind != 0 || *r <= 0) return;
    int ok = 0;
    if (i->op == IR_BIN && i->w == 4) {
        int sub = i->sub;
        if (sub == ND_ADD || sub == ND_SUB || sub == ND_MUL || sub == ND_AND || sub == ND_OR || sub == ND_XOR) ok = (r == &i->a || r == &i->b);
        else if (sub == ND_SHL) ok = (r == &i->a || r == &i->b);
        else if (sub == ND_SHR && !i->sgn) ok = (r == &i->a || r == &i->b);
    } else if (i->op == IR_STORE && i->w == 4) ok = (r == &i->b);
    else if (i->op == IR_MOV) ok = 1;                       /* уточняется ниже по цепочке копий */
    if (!ok) low[*r] = 0;
}
static void op_nonorm(Ins **a, int n, int nv, const unsigned char *mdef) {
    unsigned char *low = ka_new((size_t)nv + 2);
    memset(low, 1, (size_t)nv + 2);
    for (int k = 0; k < n; k++) op_walk(a[k], op_nn_use, low);
    for (int ch = 1, it = 0; ch && it < 64; it++) {          /* v = mov w: v читает младшие 32 бита только если их читает w */
        ch = 0;
        for (int k = 0; k < n; k++) {
            const Ins *i = a[k];
            if (i->op != IR_MOV || i->dst <= 0 || i->a <= 0 || i->dst > nv || i->a > nv) continue;
            if (!low[i->dst] && low[i->a]) { low[i->a] = 0; ch = 1; }
        }
    }
    for (int k = 0; k < n; k++) {
        Ins *i = a[k];
        int d = i->dst;
        if (d <= 0 || d > nv || i->w != 4) continue;
        if (i->op == IR_BIN && i->sgn && (i->sub == ND_ADD || i->sub == ND_SUB || i->sub == ND_MUL || i->sub == ND_AND || i->sub == ND_OR ||
                                          i->sub == ND_XOR || i->sub == ND_SHL)) {
            if (low[d]) i->nn = 1;
            else if (!kcc_wrapv && mdef[d] && d == i->a && (i->sub == ND_ADD || i->sub == ND_SUB)) {
                int cb = 0;
                for (int q = 0; q < n && !cb; q++) {      /* второй операнд - константа небольшой величины */
                    const Ins *c = a[q];
                    if (c->op == IR_CONST && c->dst == i->b && c->imm >= -(1 << 20) && c->imm <= (1 << 20)) cb = 1;
                }
                if (cb) i->nn = 1;
            }
        } else if ((i->op == IR_NEG || i->op == IR_NOT) && low[d]) i->nn = 1;
    }
}


/* ---------- локальная нумерация значений (внутри базового блока) ----------
 * Одинаковые чистые вычисления (константы, адреса, арифметика, сравнения, расширения) и загрузки по одному адресу заменяются копией первого результата,
 * пока ни один операнд и (для загрузок) память не менялись. Любая запись в память, вызов, asm и копирование блока выбрасывают все загрузки из таблицы;
 * volatile не участвует. Номера значений: у vreg на входе в блок номер равен самому vreg, каждое определение даёт новый номер. */
typedef struct { int op, sub, w, sgn, va, vb, vn, holder, load; int64_t imm; const char *sym; } OpLvnE;
#define OP_LVN_MAX 256
typedef struct { int *vn, *ep; int cur, next, nv; } OpLvn;
static int op_lvn_get(OpLvn *L, int v) {
    if (v <= 0 || v > L->nv) return v;
    if (L->ep[v] != L->cur) { L->ep[v] = L->cur; L->vn[v] = v; }
    return L->vn[v];
}
static void op_lvn_def(Ins *i, int *r, int kind, void *x) {
    (void)i;
    OpLvn *L = x;
    if (kind == 0 || *r <= 0 || *r > L->nv) return;
    L->ep[*r] = L->cur;
    L->vn[*r] = L->next++;
}
static int op_lvn(Ins **a, int n, int nv, int *cnt_out) {
    OpLvn L = { ka_new(sizeof(int) * (size_t)(nv + 2)), ka_new(sizeof(int) * (size_t)(nv + 2)), 1, nv + 2, nv };
    OpLvnE *tab = ka_new(sizeof(OpLvnE) * OP_LVN_MAX);
    int nent = 0, changed = 0;
    for (int k = 0; k < n; k++) {
        Ins *i = a[k];
        if (!i) continue;
        if (i->op == IR_LABEL) { L.cur++; nent = 0; continue; }
        int pure = 0, isload = 0;
        switch (i->op) {
        case IR_CONST: case IR_GADDR: case IR_LADDR: case IR_NEG: case IR_NOT: case IR_EXT: case IR_CMP: pure = 1; break;
        case IR_BIN: pure = i->sub != ND_DIV && i->sub != ND_MOD; break;
        case IR_LOAD: if (!i->vol) { pure = 1; isload = 1; } break;
        default: break;
        }
        int kill = 0, endblk = 0;
        if (i->op == IR_STORE || i->op == IR_CALL || i->op == IR_ASM || i->op == IR_COPY || i->op == IR_ZERO) kill = 1;
        if (i->op == IR_JMP || i->op == IR_BRZ || i->op == IR_BRNZ || i->op == IR_RET) endblk = 1;
        int newvn = -1;
        if (i->op == IR_MOV && i->dst > 0 && i->a > 0) newvn = op_lvn_get(&L, i->a);
        int hit = 0, key_found = -1;
        OpLvnE key;
        if (pure && i->dst > 0 && i->dst <= nv) {
            memset(&key, 0, sizeof key);
            key.op = i->op; key.sub = i->sub; key.w = i->w; key.sgn = i->sgn; key.imm = i->imm; key.sym = i->sym; key.load = isload;
            if (i->op == IR_LOAD || i->op == IR_NEG || i->op == IR_NOT || i->op == IR_EXT) key.va = op_lvn_get(&L, i->a);
            else if (i->op == IR_BIN || i->op == IR_CMP) {
                key.va = op_lvn_get(&L, i->a); key.vb = op_lvn_get(&L, i->b);
                int comm = i->op == IR_BIN && (i->sub == ND_ADD || i->sub == ND_MUL || i->sub == ND_AND || i->sub == ND_OR || i->sub == ND_XOR);
                if (comm && key.va > key.vb) { int t = key.va; key.va = key.vb; key.vb = t; }
            }
            if (i->op == IR_LADDR || i->op == IR_CONST || i->op == IR_GADDR) { key.va = 0; key.vb = 0; }
            for (int q = 0; q < nent; q++) {
                const OpLvnE *e = &tab[q];
                if (e->op != key.op || e->sub != key.sub || e->w != key.w || e->sgn != key.sgn || e->va != key.va || e->vb != key.vb || e->imm != key.imm ||
                    e->load != key.load) continue;
                if ((e->sym != key.sym) && (!e->sym || !key.sym || strcmp(e->sym, key.sym))) continue;
                if (op_lvn_get(&L, e->holder) != e->vn) continue;
                key_found = q;
                break;
            }
            if (key_found >= 0 && tab[key_found].holder != i->dst) hit = 1;
        }
        if (hit) {
            int h = tab[key_found].holder, v = tab[key_found].vn;
            i->op = IR_MOV; i->a = h; i->b = 0; i->sub = 0; i->imm = 0; i->sym = NULL; i->nn = 0;
            op_walk(i, op_lvn_def, &L);
            L.vn[i->dst] = v;
            changed++;
        } else {
            if (kill) { int m = 0; for (int q = 0; q < nent; q++) if (!tab[q].load) tab[m++] = tab[q]; nent = m; }
            op_walk(i, op_lvn_def, &L);
            if (newvn >= 0 && i->dst > 0 && i->dst <= nv) L.vn[i->dst] = newvn;
            else if (pure && i->dst > 0 && i->dst <= nv && key_found < 0) {
                key.vn = L.vn[i->dst]; key.holder = i->dst;
                if (nent == OP_LVN_MAX) { for (int q = 1; q < nent; q++) tab[q - 1] = tab[q]; nent--; }
                tab[nent++] = key;
            }
        }
        if (endblk) { L.cur++; nent = 0; }
    }
    if (cnt_out) *cnt_out = changed;
    return changed;
}

/* Распознавание циклического сдвига: d = (x >> c1) | (x << c2), c1 + c2 равно ширине (4 или 8 байт), оба сдвига без знака, x и
 * промежуточные значения - vreg с одним определением. Результат: d = rol x, c2. Только для x86-64 (у AArch64 свой ror). */
static void op_rotates(Ins **a, int n, const OpCnt *c, const unsigned char *md) {
#define SINGLE(v) ((v) > 0 && (v) <= c->nv && c->ndef[v] == 1 && !md[v] && c->defins[v])
    if (K.cfg && K.cfg->target && !strcmp(K.cfg->target, "aarch64")) return;
    for (int k = 0; k < n; k++) {
        Ins *o = a[k];
        if (o->op != IR_BIN || o->sub != ND_OR || o->sgn || (o->w != 4 && o->w != 8)) continue;
        if (!SINGLE(o->a) || !SINGLE(o->b) || c->nuse[o->a] != 1 || c->nuse[o->b] != 1 || o->a == o->b) continue;
        Ins *p = c->defins[o->a], *q = c->defins[o->b];
        if (p->op != IR_BIN || q->op != IR_BIN || p->w != o->w || q->w != o->w || p->sgn || q->sgn) continue;
        if (p->sub == ND_SHL && q->sub == ND_SHR) { Ins *t = p; p = q; q = t; }       /* p - сдвиг вправо, q - влево */
        if (p->sub != ND_SHR || q->sub != ND_SHL || p->a != q->a || !SINGLE(p->a)) continue;
        if (!SINGLE(p->b) || !SINGLE(q->b)) continue;
        const Ins *d1 = c->defins[p->b], *d2 = c->defins[q->b];
        if (d1->op != IR_CONST || d2->op != IR_CONST) continue;
        int64_t c1 = d1->imm, c2 = d2->imm;
        if (c1 <= 0 || c2 <= 0 || c1 + c2 != 8 * o->w) continue;
        o->sub = ND_ROL; o->a = p->a; o->b = q->b;
    }
#undef SINGLE
}


/* ---------- инверсия циклов ----------
 * L: H (условие, BRZ/BRNZ на выход); тело; JMP L   =>   L: H; Lb: тело; H'; обратный условный переход на Lb; JMP выход.
 * H' пишет во вновь созданные vreg для временных значений, нужных только заголовку; значение, которое читает и тело, остаётся одним
 * vreg с пометкой "несколько определений". Переходы на L из тела (continue) остаются. Константы и адреса не копируются: их
 * определение в H выполняется при входе в цикл и дальше неизменно. */
typedef struct { int *rd; int nv; } OpHdrCnt;
static void op_hdrcnt(Ins *i, int *r, int kind, void *x) { (void)i; OpHdrCnt *h = x; if (kind == 0 && *r > 0 && *r <= h->nv) h->rd[*r]++; }
typedef struct { const int *map; int nv; } OpHdrMap;
static void op_hdrmap(Ins *i, int *r, int kind, void *x) { (void)i; OpHdrMap *h = x; if (kind == 0 && *r > 0 && *r <= h->nv && h->map[*r]) *r = h->map[*r]; }
#define OP_HDR_MAX 12
static Ins *op_pool; static size_t op_pool_left;   /* узлы инверсии живут дольше временной арены ir_optimize: выделяются заранее */
static Ins *op_newins(void) { if (!op_pool_left) return NULL; op_pool_left--; return op_pool++; }
static Ins **op_invert(IrFn *fi, Ins **a, int *pn, int *pnv, unsigned char *mdef, int vcap, const OpCnt *c) {
    int n = *pn, nv = *pnv, cand = 0;
    for (int k = 0; k < n; k++) if (a[k]->op == IR_JMP) cand++;
    if (!cand) return a;
    size_t cap = (size_t)n + (size_t)cand * (OP_HDR_MAX + 3) + 4;
    Ins **b = ka_new(sizeof(Ins *) * cap);
    int *map = ka_new(sizeof(int) * (size_t)(c->nv + 2));
    int *rd = ka_new(sizeof(int) * (size_t)(c->nv + 2));
    for (int again = 1; again;) {
        again = 0;
        for (int ek = 0; ek < n && !again; ek++) {
            Ins *j = a[ek];
            if (j->op != IR_JMP || j->imm < 0 || j->imm > fi->nlab) continue;
            int l = -1;
            for (int q = 0; q < ek; q++) if (a[q]->op == IR_LABEL && a[q]->imm == j->imm) { l = q; break; }
            if (l < 0) continue;
            int h = -1, ok = 1;
            for (int q = l + 1; q < ek && q <= l + OP_HDR_MAX; q++) {
                const Ins *x = a[q];
                if (x->op == IR_BRZ || x->op == IR_BRNZ) { h = q; break; }
                if (!(x->op == IR_CONST || x->op == IR_GADDR || x->op == IR_LADDR || x->op == IR_MOV || x->op == IR_BIN || x->op == IR_NEG ||
                      x->op == IR_NOT || x->op == IR_EXT || x->op == IR_CMP || (x->op == IR_LOAD && !x->vol))) { ok = 0; break; }
            }
            if (!ok || h < 0 || h + 1 >= ek || a[h]->imm < 0 || a[h]->imm > fi->nlab) continue;
            int tp = -1;
            for (int q = ek + 1; q < n; q++) if (a[q]->op == IR_LABEL && a[q]->imm == a[h]->imm) { tp = q; break; }
            if (tp < 0) continue;                         /* выход должен быть строго после цикла */
            for (int q = l + 1; q < h; q++) if (a[q]->op == IR_LABEL) ok = 0;
            if (!ok || nv + (h - l) + 2 > vcap) continue;
            memset(map, 0, sizeof(int) * (size_t)(c->nv + 2));
            memset(rd, 0, sizeof(int) * (size_t)(c->nv + 2));
            OpHdrCnt hc = { rd, c->nv };
            for (int q = l + 1; q <= h; q++) op_walk(a[q], op_hdrcnt, &hc);
            Ins **cp = ka_new(sizeof(Ins *) * (size_t)(OP_HDR_MAX + 2)); int ncp = 0;
            for (int q = l + 1; q < h; q++) {
                const Ins *x = a[q];
                if (x->op == IR_CONST || x->op == IR_GADDR || x->op == IR_LADDR) continue;
                Ins *y = op_newins();
                if (!y) { ok = 0; break; }
                *y = *x; y->next = NULL;
                OpHdrMap hm = { map, c->nv };
                op_walk(y, op_hdrmap, &hm);
                int d = x->dst;
                if (d > 0 && d <= c->nv && !mdef[d]) {
                    if (c->nuse[d] == rd[d] && c->ndef[d] == 1) { map[d] = ++nv; y->dst = nv; mdef[nv] = 0; }
                    else mdef[d] = 1;                      /* значение нужно и телу: теперь два определения */
                }
                cp[ncp++] = y;
            }
            if (!ok || op_pool_left < 3) continue;
            Ins *br = op_newins();
            *br = *a[h]; br->next = NULL;
            { OpHdrMap hm = { map, c->nv }; op_walk(br, op_hdrmap, &hm); }
            int newlab = ++fi->nlab;
            br->op = a[h]->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
            br->imm = newlab;
            Ins *lb = op_newins(); lb->op = IR_LABEL; lb->imm = newlab; lb->line = a[h]->line; lb->file = a[h]->file;
            Ins *jx = op_newins(); *jx = *j; jx->imm = a[h]->imm; jx->next = NULL;
            int tot = 0;
            for (int q = 0; q <= h; q++) b[tot++] = a[q];
            b[tot++] = lb;
            for (int q = h + 1; q < ek; q++) b[tot++] = a[q];
            for (int q = 0; q < ncp; q++) b[tot++] = cp[q];
            b[tot++] = br;
            b[tot++] = jx;
            for (int q = ek + 1; q < n; q++) b[tot++] = a[q];
            Ins **t = a; a = b; b = t;
            n = tot;
            again = 1;
        }
    }
    *pn = n; *pnv = nv;
    return a;
}

void ir_optimize(IrFn *fi) {
    int nv = fi->nreg, n = 0;
    for (Ins *i = fi->head; i; i = i->next) n++;
    if (n == 0) return;
    int maxslot0 = -1;
    for (Obj *l = fi->f->locals; l; l = l->next) if (l->slot > maxslot0) maxslot0 = l->slot;
    /* mdef живёт дольше прохода (читают бэкенды): размер с запасом под новые vreg переменных (не больше числа слотов) */
    int mdef_cap = nv + maxslot0 + 1 + 2 + n;
    unsigned char *mdef = ka_new((size_t)mdef_cap + 1);
    size_t vslack = 0;
    {   /* пул узлов для инверсии циклов: число обратных безусловных переходов (переход на уже встреченную метку) */
        unsigned char *seen = ka_new((size_t)fi->nlab + 2);
        size_t loops = 0;
        for (Ins *i = fi->head; i; i = i->next) {
            if (i->op == IR_LABEL && i->imm >= 0 && i->imm <= fi->nlab) seen[i->imm] = 1;
            else if (i->op == IR_JMP && i->imm >= 0 && i->imm <= fi->nlab && seen[i->imm]) loops++;
        }
        size_t nmul = 0;
        int loops_possible = loops > 0;
        for (Ins *i = fi->head; i; i = i->next) if (loops_possible && i->op == IR_BIN && i->sub == ND_MUL && i->w == 8) nmul++;
        op_pool_left = loops * (OP_HDR_MAX + 3) + nmul * 4;
        vslack = loops * (OP_HDR_MAX + 2) + nmul * 3 + 8;
        op_pool = op_pool_left ? ka_new(sizeof(Ins) * op_pool_left) : NULL;
    }
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
    OpCnt c2 = { nvt, ka_new(sizeof(int) * (size_t)(nvt + 2 + (int)vslack)), ka_new(sizeof(int) * (size_t)(nvt + 2 + (int)vslack)), ka_new(sizeof(Ins *) * (size_t)(nvt + 2 + (int)vslack)) };
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
        int ch = 0;
        if (!has_setjmp && !kcc_nolvn) { int lc = 0; size_t lm = ka_mark(); op_lvn(a, n, nvt, &lc); ka_release(lm); if (lc) ch = 1; }
        op_recount(&c2, a, n);
        op_ssa_refine(a, n, nvt, mdef, c2.ndef, fi->nlab);
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

    op_recount(&c2, a, n);
    op_rotates(a, n, &c2, mdef);

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
    op_recount(&c2, a, n);
    if (!has_setjmp && !kcc_noopt) {
        op_licm(a, n, fi->nlab, &c2, mdef, nvt);
        a = op_invert(fi, a, &n, &nvt, mdef, mdef_cap, &c2);
        c2.nv = nvt;
        op_recount(&c2, a, n);
    }
    /* слияние "t = op ...; v = t" в "v = op ...", если t живёт ровно между ними */
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
    if (!has_setjmp && !kcc_noopt && !kcc_nosr) {
        c2.nv = nvt;
        op_sr(a, &n, fi->nlab, &c2, mdef, &nvt, mdef_cap);
        for (int k = 0; k + 1 < n; k++)                       /* повтор подряд идущего одинакового определения константы */
            if (a[k]->op == IR_CONST && a[k + 1]->op == IR_CONST && a[k]->dst == a[k + 1]->dst && a[k]->imm == a[k + 1]->imm) a[k] = NULL;
        { int m = 0; for (int k = 0; k < n; k++) if (a[k]) a[m++] = a[k]; n = m; }
    }
    if (!kcc_noopt) op_nonorm(a, n, nvt, mdef);
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
