/* kcc: кодогенерация x86-64 из линейного IR (System V ABI для вызовов).
 * Каждая функция: живые интервалы vreg -> линейный проход по пяти callee-saved регистрам (rbx, r12-r15), остальное - в стековые
 * слоты. Рабочие регистры (rax, rcx, rdx, rsi, rdi, r8-r11) свободны для кодогенератора и ничего не хранят между инструкциями IR.
 * Достижимость: в образ попадают main, функции и данные с внешней видимостью и всё, на что они ссылаются; статические
 * неиспользуемые функции отбрасываются (как --gc-sections). Неопределённый символ - ошибка. */
#include "x86.h"
#include "target.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static XA xa;
#define A (&xa)

/* ---------- состояние функции ---------- */
/* Пул vreg: пять callee-saved (сохраняются в прологе) и три caller-saved (r8-r10: бесплатны, но только для значений,
   живущих не через вызов, atomic, asm, копирование памяти и подобные инструкции, которые их используют). */
#define NCS 5
#define NPOOL 8
static const int pool[NPOOL] = { R_BX, R_12, R_13, R_14, R_15, R_8, R_9, R_10 };
static const int argreg[6] = { R_DI, R_SI, R_DX, R_CX, R_8, R_9 };
/* ---------- ABI System V: размещение аргументов ---------- */
enum { AL_REG, AL_STK, AL_AREG, AL_ASTK };
typedef struct { int kind, idx, off; } ALoc;        /* REG: номер регистра; AREG: первый из регистров; STK/ASTK: смещение в области аргументов */
static int plan_args(int n, const int *asz, int hidden, ALoc *loc, int *ngrn) {
    int g = hidden ? 1 : 0, sp = 0;
    for (int k = 0; k < n; k++) {
        int z = asz ? asz[k] : 0;
        if (!z) {
            if (g < 6) { loc[k].kind = AL_REG; loc[k].idx = g++; }
            else { loc[k].kind = AL_STK; loc[k].off = sp; sp += 8; }
        } else {
            int nr = (z + 7) / 8;
            if (z <= 16 && g + nr <= 6) { loc[k].kind = AL_AREG; loc[k].idx = g; g += nr; }
            else { loc[k].kind = AL_ASTK; loc[k].off = sp; sp += (z + 7) & ~7; }
        }
    }
    *ngrn = g;
    return sp;
}
static ALoc *ploc;                             /* размещение параметров текущей функции */
static int p_ngrn, p_stk, p_hidden;            /* регистров занято именованными, байт стека занято именованными, скрытый указатель результата */

static int *vs, *ve, *rg, *so;               /* интервал, регистр (-1 нет), номер слота спила */
static int *slotoff;                          /* смещение локального слота относительно rbp (вниз) */
static int spillbase, epi;
/* Оптимизации бэкенда: константный vreg (одно определение IR_CONST) не занимает регистр и подставляется непосредственно;
   сравнение, единственный читатель результата которого - следующий IR_BRZ/IR_BRNZ, сливается с переходом. */
static int64_t *cv;
static unsigned char *isc, *fusecmp;

typedef struct { long *wt; long w; int nv; } WtCtx;
static void wt_visit(Ins *i, int *r, int kind, void *c) {
    WtCtx *o = c;
    (void)i; (void)kind;
    if (*r > 0 && *r <= o->nv) o->wt[*r] += o->w;
}
static int cmp_start(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (vs[x] != vs[y]) return vs[x] < vs[y] ? -1 : 1;
    return x - y;
}

/* ---------- примитивы ---------- */
static Opd R64(int r) { return xr(8, r); }
static Opd spill(int v) { return xm(8, R_BP, -(int64_t)(spillbase + 8 * (so[v] + 1))); }

static void mov_imm_r(int r, int64_t v);
static void ldv(int r, int v) {
    if (v <= 0) kerr(A->file, A->line, "internal: use of an undefined vreg");
    if (isc[v]) { mov_imm_r(r, cv[v]); return; }
    if (rg[v] >= 0) { if (rg[v] != r) xa_ins(A, X_MOV, R64(r), R64(rg[v])); }
    else xa_ins(A, X_MOV, R64(r), spill(v));
}
static void stv(int v, int r) {
    if (v <= 0) return;
    if (rg[v] >= 0) { if (rg[v] != r) xa_ins(A, X_MOV, R64(rg[v]), R64(r)); }
    else xa_ins(A, X_MOV, spill(v), R64(r));
}

/* копирование n байт между [sb+so] и [db+do] через rax (размеры 8, 4, 2, 1) */
static void copy_bytes(int db, int64_t dof, int sb, int64_t sof, int n) {
    int o = 0;
    while (n - o > 0) {
        int p = n - o >= 8 ? 8 : n - o >= 4 ? 4 : n - o >= 2 ? 2 : 1;
        xa_ins(A, X_MOV, xr(p, R_AX), xm(p, sb, sof + o));
        xa_ins(A, X_MOV, xm(p, db, dof + o), xr(p, R_AX));
        o += p;
    }
}
/* загрузка n (1..8) байт [base+off] в регистр без обращения за пределы объекта */
static void ld_reg_bytes(int reg, int base, int64_t off, int n) {
    if (n == 8) { xa_ins(A, X_MOV, R64(reg), xm(8, base, off)); return; }
    if (n == 4) { xa_ins(A, X_MOV, xr(4, reg), xm(4, base, off)); return; }
    if (n == 2 || n == 1) { xa_ins(A, X_MOVZX, xr(4, reg), xm(n, base, off)); return; }
    int sc = reg == R_AX ? R_CX : R_AX, rem = n;
    xa_ins(A, X_XOR, xr(4, reg), xr(4, reg));
    while (rem > 0) {
        int p = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
        if (p == 4) xa_ins(A, X_MOV, xr(4, sc), xm(4, base, off + rem - p));
        else xa_ins(A, X_MOVZX, xr(4, sc), xm(p, base, off + rem - p));
        xa_ins(A, X_SHL, R64(reg), xi(8 * p));
        xa_ins(A, X_OR, R64(reg), R64(sc));
        rem -= p;
    }
}

/* приводит rax к каноническому виду целого шириной w байт */
static void normr(int r, int w, int sgn) {
    if (w == 4) { if (sgn) xa_ins(A, X_MOVSX, R64(r), xr(4, r)); else xa_ins(A, X_MOV, xr(4, r), xr(4, r)); }
    else if (w == 2 || w == 1) xa_ins(A, sgn ? X_MOVSX : X_MOVZX, R64(r), xr(w, r));
}
static int imm32_of(int v, int64_t *out) {
    if (v > 0 && isc[v] && cv[v] >= INT32_MIN && cv[v] <= INT32_MAX) { *out = cv[v]; return 1; }
    return 0;
}
static int log2_exact(int64_t v) {
    if (v <= 0 || (v & (v - 1))) return -1;
    int k = 0;
    while (!(v & 1)) { v >>= 1; k++; }
    return k;
}
static void norm(int w, int sgn) {
    if (w == 4) { if (sgn) xa_ins(A, X_MOVSX, R64(R_AX), xr(4, R_AX)); else xa_ins(A, X_MOV, xr(4, R_AX), xr(4, R_AX)); }
    else if (w == 2 || w == 1) xa_ins(A, sgn ? X_MOVSX : X_MOVZX, R64(R_AX), xr(w, R_AX));
}

