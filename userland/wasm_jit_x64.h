/* wasm_jit_x64.h - шаблонный JIT WebAssembly -> x86-64 (подключается из wasm.c только на x86-64; на ARM64 работает интерпретатор).
   Модуль уже разобран предкомпилятором интерпретатора (структура, высоты стека проверены), JIT идёт вторым проходом прямо по
   байтам WASM и при любой неподдержанной команде просто отказывается: тогда модуль целиком выполняет интерпретатор.

   Как устроен код:
   - Значения живут в 64-битных ячейках стека значений vs[] (как у интерпретатора): локальные - [rbx + 8*i], операндный стек
     функции - дальше [rbx + 8*(nloc + h)]. Вызов: аргументы уже лежат в ячейках будущих параметров, rbx сдвигается на начало
     окна, результат возвращается в [rbx + 0]. Машинный стек хранит только адреса возврата (свой стек из кучи, с проверкой предела).
   - Внутри базового блока значения держатся в регистрах (rsi rdi r8-r11 для целых, xmm0-11 для вещественных); local.get и
     константы ленивы (подставляются в команду операндом-памятью или непосредственным числом); результат сравнения - флаги
     (ветка/select/eqz берут их напрямую). На границах блоков, переходах и вызовах всё сброшено в ячейки.
   - Память модуля: r15 - база, r14 - длина. Каждый доступ проверяется (ea + размер <= длины); константный адрес внутри начальной
     памяти не проверяется (память не уменьшается). Деление, преобразования в целое, стек, call_indirect проверяются.
   - r12 - глобальные, r13 - контекст jctx_t (счётчик квантов, предел стеков, помощники на C). Редкие команды (clz, round, min/max,
     64-битные преобразования) вызывают помощников на C, как и ввод-вывод.
   - Ловушка из кода: код ловушки в контекст, откат на машинный стек входа, возврат в C. Помощник, поймавший ловушку, возвращает 1.
   - Память кода: JIT собирает код в куче (RW-NX), затем один раз просит ядро выдать окно кода (SYS_JIT, CAP_JIT) и запечатать
     его R-X; страница никогда не бывает W+X, после запечатывания код неизменен. */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
typedef int i32;
typedef struct {
    u64 ml, vlim, slim, saved_rsp;
    u32 ctr, trap;
    u64 table, ntable, fsig, jfn, mem, glob, stacktop, res;
    u64 h_host, h_un, h_bin, h_tick, h_grow, h_memop;
} jctx_t;
#define CXO(f) ((i32)__builtin_offsetof(jctx_t, f))
enum { TK_NONE, TK_OOB, TK_DIV0, TK_OVF, TK_CONV, TK_UNREACH, TK_NOENT, TK_BADFN, TK_SIG, TK_CSTACK, TK_VSTACK, TK_N };
static const char* const tk_msg[TK_N] = { 0, "memory access out of bounds", "integer divide by zero", "integer overflow",
    "invalid conversion to integer (NaN or out of range)", "unreachable executed", "call_indirect: no such table entry",
    "call_indirect: bad function", "call_indirect: signature mismatch", "call stack exhausted", "value stack overflow" };
static jctx_t jc;
static int jit_on = 1;
#define JIT_TICKS SLICE_TICKS
#define JIT_MAX_PAGES 256u                  /* окно кода JIT в ядре (proc.h): не больше 1 МБ */
#define JIT_STACK (192 * 1024)              /* машинный стек JIT-кода: адреса возврата и кадры помощников на C */

