/* kcc: кодогенерация AArch64 из линейного IR (AAPCS64 для вызовов).
 * Каждая функция: живые интервалы vreg -> линейный проход по десяти callee-saved регистрам x19-x28, остальное - в стековые слоты.
 * x0-x15 свободны для кодогенератора и ничего не хранят между инструкциями IR; x16, x17 - служебные кодировщика (большие смещения);
 * x18 (платформенный) не трогается. Кадр адресуется от sp. Атомарные операции - циклы ldaxr/stlxr с барьером (порядок seq_cst).
 * Образ - ELF ET_EXEC для EL0 Киры (база 0x808000000000, см. userland/user_arm64.ld). */
#include "a64.h"
#include "target.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>



enum { S0 = 9, S1 = 10, S2 = 11, S3 = 12, S4 = 13, S5 = 14, S6 = 15 };   /* рабочие регистры */
#define NPOOL 10
static const int pool[NPOOL] = { 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };

/* ---------- AAPCS64: размещение аргументов ---------- */
enum { AL_REG, AL_STK, AL_AREG, AL_ASTK, AL_VREG };
typedef struct { int kind, idx, off; } ALoc;        /* REG: номер x (VREG - номер v); AREG: первый из регистров; STK/ASTK: смещение в области аргументов */
struct A64State {
    A6 assembler;
    ALoc *ploc;
    int p_ngrn, p_nvrn, p_stk, p_hidden;
    int *vs, *ve, *rg, *so;
    int *slotoff;
    int64_t spillbase, homeoff, qoff, hidoff, frame;
    int epi, tmplab;
    int hasvla;
    int64_t maxout_cur;
};
#define A64S (&K)->a64_state
#define xa (A64S->assembler)
#define A (&xa)

/* hidden не занимает регистров (указатель результата - в x8). Агрегат больше 16 байт приходит указателем (копию делает вызывающий в ir.c). */
static int plan_args(int n, const int *asz, const unsigned char *afp, ALoc *loc, int *ngrn, int *nvrn) {
    int g = 0, v = 0, sp = 0;
    for (int k = 0; k < n; k++) {
        int z = asz ? asz[k] : 0;
        if (afp && afp[k]) {                         /* float/double: v0-v7, затем 8 байт в стеке; x-регистры не расходуются */
            if (v < 8) { loc[k].kind = AL_VREG; loc[k].idx = v++; }
            else { loc[k].kind = AL_STK; loc[k].off = sp; sp += 8; }
        } else if (!z || z > 16) {
            if (g < 8) { loc[k].kind = AL_REG; loc[k].idx = g++; }
            else { loc[k].kind = AL_STK; loc[k].off = sp; sp += 8; }
        } else {
            int nr = (z + 7) / 8;
            if (g + nr <= 8) { loc[k].kind = AL_AREG; loc[k].idx = g; g += nr; }
            else { g = 8; loc[k].kind = AL_ASTK; loc[k].off = sp; sp += nr * 8; }
        }
    }
    *ngrn = g;
    if (nvrn) *nvrn = v;
    return sp;
}

/* ---------- состояние функции ---------- */
#define ploc (A64S->ploc)
#define p_ngrn (A64S->p_ngrn)
#define p_nvrn (A64S->p_nvrn)
#define p_stk (A64S->p_stk)
#define p_hidden (A64S->p_hidden)
#define vs (A64S->vs)
#define ve (A64S->ve)
#define rg (A64S->rg)
#define so (A64S->so)
#define slotoff (A64S->slotoff)
#define spillbase (A64S->spillbase)
#define homeoff (A64S->homeoff)
#define qoff (A64S->qoff)
#define hidoff (A64S->hidoff)
#define frame (A64S->frame)
#define epi (A64S->epi)
#define tmplab (A64S->tmplab)
#define hasvla (A64S->hasvla)
#define maxout_cur (A64S->maxout_cur)
#define FB (hasvla ? A6_FP : A6_SP)

static int cmp_start(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (vs[x] != vs[y]) return vs[x] < vs[y] ? -1 : 1;
    return x - y;
}

/* ---------- примитивы ---------- */
static int64_t spill(int v) { return spillbase + 8 * (int64_t)so[v]; }

static void ldv(int r, int v) {
    if (v <= 0) kerr(A->file, A->line, "internal: use of an undefined vreg");
    if (rg[v] >= 0) a6_mov(A, r, rg[v]);
    else a6_ls(A, 1, 8, 0, r, FB, spill(v));
}
static void stv(int v, int r) {
    if (v <= 0) return;
    if (rg[v] >= 0) a6_mov(A, rg[v], r);
    else a6_ls(A, 0, 8, 0, r, FB, spill(v));
}
static int dstreg(int v, int dflt) { return v > 0 && rg[v] >= 0 ? rg[v] : dflt; }   /* куда считать результат: сразу в регистр vreg */

