/* kcc: кодогенерация x86-64 из линейного IR (System V ABI для вызовов).
 * Каждая функция: живые интервалы vreg -> линейный проход по пяти callee-saved регистрам (rbx, r12-r15), остальное - в стековые
 * слоты. Рабочие регистры (rax, rcx, rdx, rsi, rdi, r8-r11) свободны для кодогенератора и ничего не хранят между инструкциями IR.
 * Достижимость: в образ попадают main, функции и данные с внешней видимостью и всё, на что они ссылаются; статические
 * неиспользуемые функции отбрасываются (как --gc-sections). Неопределённый символ - ошибка. */
#include "x86.h"
#include "target.h"
#include "passes.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>


/* ---------- состояние функции ---------- */
/* Пул vreg (rdx - последний): пять callee-saved (сохраняются в прологе) и три caller-saved (r8-r10: бесплатны, но только для значений,
   живущих не через вызов, atomic, asm, копирование памяти и подобные инструкции, которые их используют). */
#define NCS 5
#define NPOOL 12
#define NPOOL_OLD 8
static const int pool[NPOOL] = { R_BX, R_12, R_13, R_14, R_15, R_8, R_9, R_10, R_SI, R_DI, R_11, R_DX };   /* rsi, rdi, r11: тоже только вне вызовов и подобных инструкций; rdx - ещё и вне деления */
/* Свёртка смещения: t = base + K (K - imm32), который читают только как адрес LOAD/STORE, не вычисляется: каждое обращение
   идёт по [base + K]. fbase[t] - база, fdisp[t] - K; fold[t] - свёрнутый vreg. */
static const int argreg[6] = { R_DI, R_SI, R_DX, R_CX, R_8, R_9 };
/* Вещественные значения (SSE2). Пул xmm8-xmm12 для vreg, чей живой интервал не содержит вызова (xmm - все caller-saved: значение через вызов живёт в стековом
   слоте); xmm13-xmm15 - рабочие регистры кодогенератора; xmm0-xmm7 - только аргументы, результат вызова и входные параметры (читаются до любого их
   использования другими командами). Класс vreg (fcls: 0 - целое, 4 - float, 8 - double) выводится из вещественных операций, параметров, вызовов и
   копий; на корректность не влияет (ldv/stv работают с любым классом, переходя через movd/movq), только на место хранения. Ни одной команды SSE не
   выдаётся в функциях без вещественных операций и при -mno-sse (ядро Киры). */
#define NXP 5
static const int xpool[NXP] = { 8, 9, 10, 11, 12 };
#define XS0 13
#define XS1 14
#define XS2 15
/* ---------- ABI System V: размещение аргументов ---------- */
enum { AL_REG, AL_STK, AL_AREG, AL_ASTK, AL_XREG };
typedef struct { int kind, idx, off; } ALoc;        /* REG: номер регистра; AREG: первый из регистров; STK/ASTK: смещение в области аргументов */
typedef struct {
    XA xa;
    int npool;
    unsigned char *fold;
    int *fbase;
    int64_t *fdisp;
    unsigned char pdirect[6];
    int g_retlast, g_frameless;
    unsigned char *fcls;
    int *xg;
    int p_nxrn;
    ALoc *ploc;
    int p_ngrn, p_stk, p_hidden;
    int *vs, *ve, *rg, *so;
    int *slotoff;
    int spillbase, epi;
    int64_t *cv;
    unsigned char *isc, *fusecmp;
    int *g_nd, *g_nu;
    const unsigned char *g_mdef;
    int tmplab, g_maxout;
} X86State;
#define X86S ((X86State *)K.x86_state)
#define xa (X86S->xa)
#define A (&xa)
#define npool (X86S->npool)
#define fold (X86S->fold)
#define fbase (X86S->fbase)
#define fdisp (X86S->fdisp)
#define pdirect (X86S->pdirect)
#define g_retlast (X86S->g_retlast)
#define g_frameless (X86S->g_frameless)
#define fcls (X86S->fcls)
#define xg (X86S->xg)
#define p_nxrn (X86S->p_nxrn)
#define ploc (X86S->ploc)
#define p_ngrn (X86S->p_ngrn)
#define p_stk (X86S->p_stk)
#define p_hidden (X86S->p_hidden)
#define vs (X86S->vs)
#define ve (X86S->ve)
#define rg (X86S->rg)
#define so (X86S->so)
#define slotoff (X86S->slotoff)
#define spillbase (X86S->spillbase)
#define epi (X86S->epi)
#define cv (X86S->cv)
#define isc (X86S->isc)
#define fusecmp (X86S->fusecmp)
#define g_nd (X86S->g_nd)
#define g_nu (X86S->g_nu)
#define g_mdef (X86S->g_mdef)
#define tmplab (X86S->tmplab)
#define g_maxout (X86S->g_maxout)