/* ---------- помощники на C (вызываются из кода по таблице в контексте) ---------- */
static u64 jh_tick(void) { jc.ctr = JIT_TICKS; return (u64)wasm_slice(); }
static u64 jh_host(u64 fi, u64* fp) {
    const func_t* c = &funcs[fi]; const ftype_t* t = &types[c->type];
    host_call(c->imp_id, t, fp + t->np);                          /* аргументы сняты, результат - в fp[0] */
    return (u64)(trapped || exit_req);
}
static u64 jh_grow(u64 d) {
    u32 dd = (u32)d, old = mem_pages;
    if ((u64)mem_pages + dd > mem_max) jc.res = 0xFFFFFFFFu;
    else { mem_pages += dd; jc.res = old; jc.ml = (u64)mem_pages * 65536; }
    return 0;
}
static u64 jh_memop(u64 op, u64 dst, u64 v, u64 n32) {
    u32 n = (u32)n32, d = (u32)dst; u64 ml = (u64)mem_pages * 65536;
    if (op == 10) {
        u32 src = (u32)v;
        if ((u64)src + n > ml || (u64)d + n > ml) { trap("memory.copy out of bounds"); return 1; }
        if (d < src) for (u32 i = 0; i < n; i++) mem[d + i] = mem[src + i]; else for (u32 i = n; i > 0; i--) mem[d + i - 1] = mem[src + i - 1];
    } else {
        if ((u64)d + n > ml) { trap("memory.fill out of bounds"); return 1; }
        for (u32 i = 0; i < n; i++) mem[d + i] = (u8)v;
    }
    return 0;
}
/* Редкие одноместные команды: op - код WASM (0x100 + n - насыщающие 0xFC n), a - сырые биты; результат в jc.res. */
static u64 jh_un(u64 op, u64 a) {
    u64 r = 0;
    switch (op) {
    case 0x67: r = clz32((u32)a); break;  case 0x68: r = ctz32((u32)a); break;  case 0x69: r = pop32((u32)a); break;
    case 0x79: r = clz64(a); break;       case 0x7A: r = ctz64(a); break;       case 0x7B: r = pop64(a); break;
    case 0x8D: r = pf(ceil_f(gf(a))); break;  case 0x8E: r = pf(floor_f(gf(a))); break;
    case 0x8F: r = pf(trunc_f(gf(a))); break; case 0x90: r = pf(nearest_f(gf(a))); break;
    case 0x9B: r = pd(ceil_d(gd(a))); break;  case 0x9C: r = pd(floor_d(gd(a))); break;
    case 0x9D: r = pd(trunc_d(gd(a))); break; case 0x9E: r = pd(nearest_d(gd(a))); break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAE: case 0xAF: case 0xB0: case 0xB1: {
        double x = (op == 0xA8 || op == 0xA9 || op == 0xAE || op == 0xAF) ? (double)gf(a) : gd(a);
        int bits = op <= 0xAB ? 32 : 64, sgn = (int)((op - (op <= 0xAB ? 0xA8 : 0xAE)) % 2) == 0;
        if (!to_int(x, bits, sgn, &r)) { trap(x != x ? "invalid conversion to integer (NaN)" : "integer overflow in float-to-int conversion"); return 1; }
        break;
    }
    case 0xB5: r = pf(u64_to_f(a)); break;  case 0xBA: r = pd(u64_to_d(a)); break;
    default:
        if (op >= 0x100 && op <= 0x107) {
            u32 sub = (u32)op - 0x100;
            double x = (sub == 0 || sub == 1 || sub == 4 || sub == 5) ? (double)gf(a) : gd(a);
            to_int(x, sub <= 3 ? 32 : 64, sub % 2 == 0, &r);
            break;
        }
        trap("unknown instruction"); return 1;
    }
    jc.res = r; return 0;
}
static u64 jh_bin(u64 op, u64 a, u64 b) {
    u64 r;
    switch (op) {
    case 0x96: r = pf(min_f(gf(a), gf(b))); break;  case 0x97: r = pf(max_f(gf(a), gf(b))); break;
    case 0x98: r = (a & 0x7FFFFFFFu) | (b & 0x80000000u); break;
    case 0xA4: r = pd(min_d(gd(a), gd(b))); break;  case 0xA5: r = pd(max_d(gd(a), gd(b))); break;
    case 0xA6: r = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); break;
    default: trap("unknown instruction"); return 1;
    }
    jc.res = r; return 0;
}