static void mov_imm_r(int r, int64_t v) {
    if (v >= 0 && v <= (int64_t)UINT32_MAX) xa_ins(A, X_MOV, xr(4, r), xi(v));
    else xa_ins(A, X_MOV, R64(r), xi(v));
}
static void mov_imm(int r, int64_t v) {
    if (v >= 0 && v <= (int64_t)UINT32_MAX) xa_ins(A, X_MOV, xr(4, r), xi(v));
    else xa_ins(A, X_MOV, R64(r), xi(v));
}

static void gen_bin(const Ins *i) {
    int sub = i->sub;
    if (sub == ND_DIV || sub == ND_MOD) {
        ldv(R_AX, i->a);
        ldv(R_CX, i->b);
        if (i->sgn) { xa_op0(A, X0_CQO); xa_ins(A, X_IDIV, R64(R_CX), NOOPD); }
        else { xa_ins(A, X_XOR, xr(4, R_DX), xr(4, R_DX)); xa_ins(A, X_DIV, R64(R_CX), NOOPD); }
        if (sub == ND_MOD) xa_ins(A, X_MOV, R64(R_AX), R64(R_DX));
        norm(i->w, i->sgn);
        stv(i->dst, R_AX);
        return;
    }
    int a = i->a, b = i->b;
    int comm = sub == ND_ADD || sub == ND_MUL || sub == ND_AND || sub == ND_OR || sub == ND_XOR;
    int64_t imm = 0;
    int sz4 = i->w == 4 && !i->sgn;                  /* беззнаковые 32 бита: 32-битные команды сами обнуляют верх, normr не нужен */
    #define RS(r) xr(sz4 ? 4 : 8, (r))
    int bimm = imm32_of(b, &imm);
    if (sz4 && !bimm && b > 0 && isc[b] && cv[b] >= 0 && cv[b] <= (int64_t)UINT32_MAX) { imm = (int32_t)(uint32_t)cv[b]; bimm = 1; }
    if (!bimm && comm) {
        if (imm32_of(a, &imm)) { int t = a; a = b; b = t; bimm = 1; }
        else if (sz4 && a > 0 && isc[a] && cv[a] >= 0 && cv[a] <= (int64_t)UINT32_MAX) { imm = (int32_t)(uint32_t)cv[a]; int t = a; a = b; b = t; bimm = 1; }
    }
    int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
    if (sub == ND_ROL) {                              /* константный сдвиг: ширина 4 беззнаковая либо 8 (opt.c других не создаёт) */
        ldv(d, a);
        int bits = sz4 ? 32 : 64;
        if (bimm && (imm & (bits - 1))) xa_ins(A, X_ROL, RS(d), xi(imm & (bits - 1)));
        else if (!bimm) kerr(A->file, A->line, "internal: rol by a non-constant");
    } else if (sub == ND_SHL || sub == ND_SHR) {
        if (!bimm) ldv(R_CX, b);                      /* счётчик в CL до загрузки значения: d может совпадать с регистром b */
        ldv(d, a);
        int op = sub == ND_SHL ? X_SHL : i->sgn ? X_SAR : X_SHR;
        if (bimm) { if (imm & (sz4 ? 31 : 63)) xa_ins(A, op, RS(d), xi(imm & (sz4 ? 31 : 63))); }
        else xa_ins(A, op, RS(d), xr(1, R_CX));
    } else {
        int bm = bimm ? -1 : rg[b];
        if (!bimm && bm >= 0 && bm == d && a != b) {
            if (comm) { int t = a; a = b; b = t; bm = rg[b]; }
            else { d = R_AX; }
        }
        int op = sub == ND_ADD ? X_ADD : sub == ND_SUB ? X_SUB : sub == ND_MUL ? X_IMUL : sub == ND_AND ? X_AND : sub == ND_OR ? X_OR : X_XOR;
        if (sub != ND_ADD && sub != ND_SUB && sub != ND_MUL && sub != ND_AND && sub != ND_OR && sub != ND_XOR)
            kerr(A->file, A->line, "internal: bad binary operator %d", sub);
        ldv(d, a);
        if (sub == ND_MUL && bimm) {
            int k = log2_exact(imm);
            if (k == 0) { }
            else if (k > 0) xa_ins(A, X_SHL, RS(d), xi(k));
            else xa_imul3(A, RS(d), RS(d), imm);
        } else if (bimm) xa_ins(A, op, RS(d), xi(imm));
        else if (bm >= 0) xa_ins(A, op, RS(d), RS(bm));
        else { ldv(R_CX, b); xa_ins(A, op, RS(d), RS(R_CX)); }
    }
    if (!sz4) normr(d, i->w, i->sgn);
    #undef RS
    if (rg[i->dst] != d) stv(i->dst, d);
}


/* Загрузка/запись в память. base < 0: адрес берётся из vreg i->a (в rax, если вытеснен); иначе адрес задан регистрами [base + ix*2^sc + disp] (слитый расчёт адреса) */
static void gen_ldst(const Ins *i, int base, int ix, int sc, int64_t disp) {
    if (i->w != 1 && i->w != 2 && i->w != 4 && i->w != 8) kerr(A->file, A->line, "x86-64 backend: unsupported %s width %d", i->op == IR_LOAD ? "load" : "store", i->w);
    if (base < 0) base = rg[i->a] >= 0 ? rg[i->a] : (ldv(R_AX, i->a), R_AX);
    #define MEM(w) (ix < 0 ? xm((w), base, disp) : xmi((w), base, ix, sc, disp))
    if (i->op == IR_LOAD) {
        int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
        if (i->w == 8) xa_ins(A, X_MOV, R64(d), MEM(8));
        else if (i->w == 4) { if (i->sgn) xa_ins(A, X_MOVSX, R64(d), MEM(4)); else xa_ins(A, X_MOV, xr(4, d), MEM(4)); }
        else xa_ins(A, i->sgn ? X_MOVSX : X_MOVZX, R64(d), MEM(i->w));
        if (rg[i->dst] != d) stv(i->dst, d);
    } else {
        int64_t imm;
        if (imm32_of(i->b, &imm)) xa_ins(A, X_MOV, MEM(i->w), xi(imm));
        else {
            int src = rg[i->b] >= 0 ? rg[i->b] : (ldv(R_CX, i->b), R_CX);
            xa_ins(A, X_MOV, MEM(i->w), xr(i->w, src));
        }
    }
    #undef MEM
}