/* канонический вид целого шириной w байт */
static void norm(int r, int w, int sgn) { if (w == 1 || w == 2 || w == 4) a6_ext(A, r, r, w, sgn); }

/* копирование n байт [sb+sof] -> [db+dof] через x13 */
static void copy_bytes(int db, int64_t dof, int sb, int64_t sof, int n) {
    int o = 0;
    while (n - o > 0) {
        int p = n - o >= 8 ? 8 : n - o >= 4 ? 4 : n - o >= 2 ? 2 : 1;
        a6_ls(A, 1, p, 0, S4, sb, sof + o);
        a6_ls(A, 0, p, 0, S4, db, dof + o);
        o += p;
    }
}
/* загрузка n (1..8) байт [base+off] в регистр без обращения за границы объекта */
static void ld_reg_bytes(int reg, int base, int64_t off, int n) {
    if (n == 8 || n == 4 || n == 2 || n == 1) { a6_ls(A, 1, n, 0, reg, base, off); return; }
    int rem = n;
    a6_movi(A, reg, 0);
    while (rem > 0) {
        int p = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
        a6_ls(A, 1, p, 0, S4, base, off + rem - p);
        a6_shi(A, A6_LSLV, reg, reg, 8 * p);
        a6_logic(A, A6_ORR, reg, reg, S4);
        rem -= p;
    }
}

static int ccode(const Ins *i) {
    switch (i->sub) {
    case ND_EQ: return CC_EQ;
    case ND_NE: return CC_NE;
    case ND_LT: return i->sgn ? CC_LT : CC_LO;
    case ND_LE: return i->sgn ? CC_LE : CC_LS;
    default: kerr(A->file, A->line, "internal: bad comparison %d", i->sub);
    }
    return 0;
}

static void gen_bin(const Ins *i) {
    int d = dstreg(i->dst, S0);
    ldv(S0, i->a);
    ldv(S1, i->b);
    switch (i->sub) {
    case ND_ADD: a6_arith(A, A6_ADD, d, S0, S1); break;
    case ND_SUB: a6_arith(A, A6_SUB, d, S0, S1); break;
    case ND_MUL: a6_mul(A, d, S0, S1); break;
    case ND_AND: a6_logic(A, A6_AND, d, S0, S1); break;
    case ND_OR: a6_logic(A, A6_ORR, d, S0, S1); break;
    case ND_XOR: a6_logic(A, A6_EOR, d, S0, S1); break;
    case ND_SHL: a6_shv(A, A6_LSLV, d, S0, S1); break;
    case ND_SHR: a6_shv(A, i->sgn ? A6_ASRV : A6_LSRV, d, S0, S1); break;
    case ND_DIV: a6_div(A, i->sgn, d, S0, S1); break;
    case ND_MOD: a6_div(A, i->sgn, S2, S0, S1); a6_msub(A, d, S2, S1, S0); break;
    default: kerr(A->file, A->line, "internal: bad binary operator %d", i->sub);
    }
    norm(d, i->w, i->sgn);
    if (d == S0) stv(i->dst, S0);
}

static void gen_call(const Ins *i) {
    ALoc *loc = ka_new(sizeof(ALoc) * (size_t)(i->nargs + 1));
    int ng;
    plan_args(i->nargs, i->asz, i->afp, loc, &ng, NULL);
    if (!i->sym) ldv(S6, i->a);
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_STK) { ldv(S0, i->args[k]); a6_ls(A, 0, 8, 0, S0, A6_SP, loc[k].off); }
        else if (loc[k].kind == AL_ASTK) { ldv(S5, i->args[k]); copy_bytes(A6_SP, loc[k].off, S5, 0, i->asz[k]); }
    }
    for (int k = 0; k < i->nargs; k++) {
        if (loc[k].kind == AL_REG) ldv(loc[k].idx, i->args[k]);
        else if (loc[k].kind == AL_VREG) { ldv(S0, i->args[k]); a6_fmov_to(A, i->afp[k], loc[k].idx, S0); }
        else if (loc[k].kind == AL_AREG) {
            int z = i->asz[k];
            ldv(S5, i->args[k]);
            ld_reg_bytes(loc[k].idx, S5, 0, z > 8 ? 8 : z);
            if (z > 8) ld_reg_bytes(loc[k].idx + 1, S5, 8, z - 8);
        }
    }
    if (i->rsz > 16) ldv(8, i->rt);
    if (i->sym) {
        Obj *t = gen_lookup(i->sym);
        if (!t) kerr(A->file, A->line, "undefined reference to '%s'", i->sym);
        a6_bl(A, t);
    } else a6_blr(A, S6);
    if (i->rsz && i->rsz <= 16) {                          /* результат в x0:x1; буфер дополнен до 16 байт (ir.c) */
        ldv(S0, i->rt);
        a6_ls(A, 0, 8, 0, 0, S0, 0);
        if (i->rsz > 8) a6_ls(A, 0, 8, 0, 1, S0, 8);
    }
    if (i->dst > 0 && i->fp) { a6_fmov_from(A, i->fp, 0, 0); stv(i->dst, 0); }
    else if (i->dst > 0) { norm(0, i->w, i->sgn); stv(i->dst, 0); }
}

