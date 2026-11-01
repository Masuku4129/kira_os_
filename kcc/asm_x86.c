/* kcc: собственный кодировщик x86-64 (подмножество, нужное кодогенератору и встроенному asm).
 * Каждая форма операндов, которой нет в таблице шаблонов, - ошибка компиляции, а не молчаливо неверный код. */
#include "x86.h"
#include <string.h>

Opd xr(int sz, int r) { Opd o = { OP_REG, sz, r, 0, NULL }; return o; }
Opd xm(int sz, int base, int64_t disp) { Opd o = { OP_MEM, sz, base, disp, NULL }; return o; }
Opd xs(int sz, Obj *sym, int64_t add) { Opd o = { OP_MEM, sz, 0, add, sym }; return o; }
Opd xi(int64_t v) { Opd o = { OP_IMM, 0, 0, v, NULL }; return o; }

static void fail(XA *a, const char *what, int op, const Opd *d, const Opd *s) {
    kerr(a->file, a->line, "x86-64 backend has no encoding template for %s (op %d, operands %d/%d size %d/%d)", what, op,
         d ? d->k : 0, s ? s->k : 0, d ? d->sz : 0, s ? s->sz : 0);
}

static void put(XA *a, int b) {
    if (a->len == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 65536;
        unsigned char *np = ka_new(nc);
        if (a->len) memcpy(np, a->p, a->len);
        a->p = np; a->cap = nc;
    }
    a->p[a->len++] = (unsigned char)b;
}
static void put32(XA *a, uint32_t v) { for (int i = 0; i < 4; i++) put(a, (int)(v >> (8 * i) & 255)); }
static void put64(XA *a, uint64_t v) { put32(a, (uint32_t)v); put32(a, (uint32_t)(v >> 32)); }
static int fits8(int64_t v) { return v >= -128 && v <= 127; }
static int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static XFix *add_fix(XA *a, Obj *t, int64_t add) {
    XFix *f = ka_new(sizeof *f);
    f->off = (int)a->len; f->target = t; f->add = add; f->line = a->line;
    if (a->fixtail) a->fixtail->next = f; else a->fix = f;
    a->fixtail = f;
    return f;
}

/* [0x66] [REX] опкод ModRM [SIB] [disp]. reg - номер регистра или /цифра, rm - REG или MEM. Возвращает fixup для rip-операнда. */
static XFix *enc(XA *a, int w, int p66, const unsigned char *opc, int nopc, int reg, const Opd *rm, int force_rex) {
    int rex = 0x40 | (w ? 8 : 0) | ((reg >> 3 & 1) << 2);
    int base = rm->r;
    if (rm->k == OP_REG || (rm->k == OP_MEM && !rm->sym)) rex |= base >> 3 & 1;
    if (p66) put(a, 0x66);
    if (rex != 0x40 || force_rex) put(a, rex);
    for (int i = 0; i < nopc; i++) put(a, opc[i]);
    if (rm->k == OP_REG) { put(a, 0xC0 | (reg & 7) << 3 | (base & 7)); return NULL; }
    if (rm->sym) {
        put(a, (reg & 7) << 3 | 5);
        XFix *f = add_fix(a, rm->sym, rm->v);
        put32(a, 0);
        return f;
    }
    if (!fits32(rm->v)) kerr(a->file, a->line, "x86-64 backend: displacement out of range");
    int need = rm->v != 0 || (base & 7) == 5;
    int mod = !need ? 0 : fits8(rm->v) ? 1 : 2;
    put(a, mod << 6 | (reg & 7) << 3 | ((base & 7) == 4 ? 4 : (base & 7)));
    if ((base & 7) == 4) put(a, 0x24);
    if (mod == 1) put(a, (int)(rm->v & 255));
    else if (mod == 2) put32(a, (uint32_t)rm->v);
    return NULL;
}

static int szok(int sz) { return sz == 1 || sz == 2 || sz == 4 || sz == 8; }
static int is_rm(const Opd *o) { return o->k == OP_REG || o->k == OP_MEM; }
static int byte_rex(const Opd *o) { return o->k == OP_REG && o->sz == 1 && o->r >= 4 && o->r <= 7; }

static void imm_tail(XA *a, XFix *fx, int64_t v, int nbytes) {
    for (int i = 0; i < nbytes; i++) put(a, (int)((uint64_t)v >> (8 * i) & 255));
    if (fx) fx->add -= nbytes;
}