static int *g_nd, *g_nu;                       /* числа определений/чтений vreg текущей функции */
static const unsigned char *g_mdef;
static int fuse_tmp(int v) { return v > 0 && g_nd[v] == 1 && g_nu[v] == 1 && !(g_mdef && g_mdef[v]) && !isc[v]; }
/* Слияние расчёта адреса с обращением к памяти: [SHL/MUL(z, 2^s)]; ADD x, y; LOAD/STORE. Возвращает число поглощённых инструкций (0 - не подошло). */
static int try_addr_fuse(Ins **ins, int k, int n) {
    const Ins *p = ins[k];
    if (k + 1 < n && p->op == IR_BIN && p->sub == ND_ADD && p->w == 8 && fuse_tmp(p->dst)) {
        const Ins *m = ins[k + 1];
        if ((m->op == IR_LOAD || m->op == IR_STORE) && m->a == p->dst && !(m->op == IR_STORE && m->b == p->dst)) {
            int64_t imm;
            if (rg[p->a] >= 0 && !isc[p->a] && imm32_of(p->b, &imm)) { gen_ldst(m, rg[p->a], -1, 0, imm); return 2; }
            if (rg[p->a] >= 0 && rg[p->b] >= 0 && !isc[p->a] && !isc[p->b] && rg[p->b] != R_SP) { gen_ldst(m, rg[p->a], rg[p->b], 0, 0); return 2; }
        }
    }
    if (k + 2 < n && p->op == IR_BIN && p->w == 8 && fuse_tmp(p->dst) && (p->sub == ND_SHL || p->sub == ND_MUL)) {
        const Ins *q = ins[k + 1], *m = ins[k + 2];
        int64_t c;
        if (!imm32_of(p->b, &c) || p->a <= 0 || isc[p->a] || rg[p->a] < 0) return 0;
        int sc = -1;
        if (p->sub == ND_SHL && c >= 1 && c <= 3) sc = (int)c;
        else if (p->sub == ND_MUL) sc = c == 2 ? 1 : c == 4 ? 2 : c == 8 ? 3 : -1;
        if (sc < 0) return 0;
        if (q->op != IR_BIN || q->sub != ND_ADD || q->w != 8 || !fuse_tmp(q->dst)) return 0;
        if (!(m->op == IR_LOAD || m->op == IR_STORE) || m->a != q->dst || (m->op == IR_STORE && m->b == q->dst)) return 0;
        int x = q->a == p->dst ? q->b : q->b == p->dst ? q->a : 0;
        if (x <= 0 || x == p->dst || isc[x] || rg[x] < 0) return 0;
        gen_ldst(m, rg[x], rg[p->a], sc, 0);
        return 3;
    }
    return 0;
}

/* Сравнение: a в регистр (свой или rax), b непосредственно, в своём регистре или через rcx. Возвращает код условия. */
static int cmp_cc(const Ins *i) {
    int a = i->a, b = i->b;
    int ra;
    if (rg[a] >= 0) ra = rg[a]; else { ldv(R_AX, a); ra = R_AX; }
    int64_t imm;
    if (imm32_of(b, &imm)) xa_ins(A, X_CMP, R64(ra), xi(imm));
    else if (rg[b] >= 0) xa_ins(A, X_CMP, R64(ra), R64(rg[b]));
    else { ldv(R_CX, b); xa_ins(A, X_CMP, R64(ra), R64(R_CX)); }
    switch (i->sub) {
    case ND_EQ: return CC_E;
    case ND_NE: return CC_NE;
    case ND_LT: return i->sgn ? CC_L : CC_B;
    case ND_LE: return i->sgn ? CC_LE : CC_BE;
    default: kerr(A->file, A->line, "internal: bad comparison %d", i->sub); return CC_E;
    }
}
static void gen_cmp(const Ins *i) {
    int cc = cmp_cc(i);
    int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
    xa_setcc(A, cc, xr(1, d));
    xa_ins(A, X_MOVZX, xr(4, d), xr(1, d));
    if (rg[i->dst] != d) stv(i->dst, d);
}
/* cmp + brz/brnz: условный переход напрямую по флагам. Код x86 с младшим битом 1 - обратное условие. */
static void gen_cmpbr(const Ins *c, const Ins *br) {
    int cc = cmp_cc(c);
    xa_jmp(A, br->op == IR_BRZ ? (cc ^ 1) : cc, (int)br->imm);
}

static void gen_call(const Ins *i) {
    ALoc *loc = ka_new(sizeof(ALoc) * (size_t)(i->nargs + 1));
    int ng;
    plan_args(i->nargs, i->asz, i->rsz > 16, loc, &ng);
    if (!i->sym) ldv(R_10, i->a);
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_STK) { ldv(R_AX, i->args[k]); xa_ins(A, X_MOV, xm(8, R_SP, loc[k].off), R64(R_AX)); }
        else if (loc[k].kind == AL_ASTK) { ldv(R_11, i->args[k]); copy_bytes(R_SP, loc[k].off, R_11, 0, i->asz[k]); }
    }
    if (i->rsz > 16) ldv(R_DI, i->rt);
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_REG) ldv(argreg[loc[k].idx], i->args[k]);
        else if (loc[k].kind == AL_AREG) {
            int z = i->asz[k];
            ldv(R_11, i->args[k]);
            ld_reg_bytes(argreg[loc[k].idx], R_11, 0, z > 8 ? 8 : z);
            if (z > 8) ld_reg_bytes(argreg[loc[k].idx + 1], R_11, 8, z - 8);
        }
    }
    if (i->nout) xa_ins(A, X_MOV, xr(4, R_AX), xi(0));        /* вариадический вызов: al = число векторных регистров */
    if (i->sym) {
        Obj *t = gen_lookup(i->sym);
        if (!t) kerr(A->file, A->line, "undefined reference to '%s'", i->sym);
        xa_call(A, t);
    } else xa_ins(A, X_CALLR, R64(R_10), NOOPD);
    if (i->rsz && i->rsz <= 16) {                          /* результат в rax:rdx; буфер дополнен до 16 байт (ir.c) */
        ldv(R_CX, i->rt);
        xa_ins(A, X_MOV, xm(8, R_CX, 0), R64(R_AX));
        if (i->rsz > 8) xa_ins(A, X_MOV, xm(8, R_CX, 8), R64(R_DX));
    }
    if (i->dst > 0) { norm(i->w, i->sgn); stv(i->dst, R_AX); }
}