/* ---------- кодирование x86-64 ---------- */
static u8* jb; static u32 jn, jcap; static int jfail;
static void j8(u32 b) { if (jn >= jcap) { jfail = 1; return; } jb[jn++] = (u8)b; }
static void j32(u32 v) { j8(v); j8(v >> 8); j8(v >> 16); j8(v >> 24); }
static void j64(u64 v) { j32((u32)v); j32((u32)(v >> 32)); }
static u32 rd32(u32 at) { u32 v = 0; if (at + 4 <= jn) for (int i = 3; i >= 0; i--) v = v << 8 | jb[at + i]; return v; }
static void wr32(u32 at, u32 v) { if (at + 4 <= jn) for (int i = 0; i < 4; i++) jb[at + i] = (u8)(v >> (8 * i)); }
static void jrex(int w, int reg, int idx, int base, int byteop) {
    u32 r = 0x40 | (w ? 8u : 0u) | ((reg & 8) ? 4u : 0u) | ((idx & 8) ? 2u : 0u) | ((base & 8) ? 1u : 0u);
    if (r != 0x40 || byteop) j8(r);
}
static void jopc(u32 o) { if (o > 0xFFFF) j8(o >> 16); if (o > 0xFF) j8(o >> 8); j8(o); }
static void jrr(u32 pfx, int w, u32 opc, int reg, int rm, int byteop) {
    if (pfx) j8(pfx);
    jrex(w, reg, 0, rm, byteop); jopc(opc); j8(0xC0 | (u32)(reg & 7) << 3 | (u32)(rm & 7));
}
static void jrm(u32 pfx, int w, u32 opc, int reg, int base, int idx, int sc, i32 disp, int byteop) {
    if (pfx) j8(pfx);
    jrex(w, reg, idx < 0 ? 0 : idx, base, byteop); jopc(opc);
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    if (idx >= 0 || (base & 7) == 4) { j8((u32)mod << 6 | (u32)(reg & 7) << 3 | 4); j8((u32)sc << 6 | (u32)(idx < 0 ? 4 : (idx & 7)) << 3 | (u32)(base & 7)); }
    else j8((u32)mod << 6 | (u32)(reg & 7) << 3 | (u32)(base & 7));
    if (mod == 1) j8((u32)disp); else if (mod == 2) j32((u32)disp);
}
static void mov_rr(int w, int d, int s) { if (w || d != s) jrr(0, w, 0x8B, d, s, 0); }
static void mov_rm(int w, int d, int base, i32 disp) { jrm(0, w, 0x8B, d, base, -1, 0, disp, 0); }
static void mov_mr(int w, int base, i32 disp, int s) { jrm(0, w, 0x89, s, base, -1, 0, disp, 0); }
static void mov_ri(int d, u64 v) {                                /* флаги не трогает (нужен mov, не xor) */
    if (v <= 0xFFFFFFFFull) { if (d >= 8) j8(0x41); j8(0xB8 + (u32)(d & 7)); j32((u32)v); }
    else if ((i64)v < 0 && (i64)v >= -2147483648ll) { jrex(1, 0, 0, d, 0); j8(0xC7); j8(0xC0 | (u32)(d & 7)); j32((u32)v); }
    else { jrex(1, 0, 0, d, 0); j8(0xB8 + (u32)(d & 7)); j64(v); }
}
enum { A_ADD = 0, A_OR = 1, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };
static void alu_rr(int op, int w, int d, int s) { jrr(0, w, (u32)op * 8 + 3, d, s, 0); }
static void alu_rm(int op, int w, int d, int base, i32 disp) { jrm(0, w, (u32)op * 8 + 3, d, base, -1, 0, disp, 0); }
static void alu_ri(int op, int w, int d, i32 imm) {
    if (imm >= -128 && imm <= 127) { jrr(0, w, 0x83, op, d, 0); j8((u32)imm); } else { jrr(0, w, 0x81, op, d, 0); j32((u32)imm); }
}
static void sse_rr(u32 pfx, u32 opc, int d, int s) { jrr(pfx, 0, opc, d, s, 0); }
static void sse_rm(u32 pfx, u32 opc, int x, int base, int idx, i32 disp) { jrm(pfx, 0, opc, x, base, idx, 0, disp, 0); }
static void sse_ld(int dbl, int x, int base, i32 disp) { sse_rm(dbl ? 0xF2 : 0xF3, 0x0F10, x, base, -1, disp); }
static void sse_st(int dbl, int base, i32 disp, int x) { sse_rm(dbl ? 0xF2 : 0xF3, 0x0F11, x, base, -1, disp); }
static void movd_xg(int dbl, int x, int g) { jrr(0x66, dbl, 0x0F6E, x, g, 0); }      /* xmm <- gpr */
static void movd_gx(int dbl, int g, int x) { jrr(0x66, dbl, 0x0F7E, x, g, 0); }      /* gpr <- xmm */
static void setcc_al_zx(int cc, int r) { jrr(0, 0, 0x0F90u + (u32)cc, 0, RAX, 0); jrr(0, 0, 0x0FB6, r, RAX, 0); }   /* setcc al; movzx r32, al */