void xa_ins(XA *a, int op, Opd d, Opd s) {
    unsigned char o[3];
    switch (op) {
    case X_ADD: case X_OR: case X_AND: case X_SUB: case X_XOR: case X_CMP: {
        int sz = d.sz ? d.sz : s.sz;
        if (!szok(sz) || !is_rm(&d)) fail(a, "ALU", op, &d, &s);
        int base = op * 8, w = sz == 8, p66 = sz == 2;
        if (s.k == OP_IMM) {
            if (sz == 1) { o[0] = 0x80; XFix *f = enc(a, 0, 0, o, 1, op, &d, byte_rex(&d)); imm_tail(a, f, s.v, 1); return; }
            if (sz == 2) fail(a, "16-bit ALU with immediate", op, &d, &s);
            if (fits8(s.v)) { o[0] = 0x83; XFix *f = enc(a, w, 0, o, 1, op, &d, 0); imm_tail(a, f, s.v, 1); return; }
            if (!fits32(s.v)) fail(a, "ALU with 64-bit immediate", op, &d, &s);
            o[0] = 0x81; XFix *f = enc(a, w, 0, o, 1, op, &d, 0); imm_tail(a, f, s.v, 4); return;
        }
        if (s.k == OP_REG && s.sz == sz) {
            o[0] = (unsigned char)(base + (sz == 1 ? 0 : 1));
            enc(a, w, p66, o, 1, s.r, &d, byte_rex(&s) || byte_rex(&d)); return;
        }
        if (d.k == OP_REG && s.k == OP_MEM && s.sz == sz) {
            o[0] = (unsigned char)(base + (sz == 1 ? 2 : 3));
            enc(a, w, p66, o, 1, d.r, &s, byte_rex(&d)); return;
        }
        fail(a, "ALU", op, &d, &s); return;
    }
    case X_TEST:
        if (d.k == OP_REG && s.k == OP_REG && d.sz == s.sz && (d.sz == 4 || d.sz == 8 || d.sz == 1)) {
            o[0] = d.sz == 1 ? 0x84 : 0x85;
            enc(a, d.sz == 8, 0, o, 1, s.r, &d, byte_rex(&d) || byte_rex(&s)); return;
        }
        fail(a, "TEST", op, &d, &s); return;
    case X_MOV: {
        if (d.k == OP_REG && s.k == OP_IMM) {
            int r = d.r;
            if (d.sz == 8 && fits32(s.v)) { o[0] = 0xC7; enc(a, 1, 0, o, 1, 0, &d, 0); imm_tail(a, NULL, s.v, 4); return; }
            if (d.sz == 8) { put(a, 0x48 | (r >> 3)); put(a, 0xB8 + (r & 7)); put64(a, (uint64_t)s.v); return; }
            if (d.sz == 4) { if (r >> 3) put(a, 0x41); put(a, 0xB8 + (r & 7)); put32(a, (uint32_t)s.v); return; }
            if (d.sz == 1) { if ((r >> 3) || (r >= 4 && r <= 7)) put(a, 0x40 | (r >> 3)); put(a, 0xB0 + (r & 7)); put(a, (int)(s.v & 255)); return; }
            fail(a, "MOV reg16, imm", op, &d, &s); return;
        }
        if (d.k == OP_MEM && s.k == OP_IMM) {
            if (d.sz == 1) { o[0] = 0xC6; XFix *f = enc(a, 0, 0, o, 1, 0, &d, 0); imm_tail(a, f, s.v, 1); return; }
            if (d.sz == 2) { o[0] = 0xC7; XFix *f = enc(a, 0, 1, o, 1, 0, &d, 0); imm_tail(a, f, s.v, 2); return; }
            if (d.sz == 4 || (d.sz == 8 && fits32(s.v))) { o[0] = 0xC7; XFix *f = enc(a, d.sz == 8, 0, o, 1, 0, &d, 0); imm_tail(a, f, s.v, 4); return; }
            fail(a, "MOV mem, imm", op, &d, &s); return;
        }
        if (!szok(d.sz) || d.sz != s.sz) fail(a, "MOV size", op, &d, &s);
        int sz = d.sz;
        if (d.k == OP_REG && s.k == OP_REG) { o[0] = sz == 1 ? 0x88 : 0x89; enc(a, sz == 8, sz == 2, o, 1, s.r, &d, byte_rex(&d) || byte_rex(&s)); return; }
        if (d.k == OP_REG && s.k == OP_MEM) { o[0] = sz == 1 ? 0x8A : 0x8B; enc(a, sz == 8, sz == 2, o, 1, d.r, &s, byte_rex(&d)); return; }
        if (d.k == OP_MEM && s.k == OP_REG) { o[0] = sz == 1 ? 0x88 : 0x89; enc(a, sz == 8, sz == 2, o, 1, s.r, &d, byte_rex(&s)); return; }
        fail(a, "MOV", op, &d, &s); return;
    }
    case X_MOVZX:
        if (d.k == OP_REG && (d.sz == 4 || d.sz == 8) && is_rm(&s) && (s.sz == 1 || s.sz == 2)) {
            o[0] = 0x0F; o[1] = s.sz == 1 ? 0xB6 : 0xB7;
            enc(a, 0, 0, o, 2, d.r, &s, byte_rex(&s)); return;
        }
        fail(a, "MOVZX", op, &d, &s); return;
    case X_MOVSX:
        if (d.k == OP_REG && (d.sz == 4 || d.sz == 8) && is_rm(&s) && (s.sz == 1 || s.sz == 2 || (s.sz == 4 && d.sz == 8))) {
            if (s.sz == 4) { o[0] = 0x63; enc(a, 1, 0, o, 1, d.r, &s, 0); return; }
            o[0] = 0x0F; o[1] = s.sz == 1 ? 0xBE : 0xBF;
            enc(a, d.sz == 8, 0, o, 2, d.r, &s, byte_rex(&s)); return;
        }
        fail(a, "MOVSX", op, &d, &s); return;
    case X_LEA:
        if (d.k == OP_REG && d.sz == 8 && s.k == OP_MEM) { o[0] = 0x8D; enc(a, 1, 0, o, 1, d.r, &s, 0); return; }
        fail(a, "LEA", op, &d, &s); return;
    case X_IMUL:
        if (d.k == OP_REG && (d.sz == 4 || d.sz == 8) && is_rm(&s) && s.sz == d.sz) { o[0] = 0x0F; o[1] = 0xAF; enc(a, d.sz == 8, 0, o, 2, d.r, &s, 0); return; }
        fail(a, "IMUL", op, &d, &s); return;
    case X_NEG: case X_NOT: case X_DIV: case X_IDIV: {
        int dig = op == X_NOT ? 2 : op == X_NEG ? 3 : op == X_DIV ? 6 : 7;
        if (is_rm(&d) && s.k == OP_NONE && (d.sz == 4 || d.sz == 8)) { o[0] = 0xF7; enc(a, d.sz == 8, 0, o, 1, dig, &d, 0); return; }
        fail(a, "unary", op, &d, &s); return;
    }
    case X_SHL: case X_SHR: case X_SAR: {
        int dig = op == X_SHL ? 4 : op == X_SHR ? 5 : 7;
        if (is_rm(&d) && (d.sz == 4 || d.sz == 8) && s.k == OP_REG && s.sz == 1 && s.r == R_CX) { o[0] = 0xD3; enc(a, d.sz == 8, 0, o, 1, dig, &d, 0); return; }
        if (is_rm(&d) && (d.sz == 4 || d.sz == 8) && s.k == OP_IMM && s.v >= 0 && s.v < (d.sz == 8 ? 64 : 32)) {
            o[0] = 0xC1; XFix *f = enc(a, d.sz == 8, 0, o, 1, dig, &d, 0); imm_tail(a, f, s.v, 1); return;
        }
        fail(a, "shift (by CL or immediate)", op, &d, &s); return;
    }
    case X_XCHG: case X_LXADD: case X_LCMPXCHG: {
        int sz = d.sz;
        if (d.k != OP_MEM || s.k != OP_REG || s.sz != sz || !szok(sz)) fail(a, "atomic", op, &d, &s);
        if (op != X_XCHG) put(a, 0xF0);                       /* lock */
        if (op == X_XCHG) { o[0] = sz == 1 ? 0x86 : 0x87; enc(a, sz == 8, sz == 2, o, 1, s.r, &d, byte_rex(&s)); return; }
        o[0] = 0x0F; o[1] = (unsigned char)((op == X_LXADD ? 0xC0 : 0xB0) + (sz == 1 ? 0 : 1));
        enc(a, sz == 8, sz == 2, o, 2, s.r, &d, byte_rex(&s));
        return;
    }
    case X_CALLR:
        if (d.k == OP_REG && d.sz == 8) { o[0] = 0xFF; enc(a, 0, 0, o, 1, 2, &d, 0); return; }
        fail(a, "CALL", op, &d, &s); return;
    default: fail(a, "unknown op", op, &d, &s);
    }
}

