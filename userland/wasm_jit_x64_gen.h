/* wasm_jit_x64_gen.h - вторая половина JIT x86-64: команды, функция, модуль (см. wasm_jit_x64.h). */

/* ---------- команды ---------- */
static const u8 cc_of_cmp[10] = { 4, 5, 12, 2, 15, 7, 14, 6, 13, 3 };    /* eq ne lt_s lt_u gt_s gt_u le_s le_u ge_s ge_u -> условие x86 */
static int fits32(u64 c) { return (i64)c >= -2147483648ll && (i64)c <= 2147483647ll; }
#define JNEED(n) do { if (jh_ < jctl[jnctl - 1].h + (u32)(n)) { jfail = 1; return -1; } } while (0)

static void jalu(int op, int w, int d, u32 ib) {                  /* d = d op (операнд ib: константа, память или регистр) */
    jv_t* e = &jv[ib]; i32 disp;
    if (e->k == VK_CONST && (w == 0 || fits32(e->c))) alu_ri(op, w, d, (i32)e->c);
    else if (jmemop(ib, &disp)) alu_rm(op, w, d, RBX, disp);
    else { int r = jgpr(ib, 1u << d); alu_rr(op, w, d, r); }
}
static void jint_arith(u32 k, int w) {                            /* add sub mul and or xor */
    static const u8 aluop[10] = { A_ADD, A_SUB, 0, 0, 0, 0, 0, A_AND, A_OR, A_XOR };
    u32 ia = jh_ - 2, ib = jh_ - 1;
    if (k != 1 && jv[ia].k != VK_GPR && jv[ib].k == VK_GPR) { u32 t = ia; ia = ib; ib = t; }
    int d = jgpr(ia, 0); i32 disp;
    if (k == 2) {
        jv_t* e = &jv[ib];
        if (e->k == VK_CONST && fits32(e->c)) { jrr(0, w, 0x69, d, d, 0); j32((u32)(i32)e->c); }
        else if (jmemop(ib, &disp)) jrm(0, w, 0x0FAF, d, RBX, -1, 0, disp, 0);
        else { int r = jgpr(ib, 1u << d); jrr(0, w, 0x0FAF, d, r, 0); }
    } else jalu(aluop[k], w, d, ib);
    jrel(ib); jh_ -= 2; jset_gpr(jh_, d); jh_++;
}
static void jcmp_int(u32 k, int w) {
    u32 ia = jh_ - 2, ib = jh_ - 1; jv_t* b = &jv[ib]; i32 disp;
    int b_imm = b->k == VK_CONST && (w == 0 || fits32(b->c));
    if (jmemop(ia, &disp) && (b_imm || b->k == VK_GPR)) {
        if (b_imm) {
            i32 imm = (i32)b->c;
            if (imm >= -128 && imm <= 127) { jrm(0, w, 0x83, 7, RBX, -1, 0, disp, 0); j8((u32)imm); } else { jrm(0, w, 0x81, 7, RBX, -1, 0, disp, 0); j32((u32)imm); }
        } else jrm(0, w, 0x39, b->r, RBX, -1, 0, disp, 0);
    } else { int d = jgpr(ia, 0); jalu(A_CMP, w, d, ib); }
    jpop(2); jpush(VK_CC); jv[jh_ - 1].cc = cc_of_cmp[k];
}
static void jeqz(int w) {
    u32 t = jh_ - 1; jv_t* e = &jv[t]; i32 disp;
    if (e->k == VK_CC) { e->cc ^= 1; return; }
    if (jmemop(t, &disp)) { jrm(0, w, 0x83, 7, RBX, -1, 0, disp, 0); j8(0); }
    else { int r = jgpr(t, 0); jrr(0, w, 0x85, r, r, 0); }
    jrel(t); jv[t].k = VK_CC; jv[t].cc = 4;
}
static void jdiv(u32 k, int w) {                                  /* 3 div_s 4 div_u 5 rem_s 6 rem_u */
    u32 ia = jh_ - 2, ib = jh_ - 1; int sgn = (k == 3 || k == 5), rem = (k == 5 || k == 6);
    jv_t* b = &jv[ib]; i32 disp = 0; int dreg = -1, mem = 0;
    int cnst = b->k == VK_CONST; u64 bc = b->c;
    int nz = cnst && (w ? bc != 0 : (u32)bc != 0), m1 = cnst && (w ? bc == ~0ull : (u32)bc == 0xFFFFFFFFu);
    if (cnst) { mov_ri(RCX, w ? bc : (u32)bc); dreg = RCX; }
    else if (jmemop(ib, &disp)) mem = 1;
    else dreg = jgpr(ib, 0);
    load_to(RAX, ia);
    if (!cnst) {
        if (mem) { jrm(0, w, 0x83, 7, RBX, -1, 0, disp, 0); j8(0); } else jrr(0, w, 0x85, dreg, dreg, 0);
        jjcc_trap(4, TK_DIV0);
    } else if (!nz) jjmp_trap(TK_DIV0);
    u32 dn = 0;
    if (sgn && (!cnst || m1)) {
        if (mem) { jrm(0, w, 0x83, 7, RBX, -1, 0, disp, 0); j8(0xFF); } else alu_ri(A_CMP, w, dreg, -1);
        u32 sk = jshort(5);
        if (!rem) {
            if (w) { mov_ri(RDX, 0x8000000000000000ull); alu_rr(A_CMP, 1, RAX, RDX); } else alu_ri(A_CMP, 0, RAX, (i32)0x80000000u);
            jjcc_trap(4, TK_OVF);
            jrr(0, w, 0xF7, 3, RAX, 0);                           /* neg */
        } else mov_ri(RDX, 0);
        dn = jshort(-1);
        jshort_end(sk);
    }
    if (sgn) { if (w) j8(0x48); j8(0x99); } else mov_ri(RDX, 0);   /* cdq / cqo ; edx = 0 */
    if (mem) jrm(0, w, 0xF7, sgn ? 7 : 6, RBX, -1, 0, disp, 0); else jrr(0, w, 0xF7, sgn ? 7 : 6, dreg, 0);
    if (dn) jshort_end(dn);
    jpop(2);
    int r = jalloc_gpr(jh_, 0);
    jrr(0, w, 0x8B, r, rem ? RDX : RAX, 0);
    jpush(VK_GPR); jset_gpr(jh_ - 1, r);
}
static void jshift(u32 k, int w) {                                /* 10 shl 11 shr_s 12 shr_u 13 rotl 14 rotr */
    static const u8 ext[5] = { 4, 7, 5, 0, 1 };
    u32 ia = jh_ - 2, ib = jh_ - 1;
    int d = jgpr(ia, 0); jv_t* b = &jv[ib];
    if (b->k == VK_CONST) { u32 n = (u32)b->c & (w ? 63u : 31u); if (n) { jrr(0, w, 0xC1, ext[k - 10], d, 0); j8(n); } }
    else { load_to(RCX, ib); jrr(0, w, 0xD3, ext[k - 10], d, 0); }
    jrel(ib); jh_--;
}
static void jfarith(int dbl, u32 k) {                             /* 0 add 1 sub 2 mul 3 div */
    static const u8 opc[4] = { 0x58, 0x5C, 0x59, 0x5E };
    u32 ia = jh_ - 2, ib = jh_ - 1;
    if ((k == 0 || k == 2) && jv[ia].k != VK_XMM && jv[ib].k == VK_XMM) { u32 t = ia; ia = ib; ib = t; }
    int x = jxmm(ia, dbl, 0); i32 disp; u32 pfx = dbl ? 0xF2 : 0xF3;
    int done = 0;
    if (jmemop(ib, &disp)) { sse_rm(pfx, 0x0F00u | opc[k], x, RBX, -1, disp); done = 1; }
    else if (jv[ib].k == VK_CONST) done = sse_rip(pfx, 0x0F00u | opc[k], x, dbl ? jv[ib].c : (u32)jv[ib].c);
    if (!done) { int y = jxmm(ib, dbl, 1u << x); sse_rr(pfx, 0x0F00u | opc[k], x, y); }
    jrel(ib); jh_ -= 2; jset_xmm(jh_, x, dbl); jh_++;
}
static void jfcmp(int dbl, u32 k) {                               /* 0 eq 1 ne 2 lt 3 gt 4 le 5 ge */
    u32 ia = jh_ - 2, ib = jh_ - 1; i32 disp;
    u32 fst = (k == 2 || k == 4) ? ib : ia, snd = (k == 2 || k == 4) ? ia : ib;
    int cc = (k == 2 || k == 3) ? 7 : 3;                          /* a / ae: при NaN обе ложны */
    int x = jxmm(fst, dbl, 0); u32 pfx = dbl ? 0x66 : 0;
    int done = 0;
    if (jmemop(snd, &disp)) { sse_rm(pfx, 0x0F2E, x, RBX, -1, disp); done = 1; }
    else if (jv[snd].k == VK_CONST) done = sse_rip(pfx, 0x0F2E, x, dbl ? jv[snd].c : (u32)jv[snd].c);
    if (!done) { int y = jxmm(snd, dbl, 1u << x); sse_rr(pfx, 0x0F2E, x, y); }
    if (k < 2) {
        if (k == 0) { jrr(0, 0, 0x0F9B, 0, RAX, 0); jrr(0, 0, 0x0F94, 0, RCX, 0); jrr(0, 0, 0x20, RCX, RAX, 0); }      /* setnp al; sete cl; and al, cl */
        else { jrr(0, 0, 0x0F9A, 0, RAX, 0); jrr(0, 0, 0x0F95, 0, RCX, 0); jrr(0, 0, 0x08, RCX, RAX, 0); }            /* setp al; setne cl; or al, cl */
        jpop(2); int r = jalloc_gpr(jh_, 0); jrr(0, 0, 0x0FB6, r, RAX, 0); jpush(VK_GPR); jset_gpr(jh_ - 1, r);
    } else { jpop(2); jpush(VK_CC); jv[jh_ - 1].cc = (u8)cc; }
}