/* ---------- атомарные операции (seq_cst) ---------- */
static int tmplab;
static void gen_atomic(const Ins *i) {
    int w = i->w;
    if (i->sub == AT_FENCE) { xa_op0(A, X0_MFENCE); return; }
    ldv(R_DX, i->a);
    if (i->sub == AT_LOAD) {
        if (w == 8) xa_ins(A, X_MOV, R64(R_AX), xm(8, R_DX, 0));
        else if (w == 4) xa_ins(A, X_MOV, xr(4, R_AX), xm(4, R_DX, 0));
        else xa_ins(A, X_MOVZX, xr(4, R_AX), xm(w, R_DX, 0));
        norm(w, i->sgn); stv(i->dst, R_AX);
        return;
    }
    if (i->sub == AT_CAS) {
        ldv(R_AX, i->b); ldv(R_CX, i->c);
        xa_ins(A, X_LCMPXCHG, xm(w, R_DX, 0), xr(w, R_CX));
        norm(w, i->sgn); stv(i->dst, R_AX);
        return;
    }
    ldv(R_CX, i->b);
    switch (i->sub) {
    case AT_STORE: xa_ins(A, X_XCHG, xm(w, R_DX, 0), xr(w, R_CX)); return;
    case AT_XCHG: xa_ins(A, X_XCHG, xm(w, R_DX, 0), xr(w, R_CX)); xa_ins(A, X_MOV, R64(R_AX), R64(R_CX)); break;
    case AT_ADD: case AT_SUB:
        if (i->sub == AT_SUB) xa_ins(A, X_NEG, R64(R_CX), NOOPD);
        xa_ins(A, X_LXADD, xm(w, R_DX, 0), xr(w, R_CX));
        xa_ins(A, X_MOV, R64(R_AX), R64(R_CX));
        break;
    default: {                                      /* and, or, xor, nand: цикл по cmpxchg */
        int top = tmplab++;
        if (w == 8) xa_ins(A, X_MOV, R64(R_AX), xm(8, R_DX, 0));
        else if (w == 4) xa_ins(A, X_MOV, xr(4, R_AX), xm(4, R_DX, 0));
        else xa_ins(A, X_MOVZX, xr(4, R_AX), xm(w, R_DX, 0));
        xa_label(A, top);
        xa_ins(A, X_MOV, R64(R_8), R64(R_AX));
        xa_ins(A, i->sub == AT_OR ? X_OR : i->sub == AT_XOR ? X_XOR : X_AND, R64(R_8), R64(R_CX));
        if (i->sub == AT_NAND) xa_ins(A, X_NOT, R64(R_8), NOOPD);
        xa_ins(A, X_LCMPXCHG, xm(w, R_DX, 0), xr(w, R_8));
        xa_jmp(A, CC_NE, top);
        break;
    }
    }
    norm(w, i->sgn); stv(i->dst, R_AX);
}

static const struct { const char *t, *in, *out, *hex; } xasm_tab[] = {
    { "cli; hlt", "", "", "FA F4" },
    { "sti; hlt", "", "", "FB F4" },
    { "wrmsr", "cad", "", "0F 30" },
    { "rdmsr", "c", "ad", "0F 32" },
    { "cpuid", "ac", "abcd", "0F A2" },
    { "mov %0, %%cr0", "r", "", "0F 22 C0" },
    { "mov %0, %%cr3", "r", "", "0F 22 D8" },
    { "mov %0, %%cr4", "r", "", "0F 22 E0" },
    { "mov %%cr0, %0", "", "r", "0F 20 C0" },
    { "mov %%cr2, %0", "", "r", "0F 20 D0" },
    { "mov %%cr3, %0", "", "r", "0F 20 D8" },
    { "mov %%cr4, %0", "", "r", "0F 20 E0" },
    { "mov %%cr3, %0; mov %0, %%cr3", "", "r", "0F 20 D8 0F 22 D8" },
    { "ltr %0", "r", "", "0F 00 D8" },
    { "invlpg (%0)", "r", "", "0F 01 38" },
    { "clts", "", "", "0F 06" },
    { "fninit", "", "", "DB E3" },
    { "ud2", "", "", "0F 0B" },
    { "rep movsb", "DSc", "DSc", "F3 A4" },
    { "rep movsq", "DSc", "DSc", "F3 48 A5" },
    { "std rep movsb cld", "DSc", "DSc", "FD F3 A4 FC" },
    { "rep stosb", "Dca", "Dc", "F3 AA" },
    { "rep stosq", "Dca", "Dc", "F3 48 AB" },
    { "rep insw", "Dcd", "Dc", "66 F3 6D" },
    { "rep outsw", "Scd", "Sc", "66 F3 6F" },
    { "mov %%gs:0, %0", "", "r", "65 48 8B 04 25 00 00 00 00" },
    { "xor %%rsp, %%rsp ud2", "", "", "48 31 E4 0F 0B" },
    { "fxsave (%0)", "r", "", "0F AE 00" },
    { "fxrstor (%0)", "r", "", "0F AE 08" },
    { "pushfq cli popq %0", "", "r", "9C FA 58" },
    { "pushfq; pop %0", "", "r", "9C 58" },
    { "outb %%al, $0x80", "a", "", "E6 80" },
    { "ldmxcsr %0", "r", "", "0F AE 10" },
    { "lidt %0 int3", "r", "", "0F 01 18 CC" },
    { "rdrand %0; setc %1", "", "rc", "48 0F C7 F0 0F 92 C1 0F B6 C9" },
    { "rdseed %0; setc %1", "", "rc", "48 0F C7 F8 0F 92 C1 0F B6 C9" },
    { "lgdt %0 pushq $0x08 leaq 1f(%%rip), %%rax pushq %%rax lretq 1: mov $0x10, %%ax mov %%ax, %%ds mov %%ax, %%es mov %%ax, %%fs mov %%ax, %%gs mov %%ax, %%ss", "r", "", "0F 01 10 6A 08 48 8D 05 03 00 00 00 50 48 CB 66 B8 10 00 8E D8 8E C0 8E E0 8E E8 8E D0" },
    { "int $18", "", "", "CD 12" },
    { "bsfl %1, %0", "a", "a", "0F BC C0" },
    { "bsfq %1, %0", "a", "a", "48 0F BC C0" },
    { "bsrl %1, %0", "a", "a", "0F BD C0" },
    { 0, 0, 0, 0 } };

static int hexv(char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; }
static int xreg_of(char c) {
    switch (c) { case 'a': case 'r': return R_AX; case 'b': return R_BX; case 'c': return R_CX; case 'd': return R_DX; case 'S': return R_SI; default: return R_DI; }
}
/* шаблоны привилегированных/строковых команд: входы грузятся в закреплённые регистры, команда выдаётся готовыми байтами, выходы забираются из регистров */
static int gen_asm_tab(const Ins *i) {
    const char *t = i->sym;
    const int *o = (const int *)(intptr_t)i->imm;
    for (int e = 0; xasm_tab[e].t; e++) {
        if (strcmp(t, xasm_tab[e].t)) continue;
        const char *in = xasm_tab[e].in, *out = xasm_tab[e].out;
        for (int k = 0; in[k]; k++) ldv(xreg_of(in[k]), i->args[k]);
        int hasb = strchr(out, 'b') != NULL;
        if (hasb) xa_ins(A, X_MOV, R64(R_11), R64(R_BX));
        unsigned char buf[48]; int nb = 0;
        for (const char *h = xasm_tab[e].hex; *h;) {
            if (*h == ' ') { h++; continue; }
            buf[nb++] = (unsigned char)(hexv(h[0]) * 16 + hexv(h[1]));
            h += 2;
        }
        xa_raw(A, buf, nb);
        if (hasb) { static const unsigned char x[3] = { 0x4C, 0x87, 0xDB }; xa_raw(A, x, 3); }   /* xchg rbx, r11: rbx восстановлен, r11 = результат */
        for (int k = 0; out[k]; k++) stv(o[k], out[k] == 'b' ? R_11 : xreg_of(out[k]));
        return 1;
    }
    return 0;
}