void xa_op0(XA *a, int op) {
    switch (op) {
    case X0_RET: put(a, 0xC3); return;
    case X0_CQO: put(a, 0x48); put(a, 0x99); return;
    case X0_SYSCALL: put(a, 0x0F); put(a, 0x05); return;
    case X0_RDTSC: put(a, 0x0F); put(a, 0x31); return;
    case X0_MFENCE: put(a, 0x0F); put(a, 0xAE); put(a, 0xF0); return;
    case X0_LFENCE: put(a, 0x0F); put(a, 0xAE); put(a, 0xE8); return;
    case X0_SFENCE: put(a, 0x0F); put(a, 0xAE); put(a, 0xF8); return;
    case X0_PAUSE: put(a, 0xF3); put(a, 0x90); return;
    case X0_HLT: put(a, 0xF4); return;
    case X0_CLI: put(a, 0xFA); return;
    case X0_STI: put(a, 0xFB); return;
    case X0_NOP: put(a, 0x90); return;
    case X0_UD2: put(a, 0x0F); put(a, 0x0B); return;
    case X0_REP_MOVSB: put(a, 0xF3); put(a, 0xA4); return;
    case X0_REP_STOSB: put(a, 0xF3); put(a, 0xAA); return;
    case X0_LEAVE: put(a, 0xC9); return;
    case X0_PUSH_RBP: put(a, 0x55); return;
    case X0_IN8: put(a, 0xEC); return;
    case X0_IN16: put(a, 0x66); put(a, 0xED); return;
    case X0_IN32: put(a, 0xED); return;
    case X0_OUT8: put(a, 0xEE); return;
    case X0_OUT16: put(a, 0x66); put(a, 0xEF); return;
    case X0_OUT32: put(a, 0xEF); return;
    default: kerr(a->file, a->line, "x86-64 backend has no encoding template for zero-operand op %d", op);
    }
}