/* Адрес доступа: константа в уже выданной памяти - без проверки; иначе индекс в rcx, проверка ea + размер <= длины (r14). */
static int jmem_addr(u32 ia, u32 off, u32 size, u32 avoid, i32* disp) {       /* 0 - [r15 + disp], 1 - [r15 + rcx + disp] */
    jv_t* e = &jv[ia];
    u64 lim = (u64)off + size;
    if (e->k == VK_CONST) {
        u64 ea = (u64)(u32)e->c + off;
        if (ea + size <= (u64)mem_pages * 65536) { *disp = (i32)ea; return 0; }
        jjmp_trap(TK_OOB); *disp = 0; return 0;
    }
    if (lim > 0x7FFFFFFFull) { jjmp_trap(TK_OOB); *disp = 0; return 0; }
    int ra = jgpr(ia, avoid);
    jrr(0, 0, 0x8B, RCX, ra, 0);                                  /* mov ecx, ra32: нулевое расширение */
    jrm(0, 1, 0x8D, RDX, RCX, -1, 0, (i32)lim, 0);                /* lea rdx, [rcx + off + size] */
    alu_rr(A_CMP, 1, RDX, R14); jjcc_trap(7, TK_OOB);             /* ja */
    *disp = (i32)off; return 1;
}
static int jmemacc(u32 op, u32 off) {
    static const u8 lsz[14] = { 4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4 };
    static const u8 lw[14] = { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0 };
    static const u16 lop[14] = { 0x8B, 0x8B, 0, 0, 0x0FBE, 0x0FB6, 0x0FBF, 0x0FB7, 0x0FBE, 0x0FB6, 0x0FBF, 0x0FB7, 0x63, 0x8B };
    if (op <= 0x35) {
        JNEED(1);
        u32 k = op - 0x28, sz = lsz[k], ia = jh_ - 1; i32 disp;
        int mode = jmem_addr(ia, off, sz, 0, &disp);
        jpop(1);
        if (k == 2 || k == 3) {
            int x = jalloc_xmm(jh_, 0);
            sse_rm(k == 3 ? 0xF2 : 0xF3, 0x0F10, x, R15, mode ? RCX : -1, disp);
            jpush(VK_XMM); jset_xmm(jh_ - 1, x, k == 3);
        } else {
            int r = jalloc_gpr(jh_, 0);
            jrm(0, lw[k], lop[k], r, R15, mode ? RCX : -1, 0, disp, 0);
            jpush(VK_GPR); jset_gpr(jh_ - 1, r);
        }
        return 0;
    }
    static const u8 ssz[9] = { 4, 8, 4, 8, 1, 2, 1, 2, 4 };
    JNEED(2);
    u32 sz = ssz[op - 0x36], iv = jh_ - 1, ia = jh_ - 2; i32 disp; jv_t* v = &jv[iv];
    int isf = (op == 0x38 || op == 0x39) && v->k == VK_XMM;
    int imm = v->k == VK_CONST && (sz < 8 || fits32(v->c));
    int rv = -1;
    if (!isf && !imm) rv = jgpr(iv, 0);
    int mode = jmem_addr(ia, off, sz, rv >= 0 ? 1u << rv : 0, &disp);
    int idx = mode ? RCX : -1;
    if (isf) sse_rm(sz == 8 ? 0xF2 : 0xF3, 0x0F11, v->r, R15, idx, disp);
    else if (imm) {
        if (sz == 1) { jrm(0, 0, 0xC6, 0, R15, idx, 0, disp, 0); j8((u32)v->c); }
        else if (sz == 2) { jrm(0x66, 0, 0xC7, 0, R15, idx, 0, disp, 0); j8((u32)v->c); j8((u32)v->c >> 8); }
        else { jrm(0, sz == 8, 0xC7, 0, R15, idx, 0, disp, 0); j32((u32)v->c); }
    } else {
        if (sz == 1) jrm(0, 0, 0x88, rv, R15, idx, 0, disp, 1);
        else if (sz == 2) jrm(0x66, 0, 0x89, rv, R15, idx, 0, disp, 0);
        else jrm(0, sz == 8, 0x89, rv, R15, idx, 0, disp, 0);
    }
    jpop(2);
    return 0;
}