/* переходы: цепочки заплаток по слову rel32 (в самом слове - предыдущее звено, +1; 0 - конец) */
static u32 tchain[TK_N]; static u32* cchain; static u32* jfnoff;
static void jchain(u32* head) { u32 slot = jn; j32(*head); *head = slot + 1; }
static void jpatch(u32 chain, u32 target) { while (chain) { u32 s = chain - 1, nx = rd32(s); wr32(s, target - (s + 4)); chain = nx; } }
static u32 stop_off;                                              /* адрес stop_exit: общий выход при ловушке */
static void jjcc_chain(int cc, u32* head) { j8(0x0F); j8(0x80u + (u32)cc); jchain(head); }
static void jjmp_chain(u32* head) { j8(0xE9); jchain(head); }
static void jjmp_to(u32 target) { j8(0xE9); j32(target - (jn + 4)); }
static void jjcc_trap(int cc, int k) { jjcc_chain(cc, &tchain[k]); }
static void jjmp_trap(int k) { jjmp_chain(&tchain[k]); }
static u32 jshort(int cc) { if (cc < 0) j8(0xEB); else j8(0x70u + (u32)cc); j8(0); return jn; }   /* rel8 вперёд; заплатка jshort_end */
static void jshort_end(u32 pos) { if (pos <= jn && jn - pos < 128) jb[pos - 1] = (u8)(jn - pos); else jfail = 1; }
static void jjnz_stop(void) { j8(0x0F); j8(0x85); j32(stop_off - (jn + 4)); }

/* Пул вещественных констант в конце кода: операнд [rip + c] вместо mov + movd (меньше команд на каждую константу). */
#define POOL_MAX 512
#define PREF_MAX 4096
static u64 pool_v[POOL_MAX]; static u32 pool_n; static u32 pref_pos[PREF_MAX]; static u16 pref_idx[PREF_MAX]; static u32 npref;
static int sse_rip(u32 pfx, u32 opc, int x, u64 v) {              /* 1 - команда записана, 0 - пул полон (вызывающий берёт обычный путь) */
    u32 i = 0;
    while (i < pool_n && pool_v[i] != v) i++;
    if (i == pool_n) { if (pool_n >= POOL_MAX) return 0; pool_v[pool_n++] = v; }
    if (npref >= PREF_MAX) return 0;
    if (pfx) j8(pfx);
    jrex(0, x, 0, 0, 0); jopc(opc); j8((u32)(x & 7) << 3 | 5);
    pref_pos[npref] = jn; pref_idx[npref++] = (u16)i; j32(0);
    return 1;
}
static void jpool_flush(void) {
    while (jn & 7) j8(0xCC);
    u32 off = jn;
    for (u32 i = 0; i < pool_n; i++) j64(pool_v[i]);
    for (u32 i = 0; i < npref; i++) wr32(pref_pos[i], off + 8 * pref_idx[i] - (pref_pos[i] + 4));
}

/* ---------- виртуальный стек ---------- */
enum { VK_MEM, VK_GPR, VK_XMM, VK_LOC, VK_CONST, VK_CC };
typedef struct { u8 k, r, w, cc; u32 v; u64 c; } jv_t;
#define JV_MAX 2048
static jv_t jv[JV_MAX]; static u32 jh_, jmaxh, jnloc;
static u32 gown[16], xown[16];
static const u8 gpool[6] = { RSI, RDI, R8, R9, R10, R11 };
#define NXP 12
#define SLOT(i) ((i32)(8u * (jnloc + (i))))