static void gen_asm(const Ins *i) {
    const char *t = i->sym;
    const int *o = (const int *)(intptr_t)i->imm;
    if (i->file) { A->file = i->file; A->line = i->line; }
    if (!*t) return;
    else if (!strcmp(t, "nop")) xa_op0(A, X0_NOP);
    else if (!strcmp(t, "mfence")) xa_op0(A, X0_MFENCE);
    else if (!strcmp(t, "lfence")) xa_op0(A, X0_LFENCE);
    else if (!strcmp(t, "sfence")) xa_op0(A, X0_SFENCE);
    else if (!strcmp(t, "pause")) xa_op0(A, X0_PAUSE);
    else if (!strcmp(t, "hlt")) xa_op0(A, X0_HLT);
    else if (!strcmp(t, "cli")) xa_op0(A, X0_CLI);
    else if (!strcmp(t, "sti")) xa_op0(A, X0_STI);
    else if (!strcmp(t, "rdtsc")) { xa_op0(A, X0_RDTSC); stv(o[0], R_AX); stv(o[1], R_DX); }
    else if (!strncmp(t, "out", 3) && strlen(t) == 11 && !strcmp(t + 4, " %0, %1")) {
        ldv(R_AX, i->args[0]); ldv(R_DX, i->args[1]);
        xa_op0(A, t[3] == 'b' ? X0_OUT8 : t[3] == 'w' ? X0_OUT16 : X0_OUT32);
    }
    else if (!strncmp(t, "in", 2) && strlen(t) == 10 && !strcmp(t + 3, " %1, %0")) {
        ldv(R_DX, i->args[0]);
        xa_ins(A, X_XOR, xr(4, R_AX), xr(4, R_AX));
        xa_op0(A, t[2] == 'b' ? X0_IN8 : t[2] == 'w' ? X0_IN16 : X0_IN32);
        stv(o[0], R_AX);
    }
    else if (!strcmp(t, "syscall")) {
        ldv(R_DI, i->args[0]); ldv(R_SI, i->args[1]); ldv(R_DX, i->args[2]); ldv(R_AX, i->args[3]);
        xa_op0(A, X0_SYSCALL);
        stv(o[0], R_AX); stv(o[1], R_DI); stv(o[2], R_SI); stv(o[3], R_DX);
    }
    else if (!gen_asm_tab(i)) kerr(A->file, A->line, "inline asm template '%s' is not supported on x86_64", t);
}

static void gen_ins(const Ins *i, int homebase_h) {
    switch (i->op) {
    case IR_LABEL: xa_label(A, (int)i->imm); break;
    case IR_CONST:
        if (isc[i->dst]) break;
        if (rg[i->dst] >= 0) mov_imm(rg[i->dst], i->imm);
        else if (i->imm >= INT32_MIN && i->imm <= INT32_MAX) xa_ins(A, X_MOV, spill(i->dst), xi(i->imm));
        else { mov_imm(R_AX, i->imm); stv(i->dst, R_AX); }
        break;
    case IR_GADDR: {
        Obj *t = gen_lookup(i->sym);
        if (!t) kerr(A->file, A->line, "undefined reference to '%s'", i->sym);
        xa_ins(A, X_LEA, R64(R_AX), xs(8, t, 0));
        stv(i->dst, R_AX);
        break;
    }
    case IR_LADDR:
        xa_ins(A, X_LEA, R64(R_AX), xm(8, R_BP, -(int64_t)slotoff[i->imm]));
        stv(i->dst, R_AX);
        break;
    case IR_PARAM: {
        const ALoc *l = &ploc[i->imm];
        if (!i->sub) {
            if (l->kind == AL_REG) xa_ins(A, X_MOV, R64(R_AX), xm(8, R_BP, -(int64_t)homebase_h + 8 * l->idx));
            else xa_ins(A, X_MOV, R64(R_AX), xm(8, R_BP, 16 + l->off));
            stv(i->dst, R_AX);
        } else {
            ldv(R_CX, i->a);
            if (l->kind == AL_AREG) copy_bytes(R_CX, 0, R_BP, -(int64_t)homebase_h + 8 * l->idx, i->w);
            else copy_bytes(R_CX, 0, R_BP, 16 + l->off, i->w);
        }
        break;
    }
    case IR_ATOMIC: gen_atomic(i); break;
    case IR_VAINFO:
        if (i->sub == 0) xa_ins(A, X_LEA, R64(R_AX), xm(8, R_BP, -(int64_t)homebase_h));
        else if (i->sub == 1) xa_ins(A, X_LEA, R64(R_AX), xm(8, R_BP, 16 + p_stk));
        else if (i->sub == 3) xa_ins(A, X_MOV, R64(R_AX), xm(8, R_BP, 8));
        else xa_ins(A, X_MOV, xr(4, R_AX), xi(p_ngrn));
        stv(i->dst, R_AX);
        break;
    case IR_LOAD: case IR_STORE: gen_ldst(i, -1, -1, 0, 0); break;
    case IR_MOV: { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); if (rg[i->dst] < 0) stv(i->dst, d); break; }
    case IR_BIN: gen_bin(i); break;
    case IR_NEG: { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); xa_ins(A, X_NEG, R64(d), NOOPD); normr(d, i->w, i->sgn); if (rg[i->dst] < 0) stv(i->dst, d); break; }
    case IR_NOT: { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); xa_ins(A, X_NOT, R64(d), NOOPD); normr(d, i->w, i->sgn); if (rg[i->dst] < 0) stv(i->dst, d); break; }
    case IR_CMP: gen_cmp(i); break;
    case IR_EXT: {
        int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
        if (rg[i->a] >= 0 && !isc[i->a] && i->w == 4 && !i->sgn) xa_ins(A, X_MOV, xr(4, d), xr(4, rg[i->a]));
        else { ldv(d, i->a); normr(d, i->w, i->sgn); }
        if (rg[i->dst] < 0) stv(i->dst, d);
        break;
    }
    case IR_JMP: xa_jmp(A, CC_NONE, (int)i->imm); break;
    case IR_BRZ: case IR_BRNZ:
        { int ra = rg[i->a] >= 0 && !isc[i->a] ? rg[i->a] : (ldv(R_AX, i->a), R_AX);
        xa_ins(A, X_TEST, R64(ra), R64(ra)); }
        xa_jmp(A, i->op == IR_BRZ ? CC_E : CC_NE, (int)i->imm);
        break;
    case IR_CALL: gen_call(i); break;
    case IR_RET:
        if (i->a > 0 && i->sub) {                    /* агрегат: до 16 байт в rax:rdx, больше - в память по скрытому указателю */
            ldv(R_11, i->a);
            if (i->w <= 16) {
                ld_reg_bytes(R_AX, R_11, 0, i->w > 8 ? 8 : i->w);
                if (i->w > 8) ld_reg_bytes(R_DX, R_11, 8, i->w - 8);
            } else {
                xa_ins(A, X_MOV, R64(R_CX), xm(8, R_BP, -(int64_t)homebase_h));
                copy_bytes(R_CX, 0, R_11, 0, i->w);
                xa_ins(A, X_MOV, R64(R_AX), R64(R_CX));
            }
        } else if (i->a > 0) ldv(R_AX, i->a);
        xa_jmp(A, CC_NONE, epi);
        break;
    case IR_COPY:
        ldv(R_DI, i->a); ldv(R_SI, i->b);
        mov_imm(R_CX, i->imm);
        xa_op0(A, X0_REP_MOVSB);
        break;
    case IR_ZERO:
        ldv(R_DI, i->a);
        xa_ins(A, X_XOR, xr(4, R_AX), xr(4, R_AX));
        mov_imm(R_CX, i->imm);
        xa_op0(A, X0_REP_STOSB);
        break;
    case IR_ASM: gen_asm(i); break;
    case IR_UNREACH: xa_op0(A, X0_UD2); break;
    }
}