static int plan_args(int n, const int *asz, const unsigned char *afp, int hidden, ALoc *loc, int *ngrn, int *nxrn) {
    int g = hidden ? 1 : 0, sp = 0, x = 0;
    for (int k = 0; k < n; k++) {
        int z = asz ? asz[k] : 0;
        if (afp && afp[k]) {
            if (x < 8) { loc[k].kind = AL_XREG; loc[k].idx = x++; }
            else { loc[k].kind = AL_STK; loc[k].off = sp; sp += 8; }
        } else if (!z) {
            if (g < 6) { loc[k].kind = AL_REG; loc[k].idx = g++; }
            else { loc[k].kind = AL_STK; loc[k].off = sp; sp += 8; }
        } else {
            int nr = (z + 7) / 8;
            if (z <= 16 && g + nr <= 6) { loc[k].kind = AL_AREG; loc[k].idx = g; g += nr; }
            else { loc[k].kind = AL_ASTK; loc[k].off = sp; sp += (z + 7) & ~7; }
        }
    }
    *ngrn = g;
    if (nxrn) *nxrn = x;
    return sp;
}
/* Оптимизации бэкенда: константный vreg (одно определение IR_CONST) не занимает регистр и подставляется непосредственно;
   сравнение, единственный читатель результата которого - следующий IR_BRZ/IR_BRNZ, сливается с переходом. */

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
static Opd spillsz(int v, int sz) { Opd s = spill(v); s.sz = sz; return s; }
static void ldv(int r, int v) {
    if (v <= 0) kerr(A->file, A->line, "internal: use of an undefined vreg");
    if (isc[v]) { mov_imm_r(r, cv[v]); return; }
    if (fcls && fcls[v]) {                      /* вещественный vreg: в целый регистр - младшие 4/8 байт, верх нулевой */
        int sz = fcls[v];
        if (xg[v] >= 0) xa_ins(A, X_MOVXG, xr(sz, r), xx(sz, xg[v]));
        else xa_ins(A, X_MOV, xr(sz, r), spillsz(v, sz));
        return;
    }
    if (rg[v] >= 0) { if (rg[v] != r) xa_ins(A, X_MOV, R64(r), R64(rg[v])); }
    else xa_ins(A, X_MOV, R64(r), spill(v));
}
static void stv(int v, int r) {
    if (v <= 0) return;
    if (fcls && fcls[v]) {
        if (xg[v] >= 0) xa_ins(A, X_MOVGX, xx(fcls[v], xg[v]), xr(fcls[v], r));
        else xa_ins(A, X_MOV, spill(v), R64(r));
        return;
    }
    if (rg[v] >= 0) { if (rg[v] != r) xa_ins(A, X_MOV, R64(rg[v]), R64(r)); }
    else xa_ins(A, X_MOV, spill(v), R64(r));
}

/* vreg v (размера sz: 4 или 8 байт) -> xmm x. Константа собирается через rax; нулевая - xorps. */
static void ldx(int x, int v, int sz) {
    if (v <= 0) kerr(A->file, A->line, "internal: use of an undefined vreg");
    if (isc[v]) {
        if (cv[v] == 0) xa_ins(A, X_XORPS, xx(8, x), xx(8, x));
        else { mov_imm_r(R_AX, cv[v]); xa_ins(A, X_MOVGX, xx(sz, x), xr(sz, R_AX)); }
        return;
    }
    if (xg[v] >= 0) { if (xg[v] != x) xa_ins(A, X_MOVAPS, xx(8, x), xx(8, xg[v])); return; }
    if (rg[v] >= 0) { xa_ins(A, X_MOVGX, xx(sz, x), xr(sz, rg[v])); return; }
    xa_ins(A, X_MOVS, xx(sz, x), spillsz(v, sz));
}
/* xmm x (размера sz) -> vreg v. В слот спила float пишется 4 байта; читатели целого вида класса float берут 4 байта (ldv). */
static void stx(int v, int x, int sz) {
    if (v <= 0) return;
    if (xg[v] >= 0) { if (xg[v] != x) xa_ins(A, X_MOVAPS, xx(8, xg[v]), xx(8, x)); return; }
    if (rg[v] >= 0) { xa_ins(A, X_MOVXG, xr(sz, rg[v]), xx(sz, x)); return; }
    if (sz == 4 && fcls[v] != 4) { xa_ins(A, X_MOVXG, xr(4, R_11), xx(4, x)); xa_ins(A, X_MOV, spill(v), R64(R_11)); return; }
    xa_ins(A, X_MOVS, spillsz(v, sz), xx(sz, x));
}
/* второй операнд вещественной команды: xmm vreg, слот спила подходящего размера либо рабочий регистр tmp со значением */
static Opd xsrc(int v, int sz, int tmp) {
    if (!isc[v] && xg[v] >= 0) return xx(sz, xg[v]);
    if (!isc[v] && rg[v] < 0 && fcls[v] == sz) return spillsz(v, sz);
    ldx(tmp, v, sz);
    return xx(sz, tmp);
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
        int s4 = sz4 || (i->w == 4 && !kcc_nonnshift);        /* 32-битный сдвиг: operand может быть неточным (верх не читается) */
        if (bimm) { if (imm & (s4 ? 31 : 63)) xa_ins(A, op, xr(s4 ? 4 : 8, d), xi(imm & (s4 ? 31 : 63))); }
        else xa_ins(A, op, xr(s4 ? 4 : 8, d), xr(1, R_CX));
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
            if (k > 0) xa_ins(A, X_SHL, RS(d), xi(k));
            else if (k < 0) xa_imul3(A, RS(d), RS(d), imm);           /* k == 0: умножение на 1, ничего не делаем */
        } else if (bimm) xa_ins(A, op, RS(d), xi(imm));
        else if (bm >= 0) xa_ins(A, op, RS(d), RS(bm));
        else { ldv(R_CX, b); xa_ins(A, op, RS(d), RS(R_CX)); }
    }
    if (!sz4 && !i->nn) normr(d, i->w, i->sgn);
    #undef RS
    if (rg[i->dst] != d) stv(i->dst, d);
}