static void store_entry(u32 i, int base, i32 disp);
static void jrel(u32 i) {
    jv_t* e = &jv[i];
    if (e->k == VK_GPR) gown[e->r] = 0; else if (e->k == VK_XMM) xown[e->r] = 0;
    e->k = VK_MEM;
}
static void jspill(u32 i) {
    jv_t* e = &jv[i];
    if (e->k == VK_MEM || e->k == VK_LOC || e->k == VK_CONST) return;
    store_entry(i, RBX, SLOT(i)); jrel(i);
}
static int jalloc_gpr(u32 idx, u32 avoid) {
    for (int k = 0; k < 6; k++) { int r = gpool[k]; if (!gown[r] && !(avoid >> r & 1)) { gown[r] = idx + 1; return r; } }
    int best = -1; u32 bi = ~0u;
    for (int k = 0; k < 6; k++) { int r = gpool[k]; if (!(avoid >> r & 1) && gown[r] && gown[r] - 1 < bi) { bi = gown[r] - 1; best = r; } }
    if (best < 0 || bi >= jh_) { jfail = 1; return RSI; }
    jspill(bi); gown[best] = idx + 1; return best;
}
static int jalloc_xmm(u32 idx, u32 avoid) {
    for (int x = 0; x < NXP; x++) if (!xown[x] && !(avoid >> x & 1)) { xown[x] = idx + 1; return x; }
    int best = -1; u32 bi = ~0u;
    for (int x = 0; x < NXP; x++) if (!(avoid >> x & 1) && xown[x] && xown[x] - 1 < bi) { bi = xown[x] - 1; best = x; }
    if (best < 0 || bi >= jh_) { jfail = 1; return 0; }
    jspill(bi); xown[best] = idx + 1; return best;
}
static void load_to(int hw, u32 i) {                              /* сырые 64 бита значения - в регистр hw (rax/rcx/rdx/любой) */
    jv_t* e = &jv[i];
    switch (e->k) {
    case VK_GPR: mov_rr(1, hw, e->r); break;
    case VK_XMM: movd_gx(e->w == 8, hw, e->r); break;
    case VK_MEM: mov_rm(1, hw, RBX, SLOT(i)); break;
    case VK_LOC: mov_rm(1, hw, RBX, (i32)(8 * e->v)); break;
    case VK_CONST: mov_ri(hw, e->c); break;
    default: setcc_al_zx(e->cc, hw); break;
    }
}
static int jgpr(u32 i, u32 avoid) {
    jv_t* e = &jv[i];
    if (e->k == VK_GPR) return e->r;
    int r;
    if (e->k == VK_XMM) { int x = e->r, w = e->w; xown[x] = 0; r = jalloc_gpr(i, avoid); movd_gx(w == 8, r, x); }
    else { r = jalloc_gpr(i, avoid); load_to(r, i); }
    e->k = VK_GPR; e->r = (u8)r; return r;
}
static int jxmm(u32 i, int dbl, u32 avoid) {
    jv_t* e = &jv[i];
    if (e->k == VK_XMM) return e->r;
    if (e->k == VK_CC) jgpr(i, 0);
    int x;
    if (e->k == VK_GPR) { int g = e->r; gown[g] = 0; x = jalloc_xmm(i, avoid); movd_xg(dbl, x, g); }
    else if (e->k == VK_MEM) { x = jalloc_xmm(i, avoid); sse_ld(dbl, x, RBX, SLOT(i)); }
    else if (e->k == VK_LOC) { x = jalloc_xmm(i, avoid); sse_ld(dbl, x, RBX, (i32)(8 * e->v)); }
    else {                                                        /* константа */
        x = jalloc_xmm(i, avoid);
        u64 c = dbl ? e->c : (u32)e->c;
        if (!c) jrr(0, 0, 0x0F57, x, x, 0);
        else if (!sse_rip(dbl ? 0xF2 : 0xF3, 0x0F10, x, c)) { mov_ri(RAX, c); movd_xg(dbl, x, RAX); }
    }
    e->k = VK_XMM; e->r = (u8)x; e->w = dbl ? 8 : 4; return x;
}
static void jpush(u8 k) { if (jh_ >= JV_MAX) { jfail = 1; return; } jv[jh_].k = k; jh_++; if (jh_ > jmaxh) jmaxh = jh_; }
static void jset_gpr(u32 i, int r) { jv[i].k = VK_GPR; jv[i].r = (u8)r; gown[r] = i + 1; }
static void jset_xmm(u32 i, int x, int dbl) { jv[i].k = VK_XMM; jv[i].r = (u8)x; jv[i].w = dbl ? 8 : 4; xown[x] = i + 1; }
static void jpop(u32 n) { while (n--) { jrel(jh_ - 1); jh_--; } }
static int jmemop(u32 i, i32* disp) {                             /* операнд прямо из памяти (локальная или ячейка стека) */
    if (jv[i].k == VK_LOC) { *disp = (i32)(8 * jv[i].v); return 1; }
    if (jv[i].k == VK_MEM) { *disp = SLOT(i); return 1; }
    return 0;
}
/* Значение записи i - в память base+disp (ячейка стека, локальная, глобальная). Флаги не меняются. */
static void store_entry(u32 i, int base, i32 disp) {
    jv_t* e = &jv[i];
    switch (e->k) {
    case VK_GPR: mov_mr(1, base, disp, e->r); break;
    case VK_XMM: sse_st(e->w == 8, base, disp, e->r); break;
    case VK_MEM: if (base != RBX || disp != SLOT(i)) { mov_rm(1, RAX, RBX, SLOT(i)); mov_mr(1, base, disp, RAX); } break;
    case VK_LOC: if (base != RBX || disp != (i32)(8 * e->v)) { mov_rm(1, RAX, RBX, (i32)(8 * e->v)); mov_mr(1, base, disp, RAX); } break;
    case VK_CONST:
        if (e->c <= 0x7FFFFFFFull) { jrm(0, 1, 0xC7, 0, base, -1, 0, disp, 0); j32((u32)e->c); }
        else { mov_ri(RAX, e->c); mov_mr(1, base, disp, RAX); }
        break;
    default: setcc_al_zx(e->cc, RAX); mov_mr(1, base, disp, RAX); break;
    }
}
static void jcanon(u32 from) { for (u32 i = from; i < jh_; i++) if (jv[i].k != VK_MEM) { store_entry(i, RBX, SLOT(i)); jrel(i); } }
static void jspill_regs(void) { for (u32 i = 0; i < jh_; i++) if (jv[i].k == VK_GPR || jv[i].k == VK_XMM || jv[i].k == VK_CC) jspill(i); }
static void jmaterialize_cc(void) { if (jh_ && jv[jh_ - 1].k == VK_CC) jgpr(jh_ - 1, 0); }

