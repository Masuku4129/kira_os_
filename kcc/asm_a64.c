/* kcc: собственный кодировщик инструкций AArch64 (A64) - только подмножество, нужное кодогенератору и встроенному asm.
 * Сочетание операндов, для которого нет шаблона, - ошибка компиляции с номером строки, не молчаливо неверный код.
 * Кодировки сверяются с llvm-mc (tests/kcc/a64_enc.c, tests/kcc/a64_ref.txt). */
#include "a64.h"
#include <string.h>

static void fail(A6 *a, const char *what) { kerr(a->file, a->line, "AArch64 backend has no encoding for %s", what); }
static void chk(A6 *a, int r, const char *what) { if (r < 0 || r > 31) fail(a, what); }

void a6_w(A6 *a, uint32_t w) {
    if (a->len + 4 > a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 65536;
        unsigned char *np = ka_new(nc);
        if (a->len) memcpy(np, a->p, a->len);
        a->p = np; a->cap = nc;
    }
    for (int i = 0; i < 4; i++) a->p[a->len++] = (unsigned char)(w >> (8 * i));
}

void a6_movi(A6 *a, int rd, int64_t v) {
    chk(a, rd, "mov imm");
    uint64_t u = (uint64_t)v;
    int z = 0, o = 0;                                   /* число полуслов 0x0000 и 0xffff */
    for (int h = 0; h < 4; h++) { uint16_t p = (uint16_t)(u >> (16 * h)); if (p == 0) z++; if (p == 0xffff) o++; }
    int neg = o > z;
    int first = 1;
    for (int h = 0; h < 4; h++) {
        uint16_t p = (uint16_t)(u >> (16 * h));
        if (neg ? p == 0xffff : p == 0) continue;
        if (first) {
            if (neg) a6_w(a, 0x92800000u | (uint32_t)h << 21 | (uint32_t)(uint16_t)~p << 5 | (uint32_t)rd);          /* movn */
            else a6_w(a, 0xD2800000u | (uint32_t)h << 21 | (uint32_t)p << 5 | (uint32_t)rd);                          /* movz */
            first = 0;
        } else a6_w(a, 0xF2800000u | (uint32_t)h << 21 | (uint32_t)p << 5 | (uint32_t)rd);                            /* movk */
    }
    if (first) a6_w(a, neg ? 0x92800000u | (uint32_t)rd : 0xD2800000u | (uint32_t)rd);                              /* 0 или -1 */
}

void a6_mov(A6 *a, int rd, int rm) {
    chk(a, rd, "mov"); chk(a, rm, "mov");
    if (rd == rm) return;
    if (rd == A6_SP || rm == A6_SP) { a6_w(a, 0x91000000u | (uint32_t)rm << 5 | (uint32_t)rd); return; }   /* add rd, rm, #0 */
    a6_w(a, 0xAA0003E0u | (uint32_t)rm << 16 | (uint32_t)rd);                                               /* orr rd, xzr, rm */
}