/* ---------- вещественные операции: значение лежит в x-регистре/слоте как битовый образ, считаем в v16-v18 ---------- */
static void gen_fp(const Ins *i) {
    int w = i->w, d = dstreg(i->dst, S0);
    ldv(S0, i->a);
    if (i->op == IR_FBIN) {
        static const int fa[] = { [ND_ADD] = FA_ADD, [ND_SUB] = FA_SUB, [ND_MUL] = FA_MUL, [ND_DIV] = FA_DIV };
        if (i->sub != ND_ADD && i->sub != ND_SUB && i->sub != ND_MUL && i->sub != ND_DIV) kerr(A->file, A->line, "internal: bad float operator %d", i->sub);
        ldv(S1, i->b);
        a6_fmov_to(A, w, 16, S0); a6_fmov_to(A, w, 17, S1);
        a6_fop(A, fa[i->sub], w, 16, 16, 17);
        a6_fmov_from(A, w, d, 16);
    } else if (i->op == IR_FUN) {
        int op = i->sub == FN_NEG ? FA_NEG : i->sub == FN_SQRT ? FA_SQRT : FA_ABS;
        a6_fmov_to(A, w, 16, S0);
        a6_fop(A, op, w, 16, 16, 0);
        a6_fmov_from(A, w, d, 16);
    } else if (i->op == IR_FCMP) {
        int cc;
        switch (i->sub) {                            /* NZCV после fcmp: равно ZC, меньше N, больше C, неупорядочено CV */
        case ND_EQ: cc = CC_EQ; break;
        case ND_NE: cc = CC_NE; break;
        case ND_LT: cc = CC_MI; break;
        case ND_LE: cc = CC_LS; break;
        default: kerr(A->file, A->line, "internal: bad float comparison %d", i->sub); cc = 0;
        }
        ldv(S1, i->b);
        a6_fmov_to(A, w, 16, S0); a6_fmov_to(A, w, 17, S1);
        a6_fcmp(A, w, 16, 17);
        a6_cset(A, d, cc);
    } else switch (i->sub) {
        case FC_I2F:
            a6_fcvt(A, i->sgn ? FV_SCVTF : FV_UCVTF, w, 16, S0);
            a6_fmov_from(A, w, d, 16);
            break;
        case FC_F2I:
            a6_fmov_to(A, w, 16, S0);
            a6_fcvt(A, i->sgn ? FV_FCVTZS : FV_FCVTZU, w, d, 16);
            break;
        case FC_F2D:
            a6_fmov_to(A, 4, 16, S0); a6_fcvt(A, FV_F2D, 0, 17, 16); a6_fmov_from(A, 8, d, 17);
            break;
        case FC_D2F:
            a6_fmov_to(A, 8, 16, S0); a6_fcvt(A, FV_D2F, 0, 17, 16); a6_fmov_from(A, 4, d, 17);
            break;
        default: kerr(A->file, A->line, "internal: bad float conversion %d", i->sub);
    }
    if (d == S0) stv(i->dst, S0);
}