/* Вызов помощника на C: все регистры значений сброшены в ячейки, аргументы - в rdi/rsi/rdx/rcx. */
static void jcall_h(i32 off) { jrm(0, 0, 0xFF, 2, R13, -1, 0, off, 0); }
static int jhelper(u32 op, u32 nargs) {                           /* jh_un (1 аргумент) или jh_bin (2); результат кладётся */
    JNEED(nargs);
    jspill_regs();
    mov_ri(RDI, op);
    load_to(RSI, jh_ - nargs);
    if (nargs == 2) load_to(RDX, jh_ - 1);
    jcall_h(nargs == 2 ? CXO(h_bin) : CXO(h_un));
    jrr(0, 0, 0x85, RAX, RAX, 0); jjnz_stop();
    jpop(nargs);
    int r = jalloc_gpr(jh_, 0); mov_rm(1, r, R13, CXO(res));
    jpush(VK_GPR); jset_gpr(jh_ - 1, r);
    return 0;
}
static int jconv(u32 op) {
    JNEED(1);
    u32 t = jh_ - 1; jv_t* e = &jv[t]; int r, x;
    switch (op) {
    case 0xA7: if (e->k == VK_CONST) e->c = (u32)e->c; return 0;
    case 0xBE: case 0xBF: return 0;                               /* биты те же: нужный регистр выберет потребитель */
    case 0xBC: case 0xBD: jgpr(t, 0); return 0;
    case 0xAC: r = jgpr(t, 0); jrr(0, 1, 0x63, r, r, 0); return 0;
    case 0xAD: r = jgpr(t, 0); jrr(0, 0, 0x8B, r, r, 0); return 0;
    case 0xC0: r = jgpr(t, 0); jrr(0, 0, 0x0FBE, r, r, 1); return 0;
    case 0xC1: r = jgpr(t, 0); jrr(0, 0, 0x0FBF, r, r, 0); return 0;
    case 0xC2: r = jgpr(t, 0); jrr(0, 1, 0x0FBE, r, r, 1); return 0;
    case 0xC3: r = jgpr(t, 0); jrr(0, 1, 0x0FBF, r, r, 0); return 0;
    case 0xC4: r = jgpr(t, 0); jrr(0, 1, 0x63, r, r, 0); return 0;
    case 0xB2: case 0xB3: case 0xB4: case 0xB7: case 0xB8: case 0xB9: {
        int dbl = op >= 0xB7, uns = (op == 0xB3 || op == 0xB8), w64 = (op == 0xB4 || op == 0xB9 || uns);
        r = jgpr(t, 0);
        if (uns) jrr(0, 0, 0x8B, r, r, 0);                        /* u32: нулевое расширение, затем 64-битное преобразование */
        x = jalloc_xmm(t, 0);
        jrr(dbl ? 0xF2 : 0xF3, w64, 0x0F2A, x, r, 0);             /* cvtsi2ss/sd */
        jrel(t); jset_xmm(t, x, dbl); return 0;
    }
    case 0xB6: x = jxmm(t, 1, 0); sse_rr(0xF2, 0x0F5A, x, x); jv[t].w = 4; return 0;
    case 0xBB: x = jxmm(t, 0, 0); sse_rr(0xF3, 0x0F5A, x, x); jv[t].w = 8; return 0;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: {
        int dbl = op == 0xAA || op == 0xAB, uns = op == 0xA9 || op == 0xAB;
        x = jxmm(t, dbl, 0);
        jrr(dbl ? 0xF2 : 0xF3, 1, 0x0F2C, RAX, x, 0);             /* cvttss2si rax, x (64 бита: диапазон проверяется ниже) */
        if (uns) jrr(0, 0, 0x8B, RCX, RAX, 0); else jrr(0, 1, 0x63, RCX, RAX, 0);
        alu_rr(A_CMP, 1, RCX, RAX); jjcc_trap(5, TK_CONV);
        jpop(1); r = jalloc_gpr(jh_, 0); jrr(0, 0, 0x8B, r, RAX, 0);
        jpush(VK_GPR); jset_gpr(jh_ - 1, r); return 0;
    }
    default: return jhelper(op, 1);
    }
}
static int jcall(u32 fi, int indirect, u32 ti) {
    const ftype_t* ft = &types[indirect ? ti : funcs[fi].type];
    u32 np = ft->np, nr = ft->nr;
    JNEED(np + (indirect ? 1u : 0u));
    if (indirect) { int xr = jgpr(jh_ - 1, 0); jrr(0, 0, 0x8B, RDX, xr, 0); jpop(1); }   /* индекс таблицы - в edx (rax занят при сбросе значений) */
    u32 base = jh_ - np;
    for (u32 i = 0; i < jh_; i++) {
        if (i >= base) { if (jv[i].k != VK_MEM) { store_entry(i, RBX, SLOT(i)); jrel(i); } }
        else jspill(i);
    }
    i32 adj = (i32)(8u * (jnloc + base));
    if (indirect) {
        alu_rm(A_CMP, 0, RDX, R13, CXO(ntable)); jjcc_trap(3, TK_NOENT);                       /* jae */
        mov_rm(1, RCX, R13, CXO(table)); jrm(0, 0, 0x8B, RCX, RCX, RDX, 2, 0, 0);               /* ecx = table[edx] */
        alu_ri(A_CMP, 0, RCX, -1); jjcc_trap(4, TK_NOENT);
        alu_ri(A_CMP, 0, RCX, (i32)nfuncs); jjcc_trap(3, TK_BADFN);
        mov_rm(1, RAX, R13, CXO(fsig)); jrm(0, 0, 0x0FB7, RAX, RAX, RCX, 1, 0, 0);               /* movzx eax, word [fsig + rcx*2] */
        alu_ri(A_CMP, 0, RAX, (i32)(np << 8 | nr)); jjcc_trap(5, TK_SIG);
        mov_rm(1, RAX, R13, CXO(jfn));
        if (adj) jrm(0, 1, 0x8D, RBX, RBX, -1, 0, adj, 0);
        jrm(0, 0, 0xFF, 2, RAX, RCX, 3, 0, 0);                                                  /* call [rax + rcx*8] */
    } else {
        if (adj) jrm(0, 1, 0x8D, RBX, RBX, -1, 0, adj, 0);
        j8(0xE8); jchain(&cchain[fi]);
    }
    if (adj) jrm(0, 1, 0x8D, RBX, RBX, -1, 0, -adj, 0);
    jh_ = base;
    if (nr) { jpush(VK_MEM); }
    return 0;
}