void a6_arith(A6 *a, int op, int rd, int rn, int rm) {
    chk(a, rd, "add/sub"); chk(a, rn, "add/sub"); chk(a, rm, "add/sub");
    static const uint32_t base[4] = { 0x8B000000u, 0xCB000000u, 0xAB000000u, 0xEB000000u };
    if (op < 0 || op > 3) fail(a, "add/sub kind");
    a6_w(a, base[op] | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}

void a6_addr(A6 *a, int rd, int rn, int rm) {
    chk(a, rd, "add"); chk(a, rn, "add"); chk(a, rm, "add");
    a6_w(a, 0x8B206000u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);                           /* add rd, rn, rm, uxtx */
}

void a6_addi(A6 *a, int sub, int rd, int rn, uint32_t imm) {
    chk(a, rd, "add imm"); chk(a, rn, "add imm");
    if (imm >= (1u << 24)) fail(a, "add/sub with immediate >= 16M");
    uint32_t base = sub ? 0xD1000000u : 0x91000000u;
    uint32_t hi = imm >> 12, lo = imm & 0xfff;
    if (hi) { a6_w(a, base | 1u << 22 | hi << 10 | (uint32_t)rn << 5 | (uint32_t)rd); rn = rd; }
    if (lo || !hi) a6_w(a, base | lo << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}

void a6_cmpi(A6 *a, int rn, uint32_t imm) {
    chk(a, rn, "cmp imm");
    if (imm >= 4096) fail(a, "cmp with immediate >= 4096");
    a6_w(a, 0xF100001Fu | imm << 10 | (uint32_t)rn << 5);
}

void a6_logic(A6 *a, int op, int rd, int rn, int rm) {
    chk(a, rd, "logic"); chk(a, rn, "logic"); chk(a, rm, "logic");
    static const uint32_t base[5] = { 0x8A000000u, 0xAA000000u, 0xCA000000u, 0xEA000000u, 0xAA200000u };
    if (op < 0 || op > 4) fail(a, "logic kind");
    a6_w(a, base[op] | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}

void a6_mul(A6 *a, int rd, int rn, int rm) { a6_w(a, 0x9B007C00u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd); }
void a6_msub(A6 *a, int rd, int rn, int rm, int ra) { a6_w(a, 0x9B008000u | (uint32_t)rm << 16 | (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd); }
void a6_div(A6 *a, int sgn, int rd, int rn, int rm) { a6_w(a, (sgn ? 0x9AC00C00u : 0x9AC00800u) | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd); }
void a6_shv(A6 *a, int kind, int rd, int rn, int rm) {
    static const uint32_t base[3] = { 0x9AC02000u, 0x9AC02400u, 0x9AC02800u };
    if (kind < 0 || kind > 2) fail(a, "shift kind");
    a6_w(a, base[kind] | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}

void a6_shi(A6 *a, int kind, int rd, int rn, int n) {
    chk(a, rd, "shift"); chk(a, rn, "shift");
    if (n < 0 || n > 63) fail(a, "shift by immediate out of range");
    uint32_t r = (uint32_t)n, rd32 = (uint32_t)rd | (uint32_t)rn << 5;
    if (kind == A6_LSLV) a6_w(a, 0xD3400000u | ((64u - r) & 63) << 16 | (63u - r) << 10 | rd32);              /* ubfm x, x, #(-n mod 64), #(63-n) */
    else if (kind == A6_LSRV) a6_w(a, 0xD340FC00u | r << 16 | rd32);                                           /* ubfm x, x, #n, #63 */
    else a6_w(a, 0x9340FC00u | r << 16 | rd32);                                                                /* sbfm x, x, #n, #63 */
}

void a6_ext(A6 *a, int rd, int rn, int w, int sgn) {
    chk(a, rd, "extend"); chk(a, rn, "extend");
    uint32_t rr = (uint32_t)rd | (uint32_t)rn << 5;
    if (w != 1 && w != 2 && w != 4) fail(a, "extend width");
    uint32_t imms = w == 1 ? 7u : w == 2 ? 15u : 31u;
    if (sgn) a6_w(a, 0x93400000u | imms << 10 | rr);                                                           /* sxtb/sxth/sxtw -> x */
    else if (w == 4) a6_w(a, 0x2A0003E0u | (uint32_t)rn << 16 | (uint32_t)rd);                                  /* mov wd, wn */
    else a6_w(a, 0x53000000u | imms << 10 | rr);                                                               /* uxtb/uxth -> w */
}

void a6_ls(A6 *a, int load, int size, int sgnext, int rt, int rn, int64_t off) {
    chk(a, rt, "ldr/str"); chk(a, rn, "ldr/str");
    uint32_t sz = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : size == 8 ? 3 : 9;
    if (sz == 9) fail(a, "load/store width");
    uint32_t opc = !load ? 0 : sgnext && size < 8 ? 2 : 1;
    if (sgnext && !load) fail(a, "signed store");
    int64_t unit = size;
    if (off >= 0 && off % unit == 0 && off / unit < 4096) {
        a6_w(a, sz << 30 | 0x39000000u | opc << 22 | (uint32_t)(off / unit) << 10 | (uint32_t)rn << 5 | (uint32_t)rt);
        return;
    }
    if (off >= -256 && off <= 255) {
        a6_w(a, sz << 30 | 0x38000000u | opc << 22 | ((uint32_t)off & 0x1ff) << 12 | (uint32_t)rn << 5 | (uint32_t)rt);
        return;
    }
    a6_movi(a, 17, off);                                                                                       /* x17 - служебный */
    a6_addr(a, 17, rn, 17);
    a6_w(a, sz << 30 | 0x39000000u | opc << 22 | 17u << 5 | (uint32_t)rt);
}

void a6_cset(A6 *a, int rd, int cc) {
    chk(a, rd, "cset");
    if (cc < 0 || cc > 13 || cc == 5 || cc == 6 || cc == 7) fail(a, "condition");
    a6_w(a, 0x1A9F07E0u | ((uint32_t)cc ^ 1u) << 12 | (uint32_t)rd);
}

void a6_xmem(A6 *a, int kind, int size, int rt, int rn, int rs) {
    chk(a, rt, "exclusive"); chk(a, rn, "exclusive"); chk(a, rs, "exclusive");
    uint32_t sz = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : size == 8 ? 3 : 9;
    if (sz == 9) fail(a, "exclusive width");
    static const uint32_t base[4] = { 0x08DFFC00u, 0x085FFC00u, 0x089FFC00u, 0x0800FC00u };
    if (kind < 0 || kind > 3) fail(a, "exclusive kind");
    a6_w(a, sz << 30 | base[kind] | (kind == X_STLXR ? (uint32_t)rs << 16 : 0) | (uint32_t)rn << 5 | (uint32_t)rt);
}

void a6_op0(A6 *a, int op) {
    switch (op) {
    case Z_NOP: a6_w(a, 0xD503201Fu); return;
    case Z_YIELD: a6_w(a, 0xD503203Fu); return;
    case Z_WFE: a6_w(a, 0xD503205Fu); return;
    case Z_WFI: a6_w(a, 0xD503207Fu); return;
    case Z_SEV: a6_w(a, 0xD503209Fu); return;
    case Z_SVC0: a6_w(a, 0xD4000001u); return;
    case Z_BRK0: a6_w(a, 0xD4200000u); return;
    case Z_ISB: a6_w(a, 0xD5033FDFu); return;
    case Z_CLREX: a6_w(a, 0xD5033F5Fu); return;
    case Z_RET: a6_w(a, 0xD65F03C0u); return;
    default: fail(a, "zero-operand instruction");
    }
}

void a6_bar(A6 *a, int dsb, int crm) {
    if (crm < 0 || crm > 15) fail(a, "barrier option");
    a6_w(a, (dsb ? 0xD503309Fu : 0xD50330BFu) | (uint32_t)crm << 8);
}

void a6_mrs(A6 *a, int rt, int sr) {
    chk(a, rt, "mrs");
    if (sr == SR_CNTVCT) a6_w(a, 0xD53BE040u | (uint32_t)rt);
    else if (sr == SR_CNTFRQ) a6_w(a, 0xD53BE000u | (uint32_t)rt);
    else fail(a, "system register");
}

void a6_blr(A6 *a, int rn) { chk(a, rn, "blr"); a6_w(a, 0xD63F0000u | (uint32_t)rn << 5); }

/* ---------- метки и переходы ---------- */
void a6_labels_begin(A6 *a, int nlab) {
    a->nlab = nlab + 1;
    a->lab = ka_new(sizeof(int) * (size_t)a->nlab);
    for (int i = 0; i < a->nlab; i++) a->lab[i] = -1;
    a->njf = 0;
}
void a6_label(A6 *a, int lab) {
    if (lab < 0 || lab >= a->nlab) kerr(a->file, a->line, "internal: label %d out of range", lab);
    a->lab[lab] = (int)a->len;
}
static void jump(A6 *a, uint32_t w, int kind, int lab) {
    if (a->njf == a->cjf) {
        int nc = a->cjf ? a->cjf * 2 : 256;
        A6Jmp *n = ka_new(sizeof(A6Jmp) * (size_t)nc);
        if (a->njf) memcpy(n, a->jf, sizeof(A6Jmp) * (size_t)a->njf);
        a->jf = n; a->cjf = nc;
    }
    a->jf[a->njf].off = (int)a->len; a->jf[a->njf].lab = lab; a->jf[a->njf].kind = kind; a->njf++;
    a6_w(a, w);
}
void a6_b(A6 *a, int lab) { jump(a, 0x14000000u, 0, lab); }
void a6_bcc(A6 *a, int cc, int lab) {
    if (cc < 0 || cc > 13) fail(a, "condition");
    jump(a, 0x54000000u | (uint32_t)cc, 1, lab);
}
void a6_cbz(A6 *a, int nz, int rt, int lab) { chk(a, rt, "cbz"); jump(a, (nz ? 0xB5000000u : 0xB4000000u) | (uint32_t)rt, 1, lab); }
void a6_cbzw(A6 *a, int nz, int rt, int lab) { chk(a, rt, "cbz"); jump(a, (nz ? 0x35000000u : 0x34000000u) | (uint32_t)rt, 1, lab); }

static void add_fix(A6 *a, Obj *t, int64_t add, int kind) {
    A6Fix *f = ka_new(sizeof *f);
    f->off = (int)a->len; f->target = t; f->add = add; f->line = a->line; f->kind = kind;
    if (a->fixtail) a->fixtail->next = f; else a->fix = f;
    a->fixtail = f;
}
void a6_bl(A6 *a, Obj *t) { add_fix(a, t, 0, 2); a6_w(a, 0x94000000u); }
void a6_gaddr(A6 *a, int rd, Obj *t, int64_t add) {
    chk(a, rd, "adrp");
    add_fix(a, t, add, 0); a6_w(a, 0x90000000u | (uint32_t)rd);
    add_fix(a, t, add, 1); a6_w(a, 0x91000000u | (uint32_t)rd << 5 | (uint32_t)rd);
}

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }

void a6_labels_end(A6 *a) {
    for (int i = 0; i < a->njf; i++) {
        int l = a->jf[i].lab, off = a->jf[i].off;
        if (l < 0 || l >= a->nlab || a->lab[l] < 0) kerr(a->file, a->line, "internal: jump to undefined label %d", l);
        int64_t d = ((int64_t)a->lab[l] - off) / 4;
        uint32_t w = rd32(a->p + off);
        if (a->jf[i].kind == 0) {
            if (d < -(1 << 25) || d >= (1 << 25)) kerr(a->file, a->line, "branch out of range");
            w |= (uint32_t)d & 0x3ffffff;
        } else {
            if (d < -(1 << 18) || d >= (1 << 18)) kerr(a->file, a->line, "conditional branch out of range (function too large)");
            w |= ((uint32_t)d & 0x7ffff) << 5;
        }
        wr32(a->p + off, w);
    }
    a->njf = 0;
}

/* ---------- скалярные вещественные инструкции: sz = 4 (s-регистры) либо 8 (d-регистры); fN - номер v-регистра ---------- */
static uint32_t ftype(A6 *a, int sz) {
    if (sz != 4 && sz != 8) fail(a, "floating-point width");
    return sz == 8 ? 1u << 22 : 0;
}

void a6_fmov_to(A6 *a, int sz, int fd, int rn) {                     /* fmov dD, xN / fmov sD, wN */
    chk(a, fd, "fmov"); chk(a, rn, "fmov");
    a6_w(a, (sz == 8 ? 0x9E670000u : 0x1E270000u) | (uint32_t)rn << 5 | (uint32_t)fd);
}

void a6_fmov_from(A6 *a, int sz, int rd, int fn) {                   /* fmov xD, dN / fmov wD, sN */
    chk(a, rd, "fmov"); chk(a, fn, "fmov");
    a6_w(a, (sz == 8 ? 0x9E660000u : 0x1E260000u) | (uint32_t)fn << 5 | (uint32_t)rd);
}

void a6_fop(A6 *a, int op, int sz, int fd, int fn, int fm) {
    chk(a, fd, "fp op"); chk(a, fn, "fp op"); chk(a, fm, "fp op");
    uint32_t t = ftype(a, sz);
    switch (op) {
    case FA_ADD: a6_w(a, 0x1E202800u | t | (uint32_t)fm << 16 | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_SUB: a6_w(a, 0x1E203800u | t | (uint32_t)fm << 16 | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_MUL: a6_w(a, 0x1E200800u | t | (uint32_t)fm << 16 | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_DIV: a6_w(a, 0x1E201800u | t | (uint32_t)fm << 16 | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_NEG: a6_w(a, 0x1E214000u | t | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_ABS: a6_w(a, 0x1E20C000u | t | (uint32_t)fn << 5 | (uint32_t)fd); break;
    case FA_SQRT: a6_w(a, 0x1E21C000u | t | (uint32_t)fn << 5 | (uint32_t)fd); break;
    default: fail(a, "fp op kind");
    }
}

void a6_fcmp(A6 *a, int sz, int fn, int fm) {
    chk(a, fn, "fcmp"); chk(a, fm, "fcmp");
    a6_w(a, 0x1E202000u | ftype(a, sz) | (uint32_t)fm << 16 | (uint32_t)fn << 5);
}

/* kind: FV_SCVTF/FV_UCVTF - xN -> fD (sz - ширина результата); FV_FCVTZS/FV_FCVTZU - fN (sz - ширина источника) -> xD;
   FV_F2D - float -> double, FV_D2F - double -> float (sz не используется) */
void a6_fcvt(A6 *a, int kind, int sz, int rd, int rn) {
    chk(a, rd, "fcvt"); chk(a, rn, "fcvt");
    uint32_t rr = (uint32_t)rn << 5 | (uint32_t)rd;
    switch (kind) {
    case FV_SCVTF: a6_w(a, 0x9E220000u | ftype(a, sz) | rr); break;
    case FV_UCVTF: a6_w(a, 0x9E230000u | ftype(a, sz) | rr); break;
    case FV_FCVTZS: a6_w(a, 0x9E380000u | ftype(a, sz) | rr); break;
    case FV_FCVTZU: a6_w(a, 0x9E390000u | ftype(a, sz) | rr); break;
    case FV_F2D: a6_w(a, 0x1E22C000u | rr); break;
    case FV_D2F: a6_w(a, 0x1E624000u | rr); break;
    default: fail(a, "fcvt kind");
    }
}

void a6_strq(A6 *a, int fn, int rn, int off) {                       /* str qN, [xM, #off]; off кратно 16, 0..65520 */
    chk(a, fn, "str q"); chk(a, rn, "str q");
    if (off < 0 || off > 65520 || (off & 15)) fail(a, "str q offset");
    a6_w(a, 0x3D800000u | (uint32_t)(off >> 4) << 10 | (uint32_t)rn << 5 | (uint32_t)fn);
}