/* ---------- атомарные операции (seq_cst) ---------- */
static void gen_atomic(const Ins *i) {
    int w = i->w;
    if (i->sub == AT_FENCE) { a6_bar(A, 0, BAR_ISH); return; }
    ldv(S0, i->a);
    if (i->sub == AT_LOAD) {
        a6_xmem(A, X_LDAR, w, S1, S0, 0);
        norm(S1, w, i->sgn); stv(i->dst, S1);
        return;
    }
    ldv(S1, i->b);
    if (i->sub == AT_STORE) { a6_xmem(A, X_STLR, w, S1, S0, 0); a6_bar(A, 0, BAR_ISH); return; }
    int top = tmplab++;
    if (i->sub == AT_CAS) {
        int fail = tmplab++, done = tmplab++;
        ldv(S2, i->c);
        if (w < 8) a6_ext(A, S5, S1, w, 0); else a6_mov(A, S5, S1);        /* ожидаемое без знакового расширения */
        a6_label(A, top);
        a6_xmem(A, X_LDAXR, w, S3, S0, 0);
        a6_arith(A, A6_SUBS, A6_ZR, S3, S5);
        a6_bcc(A, CC_NE, fail);
        a6_xmem(A, X_STLXR, w, S2, S0, S4);
        a6_cbzw(A, 1, S4, top);
        a6_b(A, done);
        a6_label(A, fail);
        a6_op0(A, Z_CLREX);
        a6_label(A, done);
        a6_bar(A, 0, BAR_ISH);
        norm(S3, w, i->sgn); stv(i->dst, S3);
        return;
    }
    a6_label(A, top);
    a6_xmem(A, X_LDAXR, w, S3, S0, 0);
    switch (i->sub) {
    case AT_XCHG: a6_mov(A, S2, S1); break;
    case AT_ADD: a6_arith(A, A6_ADD, S2, S3, S1); break;
    case AT_SUB: a6_arith(A, A6_SUB, S2, S3, S1); break;
    case AT_AND: a6_logic(A, A6_AND, S2, S3, S1); break;
    case AT_OR: a6_logic(A, A6_ORR, S2, S3, S1); break;
    case AT_XOR: a6_logic(A, A6_EOR, S2, S3, S1); break;
    case AT_NAND: a6_logic(A, A6_AND, S2, S3, S1); a6_logic(A, A6_ORN, S2, A6_ZR, S2); break;
    default: kerr(A->file, A->line, "internal: bad atomic operation %d", i->sub);
    }
    a6_xmem(A, X_STLXR, w, S2, S0, S4);
    a6_cbzw(A, 1, S4, top);
    a6_bar(A, 0, BAR_ISH);
    norm(S3, w, i->sgn); stv(i->dst, S3);
}

static void gen_asm(const Ins *i) {
    const char *t = i->sym;
    const int *o = (const int *)(intptr_t)i->imm;
    if (i->file) { A->file = i->file; A->line = i->line; }
    if (!*t) return;
    else if (!strcmp(t, "nop")) a6_op0(A, Z_NOP);
    else if (!strcmp(t, "yield")) a6_op0(A, Z_YIELD);
    else if (!strcmp(t, "wfe")) a6_op0(A, Z_WFE);
    else if (!strcmp(t, "wfi")) a6_op0(A, Z_WFI);
    else if (!strcmp(t, "sev")) a6_op0(A, Z_SEV);
    else if (!strcmp(t, "isb")) a6_op0(A, Z_ISB);
    else if (!strcmp(t, "dmb ish")) a6_bar(A, 0, BAR_ISH);
    else if (!strcmp(t, "dmb ishst")) a6_bar(A, 0, BAR_ISHST);
    else if (!strcmp(t, "dmb ishld")) a6_bar(A, 0, BAR_ISHLD);
    else if (!strcmp(t, "dmb sy")) a6_bar(A, 0, BAR_SY);
    else if (!strcmp(t, "dmb st")) a6_bar(A, 0, BAR_ST);
    else if (!strcmp(t, "dsb ish")) a6_bar(A, 1, BAR_ISH);
    else if (!strcmp(t, "dsb sy")) a6_bar(A, 1, BAR_SY);
    else if (!strcmp(t, "dsb st")) a6_bar(A, 1, BAR_ST);
    else if (!strcmp(t, "mrs %0, cntvct_el0")) { a6_mrs(A, S0, SR_CNTVCT); stv(o[0], S0); }
    else if (!strcmp(t, "mrs %0, cntfrq_el0")) { a6_mrs(A, S0, SR_CNTFRQ); stv(o[0], S0); }
    else if (!strcmp(t, "svc #0")) {
        ldv(0, i->args[0]); ldv(1, i->args[1]); ldv(2, i->args[2]); ldv(8, i->args[3]);
        a6_op0(A, Z_SVC0);
        stv(o[0], 0); stv(o[1], 1); stv(o[2], 2);
    }
    else kerr(A->file, A->line, "inline asm template '%s' is not supported on aarch64", t);
}