/* Загрузка/запись в память. base < 0: адрес берётся из vreg i->a (в rax, если вытеснен); иначе адрес задан регистрами [base + ix*2^sc + disp] (слитый расчёт адреса) */
static void gen_ldst(const Ins *i, int base, int ix, int sc, int64_t disp) {
    if (i->w != 1 && i->w != 2 && i->w != 4 && i->w != 8) kerr(A->file, A->line, "x86-64 backend: unsupported %s width %d", i->op == IR_LOAD ? "load" : "store", i->w);
    if (base < 0 && i->a > 0 && fold[i->a]) {
        int fb = fbase[i->a];
        base = rg[fb] >= 0 ? rg[fb] : (ldv(R_AX, fb), R_AX);
        disp = fdisp[i->a];
    }
    if (base < 0) base = rg[i->a] >= 0 ? rg[i->a] : (ldv(R_AX, i->a), R_AX);
    #define MEM(w) (ix < 0 ? xm((w), base, disp) : xmi((w), base, ix, sc, disp))
    if (i->op == IR_LOAD && fcls && xg[i->dst] >= 0 && fcls[i->dst] == i->w) {          /* вещественная загрузка прямо в xmm */
        xa_ins(A, X_MOVS, xx(i->w, xg[i->dst]), MEM(i->w));
        return;
    }
    if (i->op == IR_STORE && fcls && i->b > 0 && !isc[i->b] && xg[i->b] >= 0 && fcls[i->b] == i->w) {
        xa_ins(A, X_MOVS, MEM(i->w), xx(i->w, xg[i->b]));
        return;
    }
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


/* ---------- вещественные операции ---------- */

static void gen_fbin(const Ins *i) {
    int w = i->w, a = i->a, b = i->b;
    int op = i->sub == ND_ADD ? X_ADDS : i->sub == ND_SUB ? X_SUBS : i->sub == ND_MUL ? X_MULS : X_DIVS;
    int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
    if (a != b && !isc[b] && xg[b] >= 0 && xg[b] == d) {
        if (i->sub == ND_ADD || i->sub == ND_MUL) { int t = a; a = b; b = t; }
        else d = XS0;
    }
    ldx(d, a, w);
    xa_ins(A, op, xx(w, d), xsrc(b, w, XS1));
    stx(i->dst, d, w);
}

static void gen_fun(const Ins *i) {
    int w = i->w;
    if (i->sub == FN_SQRT) {
        int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
        xa_ins(A, X_SQRTS, xx(w, d), xsrc(i->a, w, XS1));
        stx(i->dst, d, w);
        return;
    }
    mov_imm_r(R_AX, w == 8 ? (int64_t)0x8000000000000000ULL : (int64_t)0x80000000LL);
    xa_ins(A, X_MOVGX, xx(w, XS1), xr(w, R_AX));                  /* маска знакового бита */
    if (i->sub == FN_NEG) {
        int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
        ldx(d, i->a, w);
        xa_ins(A, X_XORPS, xx(8, d), xx(8, XS1));
        stx(i->dst, d, w);
    } else {                                                     /* abs: ~маска & значение */
        Opd s = xsrc(i->a, w, XS2);
        if (s.k == OP_MEM) s.sz = 16;
        if (s.k == OP_MEM) { ldx(XS2, i->a, w); s = xx(w, XS2); }
        xa_ins(A, X_ANDNPS, xx(8, XS1), s);
        stx(i->dst, XS1, w);
    }
}

/* ucomis с подготовкой операндов: LT и LE сравнивают b с a (тогда "выше"/"не ниже" ложны и для неупорядоченных), EQ и NE - a с b */
static void fcmp_flags(const Ins *i) {
    int w = i->w, x = i->a, y = i->b;
    if (i->sub == ND_LT || i->sub == ND_LE) { x = i->b; y = i->a; }
    int rx;
    if (!isc[x] && xg[x] >= 0) rx = xg[x]; else { ldx(XS0, x, w); rx = XS0; }
    xa_ins(A, X_UCOMIS, xx(w, rx), xsrc(y, w, XS1));
}
static void gen_fcmp(const Ins *i) {
    fcmp_flags(i);
    int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
    switch (i->sub) {
    case ND_LT: xa_setcc(A, CC_A, xr(1, R_AX)); break;
    case ND_LE: xa_setcc(A, CC_AE, xr(1, R_AX)); break;
    case ND_EQ: xa_setcc(A, CC_E, xr(1, R_AX)); xa_setcc(A, CC_NP, xr(1, R_CX)); xa_ins(A, X_AND, xr(1, R_AX), xr(1, R_CX)); break;
    default: xa_setcc(A, CC_NE, xr(1, R_AX)); xa_setcc(A, CC_P, xr(1, R_CX)); xa_ins(A, X_OR, xr(1, R_AX), xr(1, R_CX)); break;
    }
    xa_ins(A, X_MOVZX, xr(4, d), xr(1, R_AX));
    if (rg[i->dst] != d) stv(i->dst, d);
}
/* сравнение + brz/brnz: переход по флагам; для неупорядоченных (PF=1) условие "равно" ложно, "не равно" истинно */
static void gen_fcmpbr(const Ins *c, const Ins *br) {
    fcmp_flags(c);
    int lab = (int)br->imm, brz = br->op == IR_BRZ;
    switch (c->sub) {
    case ND_LT: xa_jmp(A, brz ? CC_BE : CC_A, lab); break;
    case ND_LE: xa_jmp(A, brz ? CC_B : CC_AE, lab); break;
    default: {
        int jump_on_eq = (c->sub == ND_EQ) != brz;               /* EQ+brnz и NE+brz: переход, когда равны (упорядоченно) */
        if (jump_on_eq) { int skip = tmplab++; xa_jmp(A, CC_P, skip); xa_jmp(A, CC_E, lab); xa_label(A, skip); }
        else { xa_jmp(A, CC_P, lab); xa_jmp(A, CC_NE, lab); }
        break;
    }
    }
}

static void gen_fcvt(const Ins *i) {
    int w = i->w;
    switch (i->sub) {
    case FC_I2F: {
        int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
        ldv(R_AX, i->a);
        xa_ins(A, X_XORPS, xx(8, d), xx(8, d));                  /* разрыв ложной зависимости по старшим битам */
        if (i->sgn) xa_ins(A, X_CVTSI2S, xx(w, d), R64(R_AX));
        else {                                                   /* беззнаковое 64 бита: при старшем бите - (x >> 1 | x & 1), результат удвоить */
            int big = tmplab++, done = tmplab++;
            xa_ins(A, X_TEST, R64(R_AX), R64(R_AX));
            xa_jmp(A, CC_S, big);
            xa_ins(A, X_CVTSI2S, xx(w, d), R64(R_AX));
            xa_jmp(A, CC_NONE, done);
            xa_label(A, big);
            xa_ins(A, X_MOV, R64(R_CX), R64(R_AX));
            xa_ins(A, X_SHR, R64(R_AX), xi(1));
            xa_ins(A, X_AND, xr(4, R_CX), xi(1));
            xa_ins(A, X_OR, R64(R_AX), R64(R_CX));
            xa_ins(A, X_CVTSI2S, xx(w, d), R64(R_AX));
            xa_ins(A, X_ADDS, xx(w, d), xx(w, d));
            xa_label(A, done);
        }
        stx(i->dst, d, w);
        return;
    }
    case FC_F2I: {
        Opd s = xsrc(i->a, w, XS0);
        if (s.k == OP_MEM) { ldx(XS0, i->a, w); s = xx(w, XS0); }
        if (i->sgn) xa_ins(A, X_CVTTS2SI, R64(R_AX), s);
        else {                                                   /* беззнаковое 64 бита: от 2^63 вычесть 2^63, преобразовать, вернуть старший бит */
            int big = tmplab++, done = tmplab++;
            mov_imm_r(R_AX, w == 8 ? (int64_t)0x43E0000000000000LL : (int64_t)0x5F000000LL);
            xa_ins(A, X_MOVGX, xx(w, XS1), xr(w, R_AX));
            xa_ins(A, X_UCOMIS, s, xx(w, XS1));
            xa_jmp(A, CC_AE, big);
            xa_ins(A, X_CVTTS2SI, R64(R_AX), s);
            xa_jmp(A, CC_NONE, done);
            xa_label(A, big);
            xa_ins(A, X_MOVAPS, xx(8, XS2), s);
            xa_ins(A, X_SUBS, xx(w, XS2), xx(w, XS1));
            xa_ins(A, X_CVTTS2SI, R64(R_AX), xx(w, XS2));
            xa_ins(A, X_MOV, R64(R_CX), xi((int64_t)0x8000000000000000ULL));
            xa_ins(A, X_XOR, R64(R_AX), R64(R_CX));
            xa_label(A, done);
        }
        stv(i->dst, R_AX);
        return;
    }
    default: {                                                   /* F2D, D2F */
        int sw = i->sub == FC_F2D ? 4 : 8, dw = 12 - sw;
        int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
        Opd s = xsrc(i->a, sw, XS1);
        xa_ins(A, X_CVTS2S, xx(dw, d), s);
        stx(i->dst, d, dw);
        return;
    }
    }
}

static void gen_call(const Ins *i) {
    ALoc *loc = ka_new(sizeof(ALoc) * (size_t)(i->nargs + 1));
    int ng, nx;
    plan_args(i->nargs, i->asz, i->afp, i->rsz > 16, loc, &ng, &nx);
    if (!i->sym) ldv(R_10, i->a);
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_STK && i->afp && i->afp[k]) { ldx(XS0, i->args[k], i->afp[k]); xa_ins(A, X_MOVS, xm(i->afp[k], R_SP, loc[k].off), xx(i->afp[k], XS0)); }
        else if (loc[k].kind == AL_STK) { ldv(R_AX, i->args[k]); xa_ins(A, X_MOV, xm(8, R_SP, loc[k].off), R64(R_AX)); }
        else if (loc[k].kind == AL_ASTK) { ldv(R_11, i->args[k]); copy_bytes(R_SP, loc[k].off, R_11, 0, i->asz[k]); }
    }
    if (i->rsz > 16) ldv(R_DI, i->rt);
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_REG) ldv(argreg[loc[k].idx], i->args[k]);
        else if (loc[k].kind == AL_XREG) ldx(loc[k].idx, i->args[k], i->afp[k]);
        else if (loc[k].kind == AL_AREG) {
            int z = i->asz[k];
            ldv(R_11, i->args[k]);
            ld_reg_bytes(argreg[loc[k].idx], R_11, 0, z > 8 ? 8 : z);
            if (z > 8) ld_reg_bytes(argreg[loc[k].idx + 1], R_11, 8, z - 8);
        }
    }
    if (i->nout) xa_ins(A, X_MOV, xr(4, R_AX), xi(nx));       /* вариадический вызов: al = число векторных регистров */
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
    if (i->dst > 0 && i->fp) stx(i->dst, 0, i->fp);
    else if (i->dst > 0) { norm(i->w, i->sgn); stv(i->dst, R_AX); }
}

