/* kcc: кодогенерация x86-64 из линейного IR (System V ABI для вызовов).
 * Каждая функция: живые интервалы vreg -> линейный проход по пяти callee-saved регистрам (rbx, r12-r15), остальное - в стековые
 * слоты. Рабочие регистры (rax, rcx, rdx, rsi, rdi, r8-r11) свободны для кодогенератора и ничего не хранят между инструкциями IR.
 * Достижимость: в образ попадают main, функции и данные с внешней видимостью и всё, на что они ссылаются; статические
 * неиспользуемые функции отбрасываются (как --gc-sections). Неопределённый символ - ошибка. */
#include "x86.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static XA xa;
#define A (&xa)

/* ---------- таблица имён ---------- */
static Obj **htab;
static unsigned hcap;
static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static void hput(Obj *o) {
    for (unsigned i = hash(o->name) & (hcap - 1);; i = (i + 1) & (hcap - 1)) {
        if (!htab[i]) { htab[i] = o; return; }
        if (!strcmp(htab[i]->name, o->name)) {
            if ((o->is_func ? o->body != NULL : o->is_def) && !(htab[i]->is_func ? htab[i]->body != NULL : htab[i]->is_def)) htab[i] = o;
            return;
        }
    }
}
Obj *gen_lookup(const char *name) {
    for (unsigned i = hash(name) & (hcap - 1); htab[i]; i = (i + 1) & (hcap - 1))
        if (!strcmp(htab[i]->name, name)) return htab[i];
    return NULL;
}

static int is_defined(const Obj *o) { return o->is_func ? o->body != NULL : o->is_def; }
static int is_helper_name(const char *n) { return !strcmp(n, "memcpy") || !strcmp(n, "memset") || !strcmp(n, "memcmp") || !strcmp(n, "strlen"); }

/* ---------- достижимость ---------- */
static Obj **work; static int nwork;

static void mark_ref(const Obj *from, const char *name) {
    Obj *o = gen_lookup(name);
    if (!o || (!is_defined(o) && !(o->is_func && is_helper_name(o->name))))
        kerr(K.srcpath, from ? from->line : 1, "undefined reference to '%s'", name);
    if (o->emit) return;
    o->emit = 1;
    work[nwork++] = o;
}

static void scan_refs(Obj *o) {
    if (o->is_func) {
        if (!o->body) return;
        for (Ins *i = ((IrFn *)o->ir)->head; i; i = i->next)
            if ((i->op == IR_GADDR || i->op == IR_CALL) && i->sym) mark_ref(o, i->sym);
    } else {
        for (Reloc *r = o->relocs; r; r = r->next) mark_ref(o, r->sym);
    }
}

/* ---------- состояние функции ---------- */
static const int pool[5] = { R_BX, R_12, R_13, R_14, R_15 };
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

static int cmp_start(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (vs[x] != vs[y]) return vs[x] < vs[y] ? -1 : 1;
    return x - y;
}

static void touch(int v, int idx, int nv) {
    if (v <= 0) return;
    if (v > nv) kerr(A->file, A->line, "internal: bad vreg %d", v);
    if (idx < vs[v]) vs[v] = idx;
    if (idx > ve[v]) ve[v] = idx;
}
static void touch_ins(const Ins *i, int idx, int nv) {
    switch (i->op) {
    case IR_CONST: case IR_GADDR: case IR_LADDR: case IR_VAINFO: touch(i->dst, idx, nv); break;
    case IR_PARAM: touch(i->dst, idx, nv); touch(i->a, idx, nv); break;
    case IR_ATOMIC: touch(i->a, idx, nv); touch(i->b, idx, nv); touch(i->c, idx, nv); touch(i->dst, idx, nv); break;
    case IR_LOAD: case IR_MOV: case IR_NEG: case IR_NOT: case IR_EXT: touch(i->a, idx, nv); touch(i->dst, idx, nv); break;
    case IR_STORE: case IR_COPY: touch(i->a, idx, nv); touch(i->b, idx, nv); break;
    case IR_BIN: case IR_CMP: touch(i->a, idx, nv); touch(i->b, idx, nv); touch(i->dst, idx, nv); break;
    case IR_BRZ: case IR_BRNZ: case IR_ZERO: touch(i->a, idx, nv); break;
    case IR_RET: touch(i->a, idx, nv); break;
    case IR_CALL:
        if (!i->sym) touch(i->a, idx, nv);
        for (int k = 0; k < i->nargs; k++) touch(i->args[k], idx, nv);
        touch(i->rt, idx, nv);
        touch(i->dst, idx, nv);
        break;
    case IR_ASM: {
        const int *o = (const int *)(intptr_t)i->imm;
        for (int k = 0; k < i->nargs; k++) touch(i->args[k], idx, nv);
        for (int k = 0; k < i->nout; k++) touch(o[k], idx, nv);
        break;
    }
    default: break;
    }
}