static void gen_ins(const Ins *i) {
    switch (i->op) {
    case IR_LABEL: a6_label(A, (int)i->imm); break;
    case IR_CONST: { int d = dstreg(i->dst, S0); a6_movi(A, d, i->imm); if (d == S0) stv(i->dst, S0); break; }
    case IR_GADDR: {
        Obj *t = gen_lookup(i->sym);
        if (!t) kerr(A->file, A->line, "undefined reference to '%s'", i->sym);
        a6_gaddr(A, S0, t, 0);
        stv(i->dst, S0);
        break;
    }
    case IR_LADDR: { int d = dstreg(i->dst, S0); a6_addi(A, 0, d, FB, (uint32_t)slotoff[i->imm]); if (d == S0) stv(i->dst, S0); break; }
    case IR_PARAM: {
        const ALoc *l = &ploc[i->imm];
        if (i->fp && !i->sub) {
            if (l->kind == AL_VREG) a6_ls(A, 1, i->fp, 0, S0, FB, qoff + 16 * l->idx);
            else a6_ls(A, 1, i->fp, 0, S0, FB, frame + l->off);
            stv(i->dst, S0);
        } else if (!i->sub) {
            if (l->kind == AL_REG) a6_ls(A, 1, 8, 0, S0, FB, homeoff + 8 * l->idx);
            else a6_ls(A, 1, 8, 0, S0, FB, frame + l->off);
            stv(i->dst, S0);
        } else {
            ldv(S1, i->a);
            if (l->kind == AL_AREG) copy_bytes(S1, 0, FB, homeoff + 8 * l->idx, i->w);
            else if (l->kind == AL_ASTK) copy_bytes(S1, 0, FB, frame + l->off, i->w);
            else {                                           /* больше 16 байт: пришёл указатель на копию вызывающего */
                if (l->kind == AL_REG) a6_ls(A, 1, 8, 0, S2, FB, homeoff + 8 * l->idx);
                else a6_ls(A, 1, 8, 0, S2, FB, frame + l->off);
                copy_bytes(S1, 0, S2, 0, i->w);
            }
        }
        break;
    }
    case IR_ALLOCA:                                          /* sp -= размер (кратно 16); блок лежит над областью исходящих аргументов */
        ldv(S0, i->a);
        a6_addi(A, 0, S0, S0, 15);
        a6_movi(A, S1, -16);
        a6_logic(A, A6_AND, S0, S0, S1);
        a6_mov(A, S1, A6_SP);
        a6_arith(A, A6_SUB, S1, S1, S0);
        a6_mov(A, A6_SP, S1);
        a6_addi(A, 0, S0, A6_SP, (uint32_t)maxout_cur);
        stv(i->dst, S0);
        break;
    case IR_SPSAVE: a6_mov(A, S0, A6_SP); stv(i->dst, S0); break;
    case IR_SPREST: ldv(S0, i->a); a6_mov(A, A6_SP, S0); break;
    case IR_ATOMIC: gen_atomic(i); break;
    case IR_VAINFO:
        if (i->sub == 0) a6_addi(A, 0, S0, FB, (uint32_t)homeoff);
        else if (i->sub == 1) a6_addi(A, 0, S0, FB, (uint32_t)(frame + p_stk));
        else if (i->sub == 4) a6_movi(A, S0, p_nvrn);
        else a6_movi(A, S0, p_ngrn);
        stv(i->dst, S0);
        break;
    case IR_LOAD:
        if (i->w != 1 && i->w != 2 && i->w != 4 && i->w != 8) kerr(A->file, A->line, "aarch64 backend: unsupported load width %d", i->w);
        ldv(S0, i->a);
        a6_ls(A, 1, i->w, i->sgn && i->w < 8, S0, S0, 0);
        stv(i->dst, S0);
        break;
    case IR_STORE:
        if (i->w != 1 && i->w != 2 && i->w != 4 && i->w != 8) kerr(A->file, A->line, "aarch64 backend: unsupported store width %d", i->w);
        ldv(S0, i->a);
        ldv(S1, i->b);
        a6_ls(A, 0, i->w, 0, S1, S0, 0);
        break;
    case IR_MOV: ldv(S0, i->a); stv(i->dst, S0); break;
    case IR_BIN: gen_bin(i); break;
    case IR_NEG: ldv(S0, i->a); a6_arith(A, A6_SUB, S0, A6_ZR, S0); norm(S0, i->w, i->sgn); stv(i->dst, S0); break;
    case IR_NOT: ldv(S0, i->a); a6_logic(A, A6_ORN, S0, A6_ZR, S0); norm(S0, i->w, i->sgn); stv(i->dst, S0); break;
    case IR_CMP:
        ldv(S0, i->a); ldv(S1, i->b);
        a6_arith(A, A6_SUBS, A6_ZR, S0, S1);
        a6_cset(A, S0, ccode(i));
        stv(i->dst, S0);
        break;
    case IR_EXT: ldv(S0, i->a); norm(S0, i->w, i->sgn); stv(i->dst, S0); break;
    case IR_JMP: a6_b(A, (int)i->imm); break;
    case IR_BRZ: case IR_BRNZ: ldv(S0, i->a); a6_cbz(A, i->op == IR_BRNZ, S0, (int)i->imm); break;
    case IR_CALL: gen_call(i); break;
    case IR_RET:
        if (i->a > 0 && i->sub) {                    /* агрегат: до 16 байт в x0:x1, больше - в память по скрытому указателю (x8 при входе) */
            ldv(S5, i->a);
            if (i->w <= 16) {
                ld_reg_bytes(0, S5, 0, i->w > 8 ? 8 : i->w);
                if (i->w > 8) ld_reg_bytes(1, S5, 8, i->w - 8);
            } else {
                a6_ls(A, 1, 8, 0, S1, FB, hidoff);
                copy_bytes(S1, 0, S5, 0, i->w);
                a6_mov(A, 0, S1);
            }
        } else if (i->a > 0 && i->fp) { ldv(S0, i->a); a6_fmov_to(A, i->fp, 0, S0); }
        else if (i->a > 0) ldv(0, i->a);
        a6_b(A, epi);
        break;
    case IR_COPY: case IR_ZERO: {
        int cp = i->op == IR_COPY, n = (int)i->imm;
        ldv(S0, i->a);
        if (cp) ldv(S1, i->b);
        if (n > 64) {                                /* цикл по 8 байт, затем хвост */
            int top = tmplab++;
            a6_movi(A, S2, n / 8);
            a6_label(A, top);
            if (cp) { a6_ls(A, 1, 8, 0, S4, S1, 0); a6_addi(A, 0, S1, S1, 8); }
            a6_ls(A, 0, 8, 0, cp ? S4 : A6_ZR, S0, 0);
            a6_addi(A, 0, S0, S0, 8);
            a6_addi(A, 1, S2, S2, 1);
            a6_cbz(A, 1, S2, top);
            n %= 8;
        }
        if (cp) copy_bytes(S0, 0, S1, 0, n);
        else {
            int o = 0;
            while (n - o > 0) {
                int p = n - o >= 8 ? 8 : n - o >= 4 ? 4 : n - o >= 2 ? 2 : 1;
                a6_ls(A, 0, p, 0, A6_ZR, S0, o);
                o += p;
            }
        }
        break;
    }
    case IR_FBIN: case IR_FUN: case IR_FCMP: case IR_FCVT: gen_fp(i); break;
    case IR_ASM: gen_asm(i); break;
    case IR_UNREACH: a6_op0(A, Z_BRK0); break;
    }
}