static void pad16(void) { while (A->len & 15) xa_op0(A, X0_NOP); }

static void gen_func(IrFn *fi) {
    Obj *f = fi->f;
    A->file = K.srcpath; A->line = f->line;
    pad16();
    f->addr = A->len;
    int n = 0;
    for (Ins *i = fi->head; i; i = i->next) n++;
    Ins **ins = ka_new(sizeof(Ins *) * (size_t)(n + 1));
    n = 0;
    for (Ins *i = fi->head; i; i = i->next) ins[n++] = i;
    int nv = fi->nreg;
    vs = ka_new(sizeof(int) * (size_t)(nv + 1)); ve = ka_new(sizeof(int) * (size_t)(nv + 1));
    rg = ka_new(sizeof(int) * (size_t)(nv + 1)); so = ka_new(sizeof(int) * (size_t)(nv + 1));
    for (int v = 0; v <= nv; v++) { vs[v] = INT_MAX; ve[v] = -1; rg[v] = -1; }
    {
        int *nd = ka_new(sizeof(int) * (size_t)(nv + 2)), *nu = ka_new(sizeof(int) * (size_t)(nv + 2));
        Ins **di = ka_new(sizeof(Ins *) * (size_t)(nv + 2));
        ir_usecount(fi, nd, nu, di);
        g_nd = nd; g_nu = nu; g_mdef = fi->mdef;
        cv = ka_new(sizeof(int64_t) * (size_t)(nv + 2));
        isc = ka_new((size_t)nv + 2);
        fusecmp = ka_new((size_t)n + 2);
        for (int v = 1; v <= nv; v++)
            if (nd[v] == 1 && di[v] && di[v]->op == IR_CONST && !(fi->mdef && fi->mdef[v])) { isc[v] = 1; cv[v] = di[v]->imm; }
        for (int k = 0; k + 1 < n; k++) {
            const Ins *c = ins[k], *b = ins[k + 1];
            if (c->op == IR_CMP && (b->op == IR_BRZ || b->op == IR_BRNZ) && b->a == c->dst && c->dst > 0 && nd[c->dst] == 1 && nu[c->dst] == 1 &&
                !(fi->mdef && fi->mdef[c->dst]))
                fusecmp[k] = 1;
        }
    }
    int *labpos = ka_new(sizeof(int) * (size_t)(fi->nlab + 2));
    for (int l = 0; l <= fi->nlab + 1; l++) labpos[l] = -1;
    int maxout = 0, natom = 0;
    for (int k = 0; k < n; k++) {
        gen_live(ins[k], k, nv, vs, ve);
        if (ins[k]->op == IR_LABEL) {
            if (ins[k]->imm < 0 || ins[k]->imm > fi->nlab) kerr(A->file, A->line, "internal: bad label");
            labpos[ins[k]->imm] = k;
        }
        if (ins[k]->op == IR_CALL) {
            ALoc *cl = ka_new(sizeof(ALoc) * (size_t)(ins[k]->nargs + 1));
            int ng, sp = plan_args(ins[k]->nargs, ins[k]->asz, ins[k]->rsz > 16, cl, &ng);
            if (sp > maxout) maxout = sp;
        }
        if (ins[k]->op == IR_ATOMIC) natom++;
    }
    for (int v = 1; v <= nv; v++) if (isc[v]) { vs[v] = INT_MAX; ve[v] = -1; }
    for (int k = 0; k < n; k++) if (fusecmp[k]) { int d = ins[k]->dst; vs[d] = INT_MAX; ve[d] = -1; }
    /* обратные переходы: всё, что живёт в начале цикла, живёт до конца цикла */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int k = 0; k < n; k++) {
            const Ins *j = ins[k];
            if (j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) continue;
            if (j->imm < 0 || j->imm > fi->nlab || labpos[j->imm] < 0) kerr(A->file, A->line, "internal: jump to undefined label %d", (int)j->imm);
            int l = labpos[j->imm];
            if (l > k) continue;
            for (int v = 1; v <= nv; v++) {
                if (fi->mdef && fi->mdef[v] && ve[v] >= 0) {
                    /* переменная с несколькими определениями: значение может идти по обратному переходу, поэтому весь цикл */
                    if (vs[v] <= k && ve[v] >= l && (vs[v] > l || ve[v] < k)) {
                        if (vs[v] > l) vs[v] = l;
                        if (ve[v] < k) ve[v] = k;
                        changed = 1;
                    }
                } else if (ve[v] >= l && vs[v] < l && ve[v] < k) { ve[v] = k; changed = 1; }
            }
        }
    }
    /* линейный проход */
    int *order = ka_new(sizeof(int) * (size_t)(nv + 1)), no = 0;
    for (int v = 1; v <= nv; v++) if (ve[v] >= 0) order[no++] = v;
    qsort(order, (size_t)no, sizeof(int), cmp_start);
    /* барьеры: инструкции, портящие caller-saved регистры; значение, чей интервал их содержит, живёт только в callee-saved */
    int *bc = ka_new(sizeof(int) * (size_t)(n + 2));
    for (int k = 0; k < n; k++) {
        const Ins *b = ins[k];
        int bar = b->op == IR_CALL || b->op == IR_ATOMIC || b->op == IR_ASM || b->op == IR_COPY || b->op == IR_ZERO ||
                  (b->op == IR_PARAM && b->sub) || (b->op == IR_RET && b->sub);
        bc[k + 1] = bc[k] + bar;
    }
    /* вес vreg: число обращений с учётом глубины циклов (вытесняется самый «дешёвый», а не самый длинный) */
    long *wt = ka_new(sizeof(long) * (size_t)(nv + 2));
    {
        int *dd = ka_new(sizeof(int) * (size_t)(n + 2));
        for (int k = 0; k < n; k++) {
            const Ins *j = ins[k];
            if ((j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) || j->imm < 0 || j->imm > fi->nlab || labpos[j->imm] < 0) continue;
            int l = labpos[j->imm];
            if (l <= k) { dd[l]++; dd[k + 1]--; }
        }
        int depth = 0;
        for (int k = 0; k < n; k++) {
            depth += dd[k];
            long w = 1L << (3 * (depth > 5 ? 5 : depth));
            WtCtx wc = { wt, w, nv };
            ir_walk(ins[k], wt_visit, &wc);
        }
    }
    int act[NPOOL] = { 0 }, used[NPOOL] = { 0 };
    for (int q = 0; q < no; q++) {
        int v = order[q], best = -1;
        int cross = bc[ve[v] + 1] - bc[vs[v]] > 0;
        int lim = cross ? NCS : NPOOL;
        for (int k = 0; k < NPOOL; k++) if (act[k] && ve[act[k]] <= vs[v]) act[k] = 0;
        if (!cross) for (int k = NCS; k < NPOOL; k++) if (!act[k]) { best = k; break; }
        if (best < 0) for (int k = 0; k < NCS; k++) if (!act[k]) { best = k; break; }
        if (best < 0) {
            int far = -1;
            for (int k = 0; k < lim; k++) if (far < 0 || wt[act[k]] < wt[act[far]]) far = k;
            if (far >= 0 && wt[act[far]] < wt[v]) { rg[act[far]] = -1; best = far; }
        }
        if (best >= 0) { act[best] = v; rg[v] = pool[best]; used[best] = 1; }
    }
    /* слоты спила с повторным использованием */
    int nsl = 0;
    int *slend = ka_new(sizeof(int) * (size_t)(no + 1));
    for (int q = 0; q < no; q++) {
        int v = order[q];
        if (rg[v] >= 0) continue;
        int s = -1;
        for (int k = 0; k < nsl; k++) if (slend[k] <= vs[v]) { s = k; break; }
        if (s < 0) s = nsl++;
        slend[s] = ve[v]; so[v] = s;
    }
    /* кадр */
    int64_t cur = 0;
    int saveoff[NCS];
    for (int k = 0; k < NCS; k++) if (used[k]) { cur += 8; saveoff[k] = (int)cur; }
    p_hidden = ty_is_agg(f->ty->base) && ty_unqual(f->ty->base)->size > 16;
    {
        int np = f->nparams;
        ALoc *pl = ka_new(sizeof(ALoc) * (size_t)(np + 1));
        int *pa = ka_new(sizeof(int) * (size_t)(np + 1)), q = 0;
        for (Obj *p = f->params; p && q < np; p = p->next, q++) pa[q] = ty_is_agg(p->ty) ? ty_unqual(p->ty)->size : 0;
        p_stk = plan_args(np, pa, p_hidden, pl, &p_ngrn);
        ploc = pl;
    }
    int nh = f->ty->variadic ? 6 : (p_hidden && p_ngrn < 1 ? 1 : p_ngrn);   /* домашняя область - только для приходящих регистров */
    cur += 8 * nh;
    int homeh = (int)cur;
    spillbase = (int)cur;
    cur += 8 * (int64_t)nsl;
    int maxslot = -1;
    for (Obj *l = f->locals; l; l = l->next) if (l->slot > maxslot) maxslot = l->slot;
    slotoff = ka_new(sizeof(int) * (size_t)(maxslot + 2));
    for (Obj *l = f->locals; l; l = l->next) {
        int64_t sz = l->ty->size > 0 ? l->ty->size : 1;
        int al = l->align > l->ty->align ? l->align : l->ty->align;
        if (al < 1) al = 1;
        if (al > 16) kerr(K.srcpath, l->line ? l->line : f->line, "unsupported alignment %d of local '%s' (max 16)", al, l->name);
        cur = (cur + sz + al - 1) & ~(int64_t)(al - 1);
        if (cur > 0x3fffffff) kerr(K.srcpath, f->line, "stack frame of '%s' is too large", f->name);
        slotoff[l->slot] = (int)cur;
    }
    int64_t frame = (cur + (int64_t)maxout + 15) & ~(int64_t)15;
    epi = fi->nlab + 1;
    tmplab = epi + 1;
    xa_labels_begin(A, epi + 1 + natom);
    xa_op0(A, X0_PUSH_RBP);
    xa_ins(A, X_MOV, R64(R_BP), R64(R_SP));
    if (frame) xa_ins(A, X_SUB, R64(R_SP), xi(frame));
    for (int k = 0; k < NCS; k++) if (used[k]) xa_ins(A, X_MOV, xm(8, R_BP, -saveoff[k]), R64(pool[k]));
    for (int k = 0; k < nh; k++) xa_ins(A, X_MOV, xm(8, R_BP, -(int64_t)homeh + 8 * k), R64(argreg[k]));
    for (int k = 0; k < n; k++) {
        if (fusecmp[k]) { gen_cmpbr(ins[k], ins[k + 1]); k++; continue; }
        { int fz = kcc_noopt ? 0 : try_addr_fuse(ins, k, n); if (fz) { k += fz - 1; continue; } }
        gen_ins(ins[k], homeh);
    }
    A->file = K.srcpath; A->line = f->line;
    xa_label(A, epi);
    for (int k = 0; k < NCS; k++) if (used[k]) xa_ins(A, X_MOV, R64(pool[k]), xm(8, R_BP, -saveoff[k]));
    xa_op0(A, X0_LEAVE);
    xa_op0(A, X0_RET);
    xa_labels_end(A);
}