/* ---------- структура функции ---------- */
typedef struct { u8 kind, ar, dead; u32 h, start, pend, ifz; } jctl_t;   /* kind: 0 block, 1 loop, 2 if, 3 функция */
static jctl_t jctl[260]; static int jnctl, junr;
static u32 jnr;                                                   /* число результатов текущей функции */
static void jret_seq(void) {
    if (jnr) { if (jh_ == 0) { jfail = 1; return; } store_entry(jh_ - 1, RBX, 0); }
    alu_ri(A_ADD, 1, RSP, 8); j8(0xC3);
}
static void jtick(void) {
    jrm(0, 0, 0x83, 5, R13, -1, 0, CXO(ctr), 0); j8(1);           /* sub dword [r13+ctr], 1 */
    u32 sk = jshort(5);                                           /* jnz */
    jrm(0, 0, 0xFF, 2, R13, -1, 0, CXO(h_tick), 0);               /* call [r13+h_tick] */
    jrr(0, 0, 0x85, RAX, RAX, 0); jjnz_stop();
    jshort_end(sk);
}
/* переход к рамке f (cc < 0 - безусловный); значения для результата блока уже в нужной ячейке */
static void jbr_frame(jctl_t* f, int cc) {
    if (f->kind == 1) { if (cc >= 0) { j8(0x0F); j8(0x80u + (u32)cc); j32(f->start - (jn + 4)); } else jjmp_to(f->start); }
    else if (cc >= 0) jjcc_chain(cc, &f->pend); else jjmp_chain(&f->pend);
}
/* перенос верхнего значения (результат блока) в ячейку рамки; состояние стека не меняется */
static void jmove_result(const jctl_t* f) {
    u32 keep = f->kind == 1 ? 0 : f->ar;
    if (keep && jh_ >= f->h + keep) store_entry(jh_ - 1, RBX, SLOT(f->h));
}
/* флаги по условию на вершине; возвращает код условия «истина»; значение снимается */
static int jcond_flags(void) {
    u32 t = jh_ - 1; jv_t* e = &jv[t]; i32 disp; int cc = 5;
    if (e->k == VK_CC) cc = e->cc;
    else if (e->k == VK_GPR) jrr(0, 0, 0x85, e->r, e->r, 0);
    else if (jmemop(t, &disp)) { jrm(0, 0, 0x83, 7, RBX, -1, 0, disp, 0); j8(0); }
    else { int r = jgpr(t, 0); jrr(0, 0, 0x85, r, r, 0); }
    jpop(1); return cc;
}

#include "wasm_jit_x64_gen.h"