static void gen_func(IrFn *fi) {
    Obj *f = fi->f;
    A->file = K.srcpath; A->line = f->line;
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
    int *labpos = ka_new(sizeof(int) * (size_t)(fi->nlab + 2));
    for (int l = 0; l <= fi->nlab + 1; l++) labpos[l] = -1;
    int64_t maxout = 0;
    hasvla = ir_uses_sp(fi);
    int nlab_extra = 0;
    for (int k = 0; k < n; k++) {
        gen_live(ins[k], k, nv, vs, ve);
        if (ins[k]->op == IR_LABEL) {
            if (ins[k]->imm < 0 || ins[k]->imm > fi->nlab) kerr(A->file, A->line, "internal: bad label");
            labpos[ins[k]->imm] = k;
        }
        if (ins[k]->op == IR_CALL) {
            ALoc *cl = ka_new(sizeof(ALoc) * (size_t)(ins[k]->nargs + 1));
            int ng, sp = plan_args(ins[k]->nargs, ins[k]->asz, ins[k]->afp, cl, &ng, NULL);
            if (sp > maxout) maxout = sp;
        }
        if (ins[k]->op == IR_ATOMIC) nlab_extra += 3;
        if (ins[k]->op == IR_COPY || ins[k]->op == IR_ZERO) nlab_extra++;
    }
    for (int changed = 1; changed;) {                 /* обратные переходы: живое в начале цикла живёт до конца цикла */
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
    int *order = ka_new(sizeof(int) * (size_t)(nv + 1)), no = 0;
    for (int v = 1; v <= nv; v++) if (ve[v] >= 0) order[no++] = v;
    qsort(order, (size_t)no, sizeof(int), cmp_start);
    int act[NPOOL] = { 0 }, used[NPOOL] = { 0 };
    for (int q = 0; q < no; q++) {
        int v = order[q], best = -1;
        for (int k = 0; k < NPOOL; k++) if (act[k] && ve[act[k]] <= vs[v]) act[k] = 0;
        for (int k = 0; k < NPOOL; k++) if (!act[k]) { best = k; break; }
        if (best < 0) {
            int far = 0;
            for (int k = 1; k < NPOOL; k++) if (ve[act[k]] > ve[act[far]]) far = k;
            if (ve[act[far]] > ve[v]) { rg[act[far]] = -1; best = far; }
        }
        if (best >= 0) { act[best] = v; rg[v] = pool[best]; used[best] = 1; }
    }
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
    /* параметры */
    p_hidden = ty_is_agg(f->ty->base) && ty_unqual(f->ty->base)->size > 16;
    {
        int np = f->nparams;
        ALoc *pl = ka_new(sizeof(ALoc) * (size_t)(np + 1));
        int *pa = ka_new(sizeof(int) * (size_t)(np + 1)), q = 0;
        unsigned char *pf = ka_new((size_t)np + 1);
        for (Obj *p = f->params; p && q < np; p = p->next, q++) {
            pa[q] = ty_is_agg(p->ty) ? ty_unqual(p->ty)->size : 0;
            pf[q] = ty_is_float(p->ty) ? (unsigned char)ty_unqual(p->ty)->size : 0;
        }
        p_stk = plan_args(np, pa, pf, pl, &p_ngrn, &p_nvrn);
        ploc = pl;
    }
    int nhome = f->ty->variadic ? 8 : p_ngrn;
    int nq = f->ty->variadic && !kcc_nosse ? 8 : p_nvrn;     /* сохраняемые v-регистры: все восемь для va_arg, иначе занятые параметрами */
    /* кадр: [исходящие аргументы][локальные][слоты спила][регистры параметров][x8][callee-saved][x29, x30] */
    maxout_cur = (maxout + 15) & ~(int64_t)15;
    int64_t cur = maxout_cur;
    int maxslot = -1;
    for (Obj *l = f->locals; l; l = l->next) if (l->slot > maxslot) maxslot = l->slot;
    slotoff = ka_new(sizeof(int) * (size_t)(maxslot + 2));
    for (Obj *l = f->locals; l; l = l->next) {
        int64_t sz = l->ty->size > 0 ? l->ty->size : 1;
        int al = l->align > l->ty->align ? l->align : l->ty->align;
        if (al < 1) al = 1;
        if (al > 16) kerr(K.srcpath, l->line ? l->line : f->line, "unsupported alignment %d of local '%s' (max 16)", al, l->name);
        cur = (cur + al - 1) & ~(int64_t)(al - 1);
        slotoff[l->slot] = (int)cur;
        cur += sz;
        if (cur > 0x3fffffff) kerr(K.srcpath, f->line, "stack frame of '%s' is too large", f->name);
    }
    cur = (cur + 7) & ~(int64_t)7;
    spillbase = cur;
    cur += 8 * (int64_t)nsl;
    homeoff = cur;
    if (nhome || nq) cur += 64;
    qoff = cur;
    cur += 16 * (int64_t)nq;
    hidoff = cur;
    if (p_hidden) cur += 8;
    int saveoff[NPOOL];
    for (int k = 0; k < NPOOL; k++) if (used[k]) { saveoff[k] = (int)cur; cur += 8; }
    int64_t fpoff = cur;
    cur += 16;
    frame = (cur + 15) & ~(int64_t)15;
    if (frame >= (1 << 24)) kerr(K.srcpath, f->line, "stack frame of '%s' is too large", f->name);
    epi = fi->nlab + 1;
    tmplab = epi + 1;
    a6_labels_begin(A, epi + 1 + nlab_extra);
    a6_addi(A, 1, A6_SP, A6_SP, (uint32_t)frame);
    a6_ls(A, 0, 8, 0, A6_FP, A6_SP, fpoff);
    a6_ls(A, 0, 8, 0, A6_LR, A6_SP, fpoff + 8);
    a6_addi(A, 0, A6_FP, A6_SP, hasvla ? 0u : (uint32_t)fpoff);
    for (int k = 0; k < NPOOL; k++) if (used[k]) a6_ls(A, 0, 8, 0, pool[k], A6_SP, saveoff[k]);
    for (int k = 0; k < nhome; k++) a6_ls(A, 0, 8, 0, k, A6_SP, homeoff + 8 * k);
    if (p_hidden) a6_ls(A, 0, 8, 0, 8, A6_SP, hidoff);
    if (nq) {
        a6_addi(A, 0, S0, A6_SP, (uint32_t)qoff);
        for (int k = 0; k < nq; k++) a6_strq(A, k, S0, 16 * k);
    }
    for (int k = 0; k < n; k++) gen_ins(ins[k]);
    A->file = K.srcpath; A->line = f->line;
    a6_label(A, epi);
    if (hasvla) a6_mov(A, A6_SP, A6_FP);                    /* sp после VLA возвращается к нижней границе кадра */
    for (int k = 0; k < NPOOL; k++) if (used[k]) a6_ls(A, 1, 8, 0, pool[k], A6_SP, saveoff[k]);
    a6_ls(A, 1, 8, 0, A6_FP, A6_SP, fpoff);
    a6_ls(A, 1, 8, 0, A6_LR, A6_SP, fpoff + 8);
    a6_addi(A, 0, A6_SP, A6_SP, (uint32_t)frame);
    a6_op0(A, Z_RET);
    a6_labels_end(A);
}

/* ---------- встроенные функции, если программа их не определяет (байтовые циклы) ---------- */
static void gen_helper(Obj *o) {
    A->file = K.srcpath; A->line = 1;
    o->addr = A->len;
    a6_labels_begin(A, 4);
    if (!strcmp(o->name, "memcpy")) {                /* x0 = dst, x1 = src, x2 = n */
        a6_mov(A, 3, 0);
        a6_label(A, 1); a6_cbz(A, 0, 2, 2);
        a6_ls(A, 1, 1, 0, 4, 1, 0); a6_ls(A, 0, 1, 0, 4, 3, 0);
        a6_addi(A, 0, 1, 1, 1); a6_addi(A, 0, 3, 3, 1); a6_addi(A, 1, 2, 2, 1);
        a6_b(A, 1);
        a6_label(A, 2); a6_op0(A, Z_RET);
    } else if (!strcmp(o->name, "memset")) {         /* x0 = dst, x1 = byte, x2 = n */
        a6_mov(A, 3, 0);
        a6_label(A, 1); a6_cbz(A, 0, 2, 2);
        a6_ls(A, 0, 1, 0, 1, 3, 0);
        a6_addi(A, 0, 3, 3, 1); a6_addi(A, 1, 2, 2, 1);
        a6_b(A, 1);
        a6_label(A, 2); a6_op0(A, Z_RET);
    } else if (!strcmp(o->name, "memcmp")) {         /* x0, x1, x2 = n */
        a6_label(A, 1); a6_cbz(A, 0, 2, 2);
        a6_ls(A, 1, 1, 0, 3, 0, 0); a6_ls(A, 1, 1, 0, 4, 1, 0);
        a6_arith(A, A6_SUB, 3, 3, 4); a6_cbz(A, 1, 3, 3);
        a6_addi(A, 0, 0, 0, 1); a6_addi(A, 0, 1, 1, 1); a6_addi(A, 1, 2, 2, 1);
        a6_b(A, 1);
        a6_label(A, 2); a6_movi(A, 0, 0); a6_op0(A, Z_RET);
        a6_label(A, 3); a6_mov(A, 0, 3); a6_op0(A, Z_RET);
    } else {                                         /* strlen */
        a6_mov(A, 1, 0);
        a6_label(A, 1); a6_ls(A, 1, 1, 0, 2, 1, 0); a6_cbzw(A, 0, 2, 2);
        a6_addi(A, 0, 1, 1, 1); a6_b(A, 1);
        a6_label(A, 2); a6_arith(A, A6_SUB, 0, 1, 0); a6_op0(A, Z_RET);
    }
    a6_labels_end(A);
}

/* ---------- разрешение ссылок в тексте ---------- */
static void wr32(unsigned char *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (unsigned char)(v >> (8 * k)); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void a64_reloc(unsigned char *text, void *ctx) {
    (void)ctx;
    uint64_t base = elf_base(ELF_ARCH_A64);
    for (A6Fix *f = xa.fix; f; f = f->next) {
        Obj *t = f->target;
        if (!t->emit) kerr(K.srcpath, f->line, "undefined reference to '%s'", t->name);
        uint64_t s = t->addr + (uint64_t)f->add, pc = base + (uint64_t)f->off;
        uint32_t w = rd32(text + f->off);
        if (f->kind == 1) w |= (uint32_t)(s & 0xfff) << 10;
        else if (f->kind == 0) {
            int64_t pg = (int64_t)(s >> 12) - (int64_t)(pc >> 12);
            if (pg < -(1 << 20) || pg >= (1 << 20)) kerr(K.srcpath, f->line, "relocation to '%s' is out of range", t->name);
            w |= ((uint32_t)pg & 3) << 29 | (((uint32_t)pg >> 2) & 0x7ffff) << 5;
        } else {
            int64_t d = ((int64_t)t->addr - (int64_t)pc) / 4;
            if (d < -(1 << 25) || d >= (1 << 25)) kerr(K.srcpath, f->line, "call to '%s' is out of range", t->name);
            w |= (uint32_t)d & 0x3ffffff;
        }
        wr32(text + f->off, w);
    }
}

void a64_build_elf(Obj *globals, unsigned char **out, size_t *outlen) {
    if (!K.a64_state) K.a64_state = ka_new(sizeof(A64State));
    memset(A64S, 0, sizeof *A64S);
    Obj *mainf = gen_prepare(globals);
    /* _start: main(0, 0); код выхода - в K_EXIT (номер 2, x8) */
    A->file = K.srcpath; A->line = 1;
    a6_labels_begin(A, 0);
    a6_mov(A, S0, A6_SP);
    a6_movi(A, S1, -16);
    a6_logic(A, A6_AND, S0, S0, S1);
    a6_mov(A, A6_SP, S0);
    a6_movi(A, A6_FP, 0);
    a6_movi(A, 0, 0);
    a6_movi(A, 1, 0);
    a6_bl(A, mainf);
    a6_movi(A, 8, 2);
    a6_op0(A, Z_SVC0);
    a6_op0(A, Z_BRK0);
    a6_labels_end(A);
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && g->body && g->emit) gen_func((IrFn *)g->ir);
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && !g->body && g->emit && gen_is_helper_name(g->name)) gen_helper(g);
    elf_emit(globals, A->p, A->len, ELF_ARCH_A64, a64_reloc, NULL, out, outlen);
}