/* ---------- встроенные функции, если программа их не определяет ---------- */
static void gen_helper(Obj *o) {
    A->file = K.srcpath; A->line = 1;
    pad16();
    o->addr = A->len;
    xa_labels_begin(A, 3);
    if (!strcmp(o->name, "memcpy")) {
        xa_ins(A, X_MOV, R64(R_AX), R64(R_DI)); xa_ins(A, X_MOV, R64(R_CX), R64(R_DX));
        xa_op0(A, X0_REP_MOVSB); xa_op0(A, X0_RET);
    } else if (!strcmp(o->name, "memset")) {
        xa_ins(A, X_MOV, R64(R_8), R64(R_DI)); xa_ins(A, X_MOV, xr(4, R_AX), xr(4, R_SI)); xa_ins(A, X_MOV, R64(R_CX), R64(R_DX));
        xa_op0(A, X0_REP_STOSB); xa_ins(A, X_MOV, R64(R_AX), R64(R_8)); xa_op0(A, X0_RET);
    } else if (!strcmp(o->name, "memcmp")) {
        xa_ins(A, X_MOV, R64(R_CX), R64(R_DX));
        xa_label(A, 1);
        xa_ins(A, X_TEST, R64(R_CX), R64(R_CX)); xa_jmp(A, CC_E, 3);
        xa_ins(A, X_MOVZX, xr(4, R_AX), xm(1, R_DI, 0)); xa_ins(A, X_MOVZX, xr(4, R_DX), xm(1, R_SI, 0));
        xa_ins(A, X_SUB, xr(4, R_AX), xr(4, R_DX)); xa_jmp(A, CC_NE, 2);
        xa_ins(A, X_ADD, R64(R_DI), xi(1)); xa_ins(A, X_ADD, R64(R_SI), xi(1)); xa_ins(A, X_SUB, R64(R_CX), xi(1));
        xa_jmp(A, CC_NONE, 1);
        xa_label(A, 3); xa_ins(A, X_XOR, xr(4, R_AX), xr(4, R_AX));
        xa_label(A, 2); xa_ins(A, X_MOVSX, R64(R_AX), xr(4, R_AX)); xa_op0(A, X0_RET);
    } else if (!strcmp(o->name, "setjmp")) {
        /* jmp_buf (13 слов): rbx, rbp, r12-r15, rsp после возврата, адрес возврата. Возвращает 0. */
        static const int regs[6] = { R_BX, R_BP, R_12, R_13, R_14, R_15 };
        for (int k = 0; k < 6; k++) xa_ins(A, X_MOV, xm(8, R_DI, 8 * k), R64(regs[k]));
        xa_ins(A, X_LEA, R64(R_DX), xm(8, R_SP, 8)); xa_ins(A, X_MOV, xm(8, R_DI, 48), R64(R_DX));
        xa_ins(A, X_MOV, R64(R_DX), xm(8, R_SP, 0)); xa_ins(A, X_MOV, xm(8, R_DI, 56), R64(R_DX));
        xa_ins(A, X_XOR, xr(4, R_AX), xr(4, R_AX)); xa_op0(A, X0_RET);
    } else if (!strcmp(o->name, "longjmp")) {
        /* longjmp(env, v): возвращает v (0 -> 1) из места setjmp */
        static const int regs[6] = { R_BX, R_BP, R_12, R_13, R_14, R_15 };
        xa_ins(A, X_MOV, xr(4, R_AX), xr(4, R_SI));
        xa_ins(A, X_TEST, xr(4, R_AX), xr(4, R_AX)); xa_jmp(A, CC_NE, 1);
        xa_ins(A, X_ADD, xr(4, R_AX), xi(1));
        xa_label(A, 1);
        for (int k = 0; k < 6; k++) xa_ins(A, X_MOV, R64(regs[k]), xm(8, R_DI, 8 * k));
        xa_ins(A, X_MOV, R64(R_DX), xm(8, R_DI, 56));
        xa_ins(A, X_MOV, R64(R_SP), xm(8, R_DI, 48));
        xa_ins(A, X_JMPR, R64(R_DX), NOOPD);
    } else {
        xa_ins(A, X_MOV, R64(R_AX), R64(R_DI));
        xa_label(A, 1);
        xa_ins(A, X_CMP, xm(1, R_AX, 0), xi(0)); xa_jmp(A, CC_E, 2);
        xa_ins(A, X_ADD, R64(R_AX), xi(1)); xa_jmp(A, CC_NONE, 1);
        xa_label(A, 2); xa_ins(A, X_SUB, R64(R_AX), R64(R_DI)); xa_op0(A, X0_RET);
    }
    xa_labels_end(A);
}