/* ---------- примитивы ---------- */
static Opd R64(int r) { return xr(8, r); }
static Opd spill(int v) { return xm(8, R_BP, -(int64_t)(spillbase + 8 * (so[v] + 1))); }

static void ldv(int r, int v) {
    if (v <= 0) kerr(A->file, A->line, "internal: use of an undefined vreg");
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
static void norm(int w, int sgn) {
    if (w == 4) { if (sgn) xa_ins(A, X_MOVSX, R64(R_AX), xr(4, R_AX)); else xa_ins(A, X_MOV, xr(4, R_AX), xr(4, R_AX)); }
    else if (w == 2 || w == 1) xa_ins(A, sgn ? X_MOVSX : X_MOVZX, R64(R_AX), xr(w, R_AX));
}

static void mov_imm(int r, int64_t v) {
    if (v >= 0 && v <= (int64_t)UINT32_MAX) xa_ins(A, X_MOV, xr(4, r), xi(v));
    else xa_ins(A, X_MOV, R64(r), xi(v));
}

static void gen_bin(const Ins *i) {
    ldv(R_AX, i->a);
    ldv(R_CX, i->b);
    switch (i->sub) {
    case ND_ADD: xa_ins(A, X_ADD, R64(R_AX), R64(R_CX)); break;
    case ND_SUB: xa_ins(A, X_SUB, R64(R_AX), R64(R_CX)); break;
    case ND_MUL: xa_ins(A, X_IMUL, R64(R_AX), R64(R_CX)); break;
    case ND_AND: xa_ins(A, X_AND, R64(R_AX), R64(R_CX)); break;
    case ND_OR: xa_ins(A, X_OR, R64(R_AX), R64(R_CX)); break;
    case ND_XOR: xa_ins(A, X_XOR, R64(R_AX), R64(R_CX)); break;
    case ND_SHL: xa_ins(A, X_SHL, R64(R_AX), xr(1, R_CX)); break;
    case ND_SHR: xa_ins(A, i->sgn ? X_SAR : X_SHR, R64(R_AX), xr(1, R_CX)); break;
    case ND_DIV: case ND_MOD:
        if (i->sgn) { xa_op0(A, X0_CQO); xa_ins(A, X_IDIV, R64(R_CX), NOOPD); }
        else { xa_ins(A, X_XOR, xr(4, R_DX), xr(4, R_DX)); xa_ins(A, X_DIV, R64(R_CX), NOOPD); }
        if (i->sub == ND_MOD) xa_ins(A, X_MOV, R64(R_AX), R64(R_DX));
        break;
    default: kerr(A->file, A->line, "internal: bad binary operator %d", i->sub);
    }
    norm(i->w, i->sgn);
    stv(i->dst, R_AX);
}

static void gen_cmp(const Ins *i) {
    ldv(R_AX, i->a);
    ldv(R_CX, i->b);
    xa_ins(A, X_CMP, R64(R_AX), R64(R_CX));
    int cc;
    switch (i->sub) {
    case ND_EQ: cc = CC_E; break;
    case ND_NE: cc = CC_NE; break;
    case ND_LT: cc = i->sgn ? CC_L : CC_B; break;
    case ND_LE: cc = i->sgn ? CC_LE : CC_BE; break;
    default: kerr(A->file, A->line, "internal: bad comparison %d", i->sub); return;
    }
    xa_setcc(A, cc, xr(1, R_AX));
    xa_ins(A, X_MOVZX, xr(4, R_AX), xr(1, R_AX));
    stv(i->dst, R_AX);
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
    else kerr(A->file, A->line, "inline asm template '%s' is not supported on x86_64", t);
}

static void gen_ins(const Ins *i, int homebase_h) {
    switch (i->op) {
    case IR_LABEL: xa_label(A, (int)i->imm); break;
    case IR_CONST:
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
        else xa_ins(A, X_MOV, xr(4, R_AX), xi(p_ngrn));
        stv(i->dst, R_AX);
        break;
    case IR_LOAD:
        ldv(R_AX, i->a);
        if (i->w == 8) xa_ins(A, X_MOV, R64(R_AX), xm(8, R_AX, 0));
        else if (i->w == 4) { if (i->sgn) xa_ins(A, X_MOVSX, R64(R_AX), xm(4, R_AX, 0)); else xa_ins(A, X_MOV, xr(4, R_AX), xm(4, R_AX, 0)); }
        else if (i->w == 2 || i->w == 1) xa_ins(A, i->sgn ? X_MOVSX : X_MOVZX, R64(R_AX), xm(i->w, R_AX, 0));
        else kerr(A->file, A->line, "x86-64 backend: unsupported load width %d", i->w);
        stv(i->dst, R_AX);
        break;
    case IR_STORE:
        if (i->w != 1 && i->w != 2 && i->w != 4 && i->w != 8) kerr(A->file, A->line, "x86-64 backend: unsupported store width %d", i->w);
        ldv(R_AX, i->a);
        ldv(R_CX, i->b);
        xa_ins(A, X_MOV, xm(i->w, R_AX, 0), xr(i->w, R_CX));
        break;
    case IR_MOV: ldv(R_AX, i->a); stv(i->dst, R_AX); break;
    case IR_BIN: gen_bin(i); break;
    case IR_NEG: ldv(R_AX, i->a); xa_ins(A, X_NEG, R64(R_AX), NOOPD); norm(i->w, i->sgn); stv(i->dst, R_AX); break;
    case IR_NOT: ldv(R_AX, i->a); xa_ins(A, X_NOT, R64(R_AX), NOOPD); norm(i->w, i->sgn); stv(i->dst, R_AX); break;
    case IR_CMP: gen_cmp(i); break;
    case IR_EXT: ldv(R_AX, i->a); norm(i->w, i->sgn); stv(i->dst, R_AX); break;
    case IR_JMP: xa_jmp(A, CC_NONE, (int)i->imm); break;
    case IR_BRZ: case IR_BRNZ:
        ldv(R_AX, i->a);
        xa_ins(A, X_TEST, R64(R_AX), R64(R_AX));
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
    int *labpos = ka_new(sizeof(int) * (size_t)(fi->nlab + 2));
    for (int l = 0; l <= fi->nlab + 1; l++) labpos[l] = -1;
    int maxout = 0, natom = 0;
    for (int k = 0; k < n; k++) {
        touch_ins(ins[k], k, nv);
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
    /* обратные переходы: всё, что живёт в начале цикла, живёт до конца цикла */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int k = 0; k < n; k++) {
            const Ins *j = ins[k];
            if (j->op != IR_JMP && j->op != IR_BRZ && j->op != IR_BRNZ) continue;
            if (j->imm < 0 || j->imm > fi->nlab || labpos[j->imm] < 0) kerr(A->file, A->line, "internal: jump to undefined label %d", (int)j->imm);
            int l = labpos[j->imm];
            if (l > k) continue;
            for (int v = 1; v <= nv; v++)
                if (ve[v] >= l && vs[v] < l && ve[v] < k) { ve[v] = k; changed = 1; }
        }
    }
    /* линейный проход */
    int *order = ka_new(sizeof(int) * (size_t)(nv + 1)), no = 0;
    for (int v = 1; v <= nv; v++) if (ve[v] >= 0) order[no++] = v;
    qsort(order, (size_t)no, sizeof(int), cmp_start);
    int act[5] = { 0, 0, 0, 0, 0 }, used[5] = { 0, 0, 0, 0, 0 };
    for (int q = 0; q < no; q++) {
        int v = order[q], best = -1;
        for (int k = 0; k < 5; k++) if (act[k] && ve[act[k]] <= vs[v]) act[k] = 0;
        for (int k = 0; k < 5; k++) if (!act[k]) { best = k; break; }
        if (best < 0) {
            int far = 0;
            for (int k = 1; k < 5; k++) if (ve[act[k]] > ve[act[far]]) far = k;
            if (ve[act[far]] > ve[v]) { rg[act[far]] = -1; best = far; }
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
    int saveoff[5];
    for (int k = 0; k < 5; k++) if (used[k]) { cur += 8; saveoff[k] = (int)cur; }
    p_hidden = ty_is_agg(f->ty->base) && ty_unqual(f->ty->base)->size > 16;
    {
        int np = f->nparams;
        ALoc *pl = ka_new(sizeof(ALoc) * (size_t)(np + 1));
        int *pa = ka_new(sizeof(int) * (size_t)(np + 1)), q = 0;
        for (Obj *p = f->params; p && q < np; p = p->next, q++) pa[q] = ty_is_agg(p->ty) ? ty_unqual(p->ty)->size : 0;
        p_stk = plan_args(np, pa, p_hidden, pl, &p_ngrn);
        ploc = pl;
    }
    int nh = (f->nparams > 0 || f->ty->variadic || p_hidden) ? 6 : 0;
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
    for (int k = 0; k < 5; k++) if (used[k]) xa_ins(A, X_MOV, xm(8, R_BP, -saveoff[k]), R64(pool[k]));
    for (int k = 0; k < nh; k++) xa_ins(A, X_MOV, xm(8, R_BP, -(int64_t)homeh + 8 * k), R64(argreg[k]));
    for (int k = 0; k < n; k++) gen_ins(ins[k], homeh);
    A->file = K.srcpath; A->line = f->line;
    xa_label(A, epi);
    for (int k = 0; k < 5; k++) if (used[k]) xa_ins(A, X_MOV, R64(pool[k]), xm(8, R_BP, -saveoff[k]));
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
    } else {
        xa_ins(A, X_MOV, R64(R_AX), R64(R_DI));
        xa_label(A, 1);
        xa_ins(A, X_CMP, xm(1, R_AX, 0), xi(0)); xa_jmp(A, CC_E, 2);
        xa_ins(A, X_ADD, R64(R_AX), xi(1)); xa_jmp(A, CC_NONE, 1);
        xa_label(A, 2); xa_ins(A, X_SUB, R64(R_AX), R64(R_DI)); xa_op0(A, X0_RET);
    }
    xa_labels_end(A);
}

void x86_build_elf(Obj *globals, unsigned char **out, size_t *outlen) {
    memset(&xa, 0, sizeof xa);
    int ng = 0;
    for (Obj *g = globals; g; g = g->next) ng++;
    hcap = 16;
    while (hcap < (unsigned)ng * 2 + 16) hcap <<= 1;
    htab = ka_new(sizeof(Obj *) * hcap);
    for (Obj *g = globals; g; g = g->next) hput(g);
    for (Obj *g = globals; g; g = g->next) {
        if (g->is_func && g->body) {
            A->file = K.srcpath; A->line = g->line;
            g->ir = ir_lower(g);
        }
    }
    work = ka_new(sizeof(Obj *) * (size_t)(ng + 1));
    nwork = 0;
    Obj *mainf = gen_lookup("main");
    if (!mainf || !mainf->is_func || !mainf->body) kerr(K.srcpath, 1, "undefined reference to 'main'");
    mark_ref(NULL, "main");
    for (Obj *g = globals; g; g = g->next)
        if (!g->is_static && is_defined(g) && !g->emit && hcap && gen_lookup(g->name) == g) mark_ref(g, g->name);
    while (nwork) scan_refs(work[--nwork]);
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
        if (g->is_func && !g->body && g->emit && is_helper_name(g->name)) gen_helper(g);
    elf_emit(globals, A, out, outlen);
}