/* ---------- атомарные операции (seq_cst) ---------- */
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
    { "clflush (%0)", "r", "", "0F AE 38" },
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
        if (fcls && xg[i->dst] >= 0) {                                       /* вещественная константа в xmm */
            if (!i->imm) xa_ins(A, X_XORPS, xx(8, xg[i->dst]), xx(8, xg[i->dst]));
            else { mov_imm(R_AX, i->imm); xa_ins(A, X_MOVGX, xx(fcls[i->dst], xg[i->dst]), xr(fcls[i->dst], R_AX)); }
            break;
        }
        if (fcls && fcls[i->dst]) { mov_imm(R_AX, i->imm); stv(i->dst, R_AX); break; }
        if (rg[i->dst] >= 0) mov_imm(rg[i->dst], i->imm);
        else if (i->imm >= INT32_MIN && i->imm <= INT32_MAX) xa_ins(A, X_MOV, spill(i->dst), xi(i->imm));
        else { mov_imm(R_AX, i->imm); stv(i->dst, R_AX); }
        break;
    case IR_GADDR: {
        Obj *t = gen_lookup(i->sym);
        if (!t) kerr(A->file, A->line, "undefined reference to '%s'", i->sym);
        int gd = i->dst > 0 && rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
        xa_ins(A, X_LEA, R64(gd), xs(8, t, 0));
        if (gd == R_AX) stv(i->dst, R_AX);
        break;
    }
    case IR_LADDR:
    {   int ld = i->dst > 0 && rg[i->dst] >= 0 ? rg[i->dst] : R_AX;
        xa_ins(A, X_LEA, R64(ld), xm(8, R_BP, -(int64_t)slotoff[i->imm]));
        if (ld == R_AX) stv(i->dst, R_AX);
        break;
    }
    case IR_PARAM: {
        const ALoc *l = &ploc[i->imm];
        if (i->fp && !i->sub) {
            if (l->kind == AL_XREG) stx(i->dst, l->idx, i->fp);
            else {                                                           /* вещественный параметр на стеке */
                int d = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
                xa_ins(A, X_MOVS, xx(i->fp, d), xm(i->fp, R_BP, 16 + l->off));
                stx(i->dst, d, i->fp);
            }
            break;
        }
        if (!i->sub && l->kind == AL_REG && l->idx < 6 && pdirect[l->idx] == 2) { if (i->dst > 0 && ve[i->dst] >= 0) stv(i->dst, argreg[l->idx]); break; }   /* прямо из регистра аргумента */
        if (!i->sub) {
            int pd = i->dst > 0 && rg[i->dst] >= 0 ? rg[i->dst] : R_AX;       /* сразу в регистр приёмника: без промежуточного rax */
            if (l->kind == AL_REG) xa_ins(A, X_MOV, R64(pd), xm(8, R_BP, -(int64_t)homebase_h + 8 * l->idx));
            else xa_ins(A, X_MOV, R64(pd), xm(8, R_BP, 16 + l->off));
            if (pd == R_AX) stv(i->dst, R_AX);
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
        else if (i->sub == 4) xa_ins(A, X_MOV, xr(4, R_AX), xi(p_nxrn));
        else xa_ins(A, X_MOV, xr(4, R_AX), xi(p_ngrn));
        stv(i->dst, R_AX);
        break;
    case IR_ALLOCA:                                          /* rsp -= размер (кратно 16); блок лежит над областью исходящих аргументов */
        ldv(R_AX, i->a);
        xa_ins(A, X_ADD, R64(R_AX), xi(15));
        xa_ins(A, X_AND, R64(R_AX), xi(-16));
        xa_ins(A, X_SUB, R64(R_SP), R64(R_AX));
        xa_ins(A, X_LEA, R64(R_AX), xm(8, R_SP, g_maxout));
        stv(i->dst, R_AX);
        break;
    case IR_SPSAVE: xa_ins(A, X_MOV, R64(R_AX), R64(R_SP)); stv(i->dst, R_AX); break;
    case IR_SPREST: ldv(R_AX, i->a); xa_ins(A, X_MOV, R64(R_SP), R64(R_AX)); break;
    case IR_LOAD: case IR_STORE: gen_ldst(i, -1, -1, 0, 0); break;
    case IR_MOV:
        if (fcls && (fcls[i->dst] || fcls[i->a]) && (xg[i->dst] >= 0 || (!isc[i->a] && xg[i->a] >= 0))) {   /* пересылка между xmm и чем угодно */
            int sz = fcls[i->dst] ? fcls[i->dst] : fcls[i->a];
            int x = xg[i->dst] >= 0 ? xg[i->dst] : XS0;
            ldx(x, i->a, sz);
            stx(i->dst, x, sz);
            break;
        }
        { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); if (rg[i->dst] < 0) stv(i->dst, d); break; }
    case IR_BIN: gen_bin(i); break;
    case IR_FBIN: gen_fbin(i); break;
    case IR_FUN: gen_fun(i); break;
    case IR_FCMP: gen_fcmp(i); break;
    case IR_FCVT: gen_fcvt(i); break;
    case IR_NEG: { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); xa_ins(A, X_NEG, R64(d), NOOPD); if (!i->nn) normr(d, i->w, i->sgn); if (rg[i->dst] < 0) stv(i->dst, d); break; }
    case IR_NOT: { int d = rg[i->dst] >= 0 ? rg[i->dst] : R_AX; ldv(d, i->a); xa_ins(A, X_NOT, R64(d), NOOPD); if (!i->nn) normr(d, i->w, i->sgn); if (rg[i->dst] < 0) stv(i->dst, d); break; }
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
        } else if (i->a > 0 && i->fp) ldx(0, i->a, i->fp);
        else if (i->a > 0) ldv(R_AX, i->a);
        if (g_frameless) xa_op0(A, X0_RET);
        else if (!g_retlast) xa_jmp(A, CC_NONE, epi);
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
/* выравнивание заголовка цикла на 16 байт многобайтовыми nop (не больше 10 байт; как у clang) */
static void pad_loop(void) {
    static const unsigned char nop[10][10] = { {0}, {0x90}, {0x66, 0x90}, {0x0F, 0x1F, 0x00}, {0x0F, 0x1F, 0x40, 0x00}, {0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}, {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00} };
    int d = (int)((16 - (A->len & 15)) & 15);
    if (d > 0 && d <= 9) xa_raw(A, nop[d], d);
    else if (d == 10) { xa_raw(A, nop[9], 9); xa_raw(A, nop[1], 1); }
}