/* ---------- функция ---------- */
static int jcompile_func(u32 fi) {
    func_t* f = &funcs[fi];
    const ftype_t* ft = &types[f->type];
    const u8* p = f->code; const u8* end = f->end;
    jnloc = ft->np + f->nlocals; jnr = ft->nr; jh_ = jmaxh = 0; junr = 0;
    for (int i = 0; i < 16; i++) gown[i] = xown[i] = 0;
    jfnoff[fi] = jn;
    u32 fuel_ops = 0;
    alu_ri(A_SUB, 1, RSP, 8);
    alu_rm(A_CMP, 1, RSP, R13, CXO(slim)); jjcc_trap(6, TK_CSTACK);             /* jbe */
    jrm(0, 1, 0x8D, RAX, RBX, -1, 0, 0x1000, 0); u32 vpatch = jn - 4;           /* lea rax, [rbx + размер кадра] */
    alu_rm(A_CMP, 1, RAX, R13, CXO(vlim)); jjcc_trap(7, TK_VSTACK);             /* ja */
    u32 nz = f->nlocals;
    if (nz <= 8) for (u32 i = 0; i < nz; i++) { jrm(0, 1, 0xC7, 0, RBX, -1, 0, (i32)(8 * (ft->np + i)), 0); j32(0); }
    else {
        jrm(0, 1, 0x8D, RDI, RBX, -1, 0, (i32)(8 * ft->np), 0); mov_ri(RCX, nz); mov_ri(RAX, 0);
        j8(0xF3); j8(0x48); j8(0xAB);                                           /* rep stosq */
    }
    jtick();
    jctl[0].kind = 3; jctl[0].ar = (u8)jnr; jctl[0].dead = 0; jctl[0].h = 0; jctl[0].start = 0; jctl[0].pend = 0; jctl[0].ifz = 0; jnctl = 1;
    while (p < end && !jfail && !trapped) {
        if ((++fuel_ops & 255u) == 0)
            jtick();
        u8 op = *p++;
        if (junr && op != 0x02 && op != 0x03 && op != 0x04 && op != 0x05 && op != 0x0B) { skip_imm(op, &p, end); continue; }
        if (!(op == 0x0D || op == 0x04 || op == 0x45 || op == 0x1B || op == 0x1C)) jmaterialize_cc();
        switch (op) {
        case 0x00: jjmp_trap(TK_UNREACH); junr = 1; break;
        case 0x01: break;
        case 0x02: case 0x03: case 0x04: {
            u8 bt = *p; u32 ar = 0;
            if (bt == 0x40) p++;
            else if (bt == 0x7F || bt == 0x7E || bt == 0x7D || bt == 0x7C) { p++; ar = 1; }
            else { i64 ti = leb_s(&p, end, 64); if (ti < 0 || (u64)ti >= ntypes || types[ti].np) { jfail = 1; return -1; } ar = types[ti].nr; }
            if (jnctl >= 256) { jfail = 1; return -1; }
            jctl_t* n = &jctl[jnctl];
            n->kind = op == 0x03 ? 1 : op == 0x04 ? 2 : 0; n->ar = (u8)ar; n->pend = 0; n->ifz = 0; n->start = 0;
            if (junr) { n->dead = 1; n->h = jh_; jnctl++; break; }
            n->dead = 0;
            if (op == 0x04) {
                JNEED(1);
                int cc = jcond_flags();
                jcanon(0);
                jjcc_chain(cc ^ 1, &n->ifz);
            } else jcanon(0);
            n->h = jh_;
            if (op == 0x03) { n->start = jn; jtick(); }
            jnctl++;
            break;
        }
        case 0x05: {
            jctl_t* fr = &jctl[jnctl - 1];
            if (fr->kind != 2) { jfail = 1; return -1; }
            if (!junr) {
                if (jh_ != fr->h + fr->ar) { jfail = 1; return -1; }
                jmove_result(fr); jjmp_chain(&fr->pend);
            }
            if (fr->ifz) { jpatch(fr->ifz, jn); fr->ifz = 0; }
            jpop(jh_ - fr->h); junr = fr->dead; fr->kind = 0;
            break;
        }
        case 0x0B: {
            jctl_t* fr = &jctl[jnctl - 1];
            if (jnctl == 1) {
                if (!junr) { if (jh_ != jnr) { jfail = 1; return -1; } jret_seq(); }
                wr32(vpatch, 8u * (jnloc + jmaxh + 2));
                return jfail ? -1 : 0;
            }
            if (!junr) {
                if (jh_ != fr->h + fr->ar) { jfail = 1; return -1; }
                jmove_result(fr);
            }
            jpatch(fr->pend, jn); fr->pend = 0;
            if (fr->ifz) { jpatch(fr->ifz, jn); fr->ifz = 0; }
            jnctl--;
            jpop(jh_ - fr->h); junr = fr->dead;
            for (u32 i = 0; i < fr->ar; i++) jpush(VK_MEM);
            break;
        }
        case 0x0C: {
            u32 d = leb_u(&p, end); if (d >= (u32)jnctl) { jfail = 1; return -1; }
            jctl_t* tf = &jctl[jnctl - 1 - (int)d];
            if (tf->kind == 3) jret_seq(); else { jmove_result(tf); jbr_frame(tf, -1); }
            junr = 1; break;
        }
        case 0x0D: {
            u32 d = leb_u(&p, end); if (d >= (u32)jnctl) { jfail = 1; return -1; }
            JNEED(1);
            jctl_t* tf = &jctl[jnctl - 1 - (int)d];
            int cc = jcond_flags();
            u32 keep = tf->kind == 1 ? 0 : tf->ar;
            if (tf->kind == 3) { u32 sk = jshort(cc ^ 1); jret_seq(); jshort_end(sk); }
            else if (keep && !(jh_ - 1 == tf->h && jv[jh_ - 1].k == VK_MEM)) { u32 sk = jshort(cc ^ 1); jmove_result(tf); jbr_frame(tf, -1); jshort_end(sk); }
            else jbr_frame(tf, cc);
            break;
        }
        case 0x0E: {
            u32 cnt = leb_u(&p, end);
            if (cnt > (u32)(end - p)) { jfail = 1; return -1; }
            JNEED(1);
            load_to(RAX, jh_ - 1); jpop(1);
            alu_ri(A_CMP, 0, RAX, (i32)cnt);
            u32 in = jshort(2); mov_ri(RAX, cnt); jshort_end(in);
            j8(0x48); j8(0x8D); j8(0x0D); u32 lp = jn; j32(0);        /* lea rcx, [rip + T] */
            jrm(0, 1, 0x63, RAX, RCX, RAX, 2, 0, 0);                  /* movsxd rax, dword [rcx + rax*4] */
            alu_rr(A_ADD, 1, RAX, RCX);
            j8(0xFF); j8(0xE0);                                       /* jmp rax */
            u32 T = jn; wr32(lp, T - (lp + 4));
            for (u32 i = 0; i <= cnt; i++) j32(0);
            for (u32 i = 0; i <= cnt && !jfail; i++) {
                u32 d = leb_u(&p, end); if (d >= (u32)jnctl) { jfail = 1; return -1; }
                jctl_t* tf = &jctl[jnctl - 1 - (int)d];
                wr32(T + 4 * i, jn - T);
                if (tf->kind == 3) jret_seq(); else { jmove_result(tf); jbr_frame(tf, -1); }
            }
            junr = 1; break;
        }
        case 0x0F: jret_seq(); junr = 1; break;
        case 0x10: {
            u32 fi2 = leb_u(&p, end); if (fi2 >= nfuncs) { jfail = 1; return -1; }
            if (jcall(fi2, 0, 0)) return -1;
            break;
        }
        case 0x11: {
            u32 ti = leb_u(&p, end); p++;
            if (ti >= ntypes) { jfail = 1; return -1; }
            if (jcall(0, 1, ti)) return -1;
            break;
        }
        case 0x1A: JNEED(1); jpop(1); break;
        case 0x1B: case 0x1C: {
            if (op == 0x1C) { u32 k = leb_u(&p, end); p += k; }
            JNEED(3);
            u32 ic = jh_ - 1, ib = jh_ - 2, ia = jh_ - 3; i32 disp = 0;
            int cc = -1;
            if (jv[ic].k == VK_CC) cc = jv[ic].cc;
            int da = jgpr(ia, 0), r2 = -1, bm = 0;
            if (jmemop(ib, &disp)) bm = 1;
            else if (jv[ib].k == VK_CONST) { mov_ri(RAX, jv[ib].c); r2 = RAX; }
            else r2 = jgpr(ib, 1u << da);
            if (cc < 0) {
                i32 d2;
                if (jv[ic].k == VK_GPR) jrr(0, 0, 0x85, jv[ic].r, jv[ic].r, 0);
                else if (jmemop(ic, &d2)) { jrm(0, 0, 0x83, 7, RBX, -1, 0, d2, 0); j8(0); }
                else { int rc = jgpr(ic, (1u << da) | (r2 >= 0 && r2 != RAX ? 1u << r2 : 0)); jrr(0, 0, 0x85, rc, rc, 0); }
                cc = 5;
            }
            if (bm) jrm(0, 1, 0x0F40u + (u32)(cc ^ 1), da, RBX, -1, 0, disp, 0); else jrr(0, 1, 0x0F40u + (u32)(cc ^ 1), da, r2, 0);
            jrel(ic); jrel(ib); jh_ -= 2;
            break;
        }
        case 0x20: {
            u32 i = leb_u(&p, end); if (i >= jnloc) { jfail = 1; return -1; }
            jpush(VK_LOC); jv[jh_ - 1].v = i; break;
        }
        case 0x21: case 0x22: {
            u32 i = leb_u(&p, end); if (i >= jnloc) { jfail = 1; return -1; }
            JNEED(1);
            u32 t = jh_ - 1;
            for (u32 j = jctl[jnctl - 1].h; j < t; j++) if (jv[j].k == VK_LOC && jv[j].v == i) jgpr(j, 0);   /* запись в i не должна менять отложенное чтение i */
            if (!(jv[t].k == VK_LOC && jv[t].v == i)) store_entry(t, RBX, (i32)(8 * i));
            if (op == 0x21) jpop(1);
            break;
        }
        case 0x23: {
            u32 i = leb_u(&p, end); if (i >= nglobals) { jfail = 1; return -1; }
            int r = jalloc_gpr(jh_, 0); mov_rm(1, r, R12, (i32)(8 * i));
            jpush(VK_GPR); jset_gpr(jh_ - 1, r); break;
        }
        case 0x24: {
            u32 i = leb_u(&p, end); if (i >= nglobals) { jfail = 1; return -1; }
            JNEED(1); store_entry(jh_ - 1, R12, (i32)(8 * i)); jpop(1); break;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: {
            leb_u(&p, end); u32 off = leb_u(&p, end);
            if (jmemacc(op, off)) return -1;
            break;
        }
        case 0x3F: { p++; int r = jalloc_gpr(jh_, 0); mov_rr(1, r, R14); jrr(0, 1, 0xC1, 5, r, 0); j8(16); jpush(VK_GPR); jset_gpr(jh_ - 1, r); break; }
        case 0x40: {
            p++; JNEED(1);
            jspill_regs(); load_to(RDI, jh_ - 1); jcall_h(CXO(h_grow));
            mov_rm(1, R14, R13, CXO(ml));
            jpop(1); int r = jalloc_gpr(jh_, 0); mov_rm(1, r, R13, CXO(res)); jpush(VK_GPR); jset_gpr(jh_ - 1, r);
            break;
        }
        case 0x41: jpush(VK_CONST); jv[jh_ - 1].c = (u32)leb_s(&p, end, 32); break;
        case 0x42: jpush(VK_CONST); jv[jh_ - 1].c = (u64)leb_s(&p, end, 64); break;
        case 0x43: { if (p + 4 > end) { jfail = 1; return -1; } jpush(VK_CONST); jv[jh_ - 1].c = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; p += 4; break; }
        case 0x44: { if (p + 8 > end) { jfail = 1; return -1; } u64 v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; p += 8; jpush(VK_CONST); jv[jh_ - 1].c = v; break; }
        case 0x45: JNEED(1); jeqz(0); break;
        case 0x50: JNEED(1); jeqz(1); break;
        case 0xFC: {
            u32 sub = leb_u(&p, end);
            if (sub <= 7) { if (jhelper(0x100 + sub, 1)) return -1; }
            else if (sub == 10 || sub == 11) {
                p += sub == 10 ? 2 : 1; JNEED(3);
                jspill_regs(); mov_ri(RDI, sub);
                load_to(RSI, jh_ - 3); load_to(RDX, jh_ - 2); load_to(RCX, jh_ - 1);
                jcall_h(CXO(h_memop)); jrr(0, 0, 0x85, RAX, RAX, 0); jjnz_stop();
                jpop(3);
            } else { jfail = 1; return -1; }
            break;
        }
        default:
            if (op >= 0x46 && op <= 0x4F) { JNEED(2); jcmp_int(op - 0x46u, 0); break; }
            if (op >= 0x51 && op <= 0x5A) { JNEED(2); jcmp_int(op - 0x51u, 1); break; }
            if (op >= 0x5B && op <= 0x60) { JNEED(2); jfcmp(0, op - 0x5Bu); break; }
            if (op >= 0x61 && op <= 0x66) { JNEED(2); jfcmp(1, op - 0x61u); break; }
            if ((op >= 0x6A && op <= 0x78) || (op >= 0x7C && op <= 0x8A)) {
                JNEED(2);
                int w = op >= 0x7C; u32 k = op - (w ? 0x7Cu : 0x6Au);
                if (k <= 2 || (k >= 7 && k <= 9)) jint_arith(k, w);
                else if (k >= 3 && k <= 6) jdiv(k, w);
                else jshift(k, w);                                // 10..12 shl shr_s shr_u, 13 rotl, 14 rotr
                break;
            }
            if (op >= 0x92 && op <= 0x95) { JNEED(2); jfarith(0, op - 0x92u); break; }
            if (op >= 0xA0 && op <= 0xA3) { JNEED(2); jfarith(1, op - 0xA0u); break; }
            if (op == 0x91 || op == 0x9F) {                       /* sqrt */
                JNEED(1); int dbl = op == 0x9F; u32 t = jh_ - 1;
                int x = jxmm(t, dbl, 0); sse_rr(dbl ? 0xF2 : 0xF3, 0x0F51, x, x); break;
            }
            if (op == 0x8B || op == 0x8C) {                       /* f32.abs / f32.neg: биты через eax */
                JNEED(1); u32 t = jh_ - 1; int x = jxmm(t, 0, 0);
                movd_gx(0, RAX, x); alu_ri(op == 0x8B ? A_AND : A_XOR, 0, RAX, op == 0x8B ? 0x7FFFFFFF : (i32)0x80000000u); movd_xg(0, x, RAX); break;
            }
            if (op == 0x99 || op == 0x9A) {                       /* f64.abs / f64.neg: btr / btc бита 63 */
                JNEED(1); u32 t = jh_ - 1; int x = jxmm(t, 1, 0);
                movd_gx(1, RAX, x); jrr(0, 1, 0x0FBA, op == 0x99 ? 6 : 7, RAX, 0); j8(63); movd_xg(1, x, RAX); break;
            }
            if (op == 0x67 || op == 0x68 || op == 0x69 || op == 0x79 || op == 0x7A || op == 0x7B || (op >= 0x8D && op <= 0x90) || (op >= 0x9B && op <= 0x9E)) {
                if (jhelper(op, 1)) return -1;
                break;
            }
            if (op == 0x96 || op == 0x97 || op == 0x98 || op == 0xA4 || op == 0xA5 || op == 0xA6) { JNEED(2); if (jhelper(op, 2)) return -1; break; }
            if (op >= 0xA7 && op <= 0xC4) { if (jconv(op)) return -1; break; }
            jfail = 1; return -1;
        }
    }
    jfail = 1; return -1;
}

/* ---------- модуль ---------- */
typedef u64 (*jit_entry_t)(jctx_t*, u64*, u64);
static jit_entry_t jit_entry; static u32 jit_code_size;
static void jtrap_tails(void) {
    for (u32 k = 1; k < TK_N; k++) {
        if (!tchain[k]) continue;
        jpatch(tchain[k], jn);
        jrm(0, 0, 0xC7, 0, R13, -1, 0, CXO(trap), 0); j32(k);     /* mov dword [r13+trap], k */
        jjmp_to(stop_off);
    }
}
static int jit_build(void) {
    if (!jit_on) return -1;
    if (nfuncs == 0 || entry >= nfuncs) return -1;
    u32 total = 0;
    for (u32 i = 0; i < nfuncs; i++) {
        if (funcs[i].type >= ntypes) return -1;
        if (!funcs[i].import) total += (u32)(funcs[i].end - funcs[i].code);
    }
    u64 cap = 16ull * total + 192ull * nfuncs + 8192;             /* код получается в 2-4 раза больше байт WASM; не хватит - модуль остаётся интерпретатору */
    if (cap > (8u << 20)) cap = 8u << 20;
    jcap = (u32)cap; jb = (u8*)alloc(jcap);
    cchain = (u32*)alloc(4ull * nfuncs); jfnoff = (u32*)alloc(4ull * nfuncs);
    u64* jfn = (u64*)alloc(8ull * nfuncs); u16* fsig = (u16*)alloc(2ull * nfuncs + 16);
    u8* stk = (u8*)alloc(JIT_STACK);
    if (!jb || !cchain || !jfnoff || !jfn || !fsig || !stk) return -1;
    jn = 0; jfail = 0; pool_n = 0; npref = 0;
    for (int k = 0; k < TK_N; k++) tchain[k] = 0;
    /* вход из C: сохранить регистры, переключиться на свой стек, вызвать функцию; при ловушке - откат на сохранённый стек */
    j8(0x53); j8(0x55); j8(0x41); j8(0x54); j8(0x41); j8(0x55); j8(0x41); j8(0x56); j8(0x41); j8(0x57);   /* push rbx rbp r12 r13 r14 r15 */
    mov_rr(1, R13, RDI); mov_rr(1, RBX, RSI);
    mov_rm(1, R12, R13, CXO(glob)); mov_rm(1, R15, R13, CXO(mem)); mov_rm(1, R14, R13, CXO(ml));
    mov_mr(1, R13, CXO(saved_rsp), RSP);
    mov_rm(1, RSP, R13, CXO(stacktop));
    j8(0xFF); j8(0xD2);                                           /* call rdx */
    mov_rm(1, RSP, R13, CXO(saved_rsp)); mov_ri(RAX, 0);
    u32 pops = jn;
    j8(0x41); j8(0x5F); j8(0x41); j8(0x5E); j8(0x41); j8(0x5D); j8(0x41); j8(0x5C); j8(0x5D); j8(0x5B); j8(0xC3);   /* pop r15 r14 r13 r12 rbp rbx; ret */
    stop_off = jn;
    mov_rm(1, RSP, R13, CXO(saved_rsp)); mov_ri(RAX, 1); j8(0xE9); j32(pops - (jn + 4));
    for (u32 fi = 0; fi < nfuncs && !jfail; fi++) {
        const ftype_t* t = &types[funcs[fi].type];
        fsig[fi] = (u16)(t->np << 8 | t->nr);
        if (funcs[fi].import) {
            jfnoff[fi] = jn;
            alu_ri(A_SUB, 1, RSP, 8); mov_ri(RDI, fi); mov_rr(1, RSI, RBX); jcall_h(CXO(h_host));
            alu_ri(A_ADD, 1, RSP, 8); jrr(0, 0, 0x85, RAX, RAX, 0); jjnz_stop(); j8(0xC3);
        } else if (jcompile_func(fi)) return -1;
    }
    if (jfail) return -1;
    jtrap_tails();
    jpool_flush();
    for (u32 fi = 0; fi < nfuncs; fi++) jpatch(cchain[fi], jfnoff[fi]);
    if (jfail) return -1;
    if (jn > (JIT_MAX_PAGES - 1) * 4096) return -1;                /* окно кода ядра - не больше JIT_MAX_PAGES страниц */
    u64 pages = ((u64)jn + 4095) / 4096;
    u8* dst = (u8*)k_jit_alloc(pages);
    if (!dst) return -1;
    for (u32 i = 0; i < jn; i++) dst[i] = jb[i];
    if (k_jit_seal(dst, pages) != 0) return -1;
    for (u32 fi = 0; fi < nfuncs; fi++) jfn[fi] = (u64)(dst + jfnoff[fi]);
    jc.ml = (u64)mem_pages * 65536; jc.vlim = (u64)(vs + VSTK); jc.slim = (u64)stk + 16384; jc.saved_rsp = 0; jc.ctr = JIT_TICKS; jc.trap = 0;
    jc.table = (u64)table; jc.ntable = ntable; jc.fsig = (u64)fsig; jc.jfn = (u64)jfn; jc.mem = (u64)mem; jc.glob = (u64)globals;
    jc.stacktop = ((u64)stk + JIT_STACK) & ~15ull; jc.res = 0;
    jc.h_host = (u64)jh_host; jc.h_un = (u64)jh_un; jc.h_bin = (u64)jh_bin; jc.h_tick = (u64)jh_tick; jc.h_grow = (u64)jh_grow; jc.h_memop = (u64)jh_memop;
    jit_entry = (jit_entry_t)dst; jit_code_size = jn;
    return 0;
}
/* Выполняет fi машинным кодом после успешного jit_build (результат и состояние - как после execute). */
static void jit_exec(u32 fi) {
    const ftype_t* t = &types[funcs[fi].type];
    for (u32 i = 0; i < t->np; i++) vs[i] = 0;
    sp = 0;
    u64 r = jit_entry(&jc, vs, (u64)((const u8*)jit_entry + jfnoff[fi]));
    if (r && jc.trap) trap(tk_msg[jc.trap]);
    if (r && !trapped && !exit_req) trap("stopped");
    sp = t->nr;
}