void xa_setcc(XA *a, int cc, Opd d) {
    unsigned char o[2] = { 0x0F, (unsigned char)(0x90 + cc) };
    if (cc < 0 || cc > 15 || !is_rm(&d) || d.sz != 1) fail(a, "SETcc", cc, &d, NULL);
    enc(a, 0, 0, o, 2, 0, &d, byte_rex(&d));
}

void xa_labels_begin(XA *a, int nlab) {
    a->nlab = nlab + 1;
    a->lab = ka_new(sizeof(int) * (size_t)a->nlab);
    for (int i = 0; i < a->nlab; i++) a->lab[i] = -1;
    a->njf = 0;
}
void xa_label(XA *a, int lab) {
    if (lab < 0 || lab >= a->nlab) kerr(a->file, a->line, "internal: label %d out of range", lab);
    a->lab[lab] = (int)a->len;
}
void xa_jmp(XA *a, int cc, int lab) {
    if (cc == CC_NONE) put(a, 0xE9); else { put(a, 0x0F); put(a, 0x80 + cc); }
    if (a->njf == a->cjf) {
        int nc = a->cjf ? a->cjf * 2 : 256;
        XJmp *n = ka_new(sizeof(XJmp) * (size_t)nc);
        if (a->njf) memcpy(n, a->jf, sizeof(XJmp) * (size_t)a->njf);
        a->jf = n; a->cjf = nc;
    }
    a->jf[a->njf].off = (int)a->len; a->jf[a->njf].lab = lab; a->njf++;
    put32(a, 0);
}
void xa_labels_end(XA *a) {
    for (int i = 0; i < a->njf; i++) {
        int l = a->jf[i].lab, off = a->jf[i].off;
        if (l < 0 || l >= a->nlab || a->lab[l] < 0) kerr(a->file, a->line, "internal: jump to undefined label %d", l);
        uint32_t rel = (uint32_t)(a->lab[l] - (off + 4));
        for (int k = 0; k < 4; k++) a->p[off + k] = (unsigned char)(rel >> (8 * k) & 255);
    }
    a->njf = 0;
}
void xa_call(XA *a, Obj *t) {
    put(a, 0xE8);
    add_fix(a, t, 0);
    put32(a, 0);
}