typedef struct { int v, hit; } FoldDef;
static void fold_badvisit(Ins *i, int *r, int kind, void *c) {
    unsigned char *bad = c;
    if (kind == 0 && *r > 0 && !((i->op == IR_LOAD || i->op == IR_STORE) && r == &i->a)) bad[*r] = 1;
}
static void fold_defvisit(Ins *i, int *r, int kind, void *c) { FoldDef *f = c; (void)i; if (kind != 0 && *r == f->v) f->hit = 1; }

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
        fcls = NULL; xg = NULL;
        if (!kcc_nosse) {
            int hasf = 0;
            for (int k = 0; k < n && !hasf; k++) {
                const Ins *q = ins[k];
                if (q->op == IR_FBIN || q->op == IR_FUN || q->op == IR_FCMP || q->op == IR_FCVT || q->fp || q->afp) hasf = 1;
            }
            if (hasf) {
                fcls = ka_new((size_t)nv + 2); xg = ka_new(sizeof(int) * (size_t)(nv + 2));
                for (int v = 0; v <= nv; v++) xg[v] = -1;
                #define FSEED(v, c) do { int v_ = (v); if (v_ > 0 && v_ <= nv) { int c_ = (c); fcls[v_] = (unsigned char)(!fcls[v_] || fcls[v_] == c_ ? c_ : 3); } } while (0)
                for (int k = 0; k < n; k++) {
                    const Ins *q = ins[k];
                    switch (q->op) {
                    case IR_FBIN: case IR_FCMP: FSEED(q->a, q->w); FSEED(q->b, q->w); if (q->op == IR_FBIN) FSEED(q->dst, q->w); break;
                    case IR_FUN: FSEED(q->a, q->w); FSEED(q->dst, q->w); break;
                    case IR_FCVT:
                        if (q->sub == FC_I2F) FSEED(q->dst, q->w);
                        else if (q->sub == FC_F2I) FSEED(q->a, q->w);
                        else { FSEED(q->a, q->sub == FC_F2D ? 4 : 8); FSEED(q->dst, q->sub == FC_F2D ? 8 : 4); }
                        break;
                    case IR_PARAM: if (q->fp && !q->sub) FSEED(q->dst, q->fp); break;
                    case IR_RET: if (q->fp) FSEED(q->a, q->fp); break;
                    case IR_CALL:
                        if (q->fp) FSEED(q->dst, q->fp);
                        for (int j = 0; q->afp && j < q->nargs; j++) if (q->afp[j]) FSEED(q->args[j], q->afp[j]);
                        break;
                    default: break;
                    }
                }
                #undef FSEED
                for (int ch = 1; ch;) {                   /* класс переходит через копии; несовместимые размеры - целый класс */
                    ch = 0;
                    for (int k = 0; k < n; k++) {
                        const Ins *q = ins[k];
                        if (q->op != IR_MOV || q->dst <= 0 || q->a <= 0 || q->dst > nv || q->a > nv) continue;
                        unsigned char x = fcls[q->a], y = fcls[q->dst], m = x == y ? x : !x ? y : !y ? x : 3;
                        if (x != m) { fcls[q->a] = m; ch = 1; }
                        if (y != m) { fcls[q->dst] = m; ch = 1; }
                    }
                }
                for (int v = 0; v <= nv; v++) if (fcls[v] == 3) fcls[v] = 0;
            }
        }
        for (int k = 0; k + 1 < n; k++) {
            const Ins *c = ins[k], *b = ins[k + 1];
            if ((c->op == IR_CMP || c->op == IR_FCMP) && (b->op == IR_BRZ || b->op == IR_BRNZ) && b->a == c->dst && c->dst > 0 && nd[c->dst] == 1 && nu[c->dst] == 1 &&
                !(fi->mdef && fi->mdef[c->dst]))
                fusecmp[k] = 1;
        }
    }
    fold = ka_new((size_t)nv + 2); fbase = ka_new(sizeof(int) * (size_t)(nv + 2)); fdisp = ka_new(sizeof(int64_t) * (size_t)(nv + 2));
    if (!kcc_nodisp && !kcc_noopt) {
        unsigned char *bad = ka_new((size_t)nv + 2);
        int *dpos = ka_new(sizeof(int) * (size_t)(nv + 2));
        for (int k = 0; k < n; k++) {
            const Ins *d = ins[k];
            int64_t imm;
            if (d->op == IR_BIN && d->sub == ND_ADD && d->w == 8 && d->dst > 0 && g_nd[d->dst] == 1 && !(g_mdef && g_mdef[d->dst]) && !isc[d->dst] &&
                d->a > 0 && !isc[d->a] && d->a != d->dst && imm32_of(d->b, &imm)) { fold[d->dst] = 1; fbase[d->dst] = d->a; fdisp[d->dst] = imm; dpos[d->dst] = k; }
        }
        for (int k = 0; k < n; k++) ir_walk(ins[k], fold_badvisit, bad);       /* чтение t не как адреса LOAD/STORE отменяет свёртку */
        for (int k = 0; k < n; k++) {                         /* база должна оставаться той же от определения t до каждого обращения */
            const Ins *u = ins[k];
            if ((u->op != IR_LOAD && u->op != IR_STORE) || u->a <= 0 || !fold[u->a]) continue;
            int t = u->a, x = fbase[t], ok = k > dpos[t];
            if (ok && (g_nd[x] > 1 || (g_mdef && g_mdef[x])))
                for (int j = dpos[t] + 1; j < k && ok; j++) {
                    if (ins[j]->op == IR_LABEL) ok = 0;
                    else { FoldDef fd = { x, 0 }; ir_walk((Ins *)ins[j], fold_defvisit, &fd); if (fd.hit) ok = 0; }
                }
            if (!ok) bad[t] = 1;
        }
        for (int v = 1; v <= nv; v++) if (fold[v] && bad[v]) fold[v] = 0;
    }
    int *labpos = ka_new(sizeof(int) * (size_t)(fi->nlab + 2));
    for (int l = 0; l <= fi->nlab + 1; l++) labpos[l] = -1;
    int maxout = 0, natom = 0, nfp = 0;
    for (int k = 0; k < n; k++) {
        gen_live(ins[k], k, nv, vs, ve);
        if (ins[k]->op == IR_LABEL) {
            if (ins[k]->imm < 0 || ins[k]->imm > fi->nlab) kerr(A->file, A->line, "internal: bad label");
            labpos[ins[k]->imm] = k;
        }
        if (ins[k]->op == IR_CALL) {
            ALoc *cl = ka_new(sizeof(ALoc) * (size_t)(ins[k]->nargs + 1));
            int ng, sp = plan_args(ins[k]->nargs, ins[k]->asz, ins[k]->afp, ins[k]->rsz > 16, cl, &ng, NULL);
            if (sp > maxout) maxout = sp;
        }
        if (ins[k]->op == IR_ATOMIC) natom++;
        if (ins[k]->op == IR_FCMP) nfp += 2;
        if (ins[k]->op == IR_FCVT) nfp += 2;
    }
    for (int v = 1; v <= nv; v++) if (fold[v]) { if (ve[v] >= 0 && ve[fbase[v]] < ve[v]) ve[fbase[v]] = ve[v]; vs[v] = INT_MAX; ve[v] = -1; }
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
    int *bc = ka_new(sizeof(int) * (size_t)(n + 2)), *bd = ka_new(sizeof(int) * (size_t)(n + 2));
    for (int k = 0; k < n; k++) {
        bd[k + 1] = bd[k] + (ins[k]->op == IR_BIN && (ins[k]->sub == ND_DIV || ins[k]->sub == ND_MOD));
        const Ins *b = ins[k];
        int bar = b->op == IR_CALL || b->op == IR_ATOMIC || b->op == IR_ASM || b->op == IR_COPY || b->op == IR_ZERO ||
                  (b->op == IR_PARAM && b->sub) || (b->op == IR_RET && b->sub);
        bc[k + 1] = bc[k] + bar;
    }
    /* барьеры для xmm: вызов портит все xmm; значение, чей интервал содержит вызов строго внутри, живёт в слоте. Аргумент, умирающий на вызове, и результат
       вызова (определяется после него) не мешают. */
    int *xb = NULL;
    if (fcls) {
        xb = ka_new(sizeof(int) * (size_t)(n + 2));
        for (int k = 0; k < n; k++) xb[k + 1] = xb[k] + (ins[k]->op == IR_CALL || ins[k]->op == IR_ASM);
    }
    /* вес vreg: число обращений с учётом глубины циклов (вытесняется самый «дешёвый», а не самый длинный) */
    long *wt = ka_new(sizeof(long) * (size_t)(nv + 2));
    unsigned char *lhead = ka_new((size_t)fi->nlab + 2);     /* метка - цель обратного перехода (заголовок цикла) */
    {
        int *dd = ka_new(sizeof(int) * (size_t)(n + 2));
        for (int k = 0; k < n; k++) {
            const Ins *j = ins[k];
            if ((j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) || j->imm < 0 || j->imm > fi->nlab || labpos[j->imm] < 0) continue;
            int l = labpos[j->imm];
            if (l <= k) { dd[l]++; dd[k + 1]--; lhead[j->imm] = 1; }
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
    npool = kcc_noregs ? NPOOL_OLD : NPOOL;
    /* при выборе жертвы сравнивается плотность (вес на длину интервала): короткие временные значения важнее долгоживущих переменных */
    #define DENS_LESS(a, b) (kcc_noregs ? wt[a] < wt[b] : wt[a] * (long)(ve[b] - vs[b] + 1) < wt[b] * (long)(ve[a] - vs[a] + 1))
    for (int q = 0; q < no; q++) {
        int v = order[q], best = -1;
        if (fcls && fcls[v] && !isc[v]) continue;                    /* вещественные vreg - в xmm либо в слоте, ниже */
        int cross = bc[ve[v] + 1] - bc[vs[v]] > 0;
        int divx = bd[ve[v] + 1] - bd[vs[v]] > 0;                 /* в интервале есть деление: rdx занят */
        int lim = cross ? NCS : npool;
        for (int k = 0; k < npool; k++) if (act[k] && ve[act[k]] <= vs[v]) act[k] = 0;
        if (!cross) for (int k = NCS; k < npool; k++) if (!act[k] && !(divx && pool[k] == R_DX)) { best = k; break; }
        if (best < 0) for (int k = 0; k < NCS; k++) if (!act[k]) { best = k; break; }
        if (best < 0) {
            int far = -1;
            for (int k = 0; k < lim; k++) if (!(divx && pool[k] == R_DX) && (far < 0 || DENS_LESS(act[k], act[far]))) far = k;
            if (far >= 0 && DENS_LESS(act[far], v)) { rg[act[far]] = -1; best = far; }
        }
        if (best >= 0) { act[best] = v; rg[v] = pool[best]; used[best] = 1; }
    }
    if (fcls) {                                                      /* линейный проход по пулу xmm */
        int xact[NXP] = { 0 };
        for (int q = 0; q < no; q++) {
            int v = order[q], best = -1;
            if (!fcls[v] || isc[v]) continue;
            for (int k = 0; k < NXP; k++) if (xact[k] && ve[xact[k]] <= vs[v]) xact[k] = 0;
            if (vs[v] < ve[v] && xb[ve[v]] - xb[vs[v] + 1] > 0) continue;
            for (int k = 0; k < NXP; k++) if (!xact[k]) { best = k; break; }
            if (best < 0) {
                int far = 0;
                for (int k = 1; k < NXP; k++) if (wt[xact[k]] < wt[xact[far]]) far = k;
                if (wt[xact[far]] < wt[v]) { xg[xact[far]] = -1; best = far; }
            }
            if (best >= 0) { xact[best] = v; xg[v] = xpool[best]; }
        }
    }
    /* слоты спила с повторным использованием */
    int nsl = 0;
    int *slend = ka_new(sizeof(int) * (size_t)(no + 1));
    for (int q = 0; q < no; q++) {
        int v = order[q];
        if (rg[v] >= 0 || (xg && xg[v] >= 0) || (fcls && isc[v])) continue;
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
        unsigned char *pf = ka_new((size_t)np + 1);
        for (Obj *p = f->params; p && q < np; p = p->next, q++) {
            pa[q] = ty_is_agg(p->ty) ? ty_unqual(p->ty)->size : 0;
            pf[q] = (unsigned char)(ty_is_float(p->ty) ? p->ty->size : 0);
        }
        p_stk = plan_args(np, pa, pf, p_hidden, pl, &p_ngrn, &p_nxrn);
        ploc = pl;
    }
    int vsse = f->ty->variadic && !kcc_nosse;                      /* вариадическая функция сохраняет и xmm0-7 (область регистров: 6 * 8 + 8 * 16 = 176 байт) */
    int nhin = f->ty->variadic ? 6 : (p_hidden && p_ngrn < 1 ? 1 : p_ngrn);   /* домашняя область - только для приходящих регистров */
    unsigned char homed[6] = { 0 };               /* какие приходящие регистры надо сохранить в домашней области */
    for (int k = 0; k < 6; k++) pdirect[k] = 0;
    for (int k = 0; k < nhin && k < 6; k++) homed[k] = 1;
    if (!kcc_noregs) {
        /* скалярные параметры в начале функции берутся прямо из регистров аргументов, если очередная запись в регистр приёмника
           не портит ещё не прочитанный аргумент; агрегаты в начале (копирование через rcx) отключают это целиком */
        int lead = 0, subp = 0;
        while (lead < n && ins[lead]->op == IR_PARAM) { if (ins[lead]->sub) subp = 1; lead++; }
        for (int m = 0; m < lead && !subp; m++) {
            const ALoc *lm = &ploc[ins[m]->imm];
            if (lm->kind != AL_REG || lm->idx >= 6) continue;
            int ok = 1;
            for (int j = 0; j < m && ok; j++) {
                int dj = ins[j]->dst > 0 && ve[ins[j]->dst] >= 0 ? rg[ins[j]->dst] : -1;
                if (dj == argreg[lm->idx]) ok = 0;
            }
            if (ok) { pdirect[lm->idx] = 2; homed[lm->idx] = 0; }
        }
    }
    int nh = 0;
    for (int k = 0; k < 6; k++) if (homed[k]) nh = k + 1;
    cur += vsse ? 176 : 8 * nh;
    if (vsse) nfp++;
    int homeh = (int)cur;
    spillbase = (int)cur;
    cur += 8 * (int64_t)nsl;
    int maxslot = -1;
    for (Obj *l = f->locals; l; l = l->next) if (l->slot > maxslot) maxslot = l->slot;
    slotoff = ka_new(sizeof(int) * (size_t)(maxslot + 2));
    unsigned char *slotref = ka_new((size_t)maxslot + 2);       /* слоты, чей адрес ещё берётся (продвинутые в vreg места в кадре не занимают) */
    for (int k = 0; k < n; k++) if (ins[k]->op == IR_LADDR && ins[k]->imm >= 0 && ins[k]->imm <= maxslot) slotref[ins[k]->imm] = 1;
    for (Obj *l = f->locals; l; l = l->next) {
        if (!kcc_noregs && (l->slot < 0 || l->slot > maxslot || !slotref[l->slot])) continue;
        int64_t sz = l->ty->size > 0 ? l->ty->size : 1;
        int al = l->align > l->ty->align ? l->align : l->ty->align;
        if (al < 1) al = 1;
        if (al > 16) kerr(K.srcpath, l->line ? l->line : f->line, "unsupported alignment %d of local '%s' (max 16)", al, l->name);
        cur = (cur + sz + al - 1) & ~(int64_t)(al - 1);
        if (cur > 0x3fffffff) kerr(K.srcpath, f->line, "stack frame of '%s' is too large", f->name);
        slotoff[l->slot] = (int)cur;
    }
    int64_t frame = (cur + (int64_t)maxout + 15) & ~(int64_t)15;
    g_maxout = maxout;
    {   /* листовая функция без кадра: ни стека, ни callee-saved, ни вызовов; rbp не нужен */
        int nused = 0, leaf = 1;
        for (int k = 0; k < NCS; k++) nused += used[k];
        for (int k = 0; k < n; k++) if (ins[k]->op == IR_CALL || ins[k]->op == IR_ASM || ins[k]->op == IR_VAINFO || ins[k]->op == IR_COPY || ins[k]->op == IR_ZERO || ins[k]->op == IR_ALLOCA || ins[k]->op == IR_SPSAVE || ins[k]->op == IR_SPREST) leaf = 0;
        g_frameless = !kcc_noregs && leaf && frame == 0 && !nused && nh == 0 && p_stk == 0;
        for (int k = 0; g_frameless && k < n; k++) if (ins[k]->op == IR_PARAM && (ins[k]->sub || ploc[ins[k]->imm].kind != AL_REG)) g_frameless = 0;
    }
    epi = fi->nlab + 1;
    tmplab = epi + 1;
    xa_labels_begin(A, epi + 1 + natom + nfp);
    if (!g_frameless) {
        xa_op0(A, X0_PUSH_RBP);
        xa_ins(A, X_MOV, R64(R_BP), R64(R_SP));
        if (frame) xa_ins(A, X_SUB, R64(R_SP), xi(frame));
    }
    for (int k = 0; k < NCS; k++) if (used[k]) xa_ins(A, X_MOV, xm(8, R_BP, -saveoff[k]), R64(pool[k]));
    for (int k = 0; k < nh; k++) if (homed[k]) xa_ins(A, X_MOV, xm(8, R_BP, -(int64_t)homeh + 8 * k), R64(argreg[k]));
    if (vsse) {                                                    /* al == 0: вещественных аргументов нет, xmm не сохраняем (как gcc) */
        int skip = tmplab++;
        xa_ins(A, X_TEST, xr(1, R_AX), xr(1, R_AX));
        xa_jmp(A, CC_E, skip);
        for (int k = 0; k < 8; k++) xa_ins(A, X_MOVUPS, xm(8, R_BP, -(int64_t)homeh + 48 + 16 * k), xx(8, k));
        xa_label(A, skip);
    }
    for (int k = 0; k < n; k++) {
        g_retlast = k == n - 1 && !kcc_noregs;
        if (ins[k]->op == IR_LABEL && lhead[ins[k]->imm] && !kcc_noalign) pad_loop();
        if (fusecmp[k]) { if (ins[k]->op == IR_FCMP) gen_fcmpbr(ins[k], ins[k + 1]); else gen_cmpbr(ins[k], ins[k + 1]); k++; continue; }
        if (ins[k]->op == IR_BIN && ins[k]->dst > 0 && fold[ins[k]->dst]) continue;        /* смещение свёрнуто в обращения к памяти */
        { int fz = kcc_noopt ? 0 : try_addr_fuse(ins, k, n); if (fz) { k += fz - 1; continue; } }
        gen_ins(ins[k], homeh);
    }
    g_retlast = 0;
    A->file = K.srcpath; A->line = f->line;
    xa_label(A, epi);
    for (int k = 0; k < NCS; k++) if (used[k]) xa_ins(A, X_MOV, R64(pool[k]), xm(8, R_BP, -saveoff[k]));
    if (!g_frameless) xa_op0(A, X0_LEAVE);
    xa_op0(A, X0_RET);
    g_frameless = 0;
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
    if (!K.x86_state) K.x86_state = ka_new(sizeof(X86State));
    memset(X86S, 0, sizeof *X86S);
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
    if (!K.x86_state) K.x86_state = ka_new(sizeof(X86State));
    memset(X86S, 0, sizeof *X86S);
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