static void x86_reloc(unsigned char *text, void *ctx) {
    (void)ctx;
    for (XFix *f = xa.fix; f; f = f->next) {
        Obj *t = f->target;
        if (!t->emit) kerr(K.srcpath, f->line, "undefined reference to '%s'", t->name);
        int64_t v = (int64_t)t->addr + f->add - (int64_t)(elf_base(ELF_ARCH_X86) + (uint64_t)f->off + 4);
        if (v < INT32_MIN || v > INT32_MAX) kerr(K.srcpath, f->line, "relocation to '%s' is out of range", t->name);
        for (int k = 0; k < 4; k++) text[f->off + k] = (unsigned char)((uint32_t)v >> (8 * k));
    }
}

void x86_build_obj(Obj *globals, unsigned char **out, size_t *outlen) {
    memset(&xa, 0, sizeof xa);
    gen_prepare(globals);
    A->file = K.srcpath; A->line = 1;
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && g->body && g->emit) gen_func((IrFn *)g->ir);
    size_t nr = 0;
    for (XFix *f = xa.fix; f; f = f->next) nr++;
    ObjRel *rel = ka_new(sizeof(ObjRel) * (nr + 1));
    nr = 0;
    for (XFix *f = xa.fix; f; f = f->next) {
        rel[nr].off = (uint64_t)f->off; rel[nr].t = f->target; rel[nr].add = f->add - 4; rel[nr].type = f->target->is_func ? 4 : 2;
        nr++;
    }
    elf_emit_obj(globals, A->p, A->len, ELF_ARCH_X86, rel, nr, out, outlen);
}

void x86_build_elf(Obj *globals, unsigned char **out, size_t *outlen) {
    memset(&xa, 0, sizeof xa);
    Obj *mainf = gen_prepare(globals);
    /* _start: argc/argv не передаются, main(0, 0); код выхода - в K_EXIT */
    A->file = K.srcpath; A->line = 1;
    xa_ins(A, X_XOR, xr(4, R_BP), xr(4, R_BP));
    xa_ins(A, X_AND, R64(R_SP), xi(-16));
    xa_ins(A, X_XOR, xr(4, R_DI), xr(4, R_DI));
    xa_ins(A, X_XOR, xr(4, R_SI), xr(4, R_SI));
    xa_call(A, mainf);
    xa_ins(A, X_MOV, xr(4, R_DI), xr(4, R_AX));
    xa_ins(A, X_MOV, xr(4, R_AX), xi(2));
    xa_op0(A, X0_SYSCALL);
    xa_op0(A, X0_UD2);
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && g->body && g->emit) gen_func((IrFn *)g->ir);
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && !g->body && g->emit && gen_is_helper_name(g->name)) gen_helper(g);
    elf_emit(globals, A->p, A->len, ELF_ARCH_X86, x86_reloc, NULL, out, outlen);
}
