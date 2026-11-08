/* Хост-эмулятор EL0 для проверки AArch64-кода kcc без железа ARM (нет qemu-user).
 * Загружает ELF ARM64 Киры настоящим elf_parse() ядра (окно пользователя ARM64, W^X, выравнивание), отображает сегменты в своей памяти с правами
 * R-X / R-- / RW- и выполняет ТОЛЬКО то подмножество A64, которое выдаёт kcc (любая другая инструкция - стоп с кодом 97). Системные вызовы
 * Киры (svc #0, номер в x8) обслуживаются так же, как в tests/kcc/kloader.c (K_WRITE, K_EXIT, K_TICKS, K_HEAP, K_FS, K_ARGS...).
 * Декодер написан независимо от кодировщика kcc/asm_a64.c; кодировки кодировщика отдельно сверены с llvm-mc (a64_enc.c).
 * Эксклюзивные пары (ldaxr/stlxr) выполняются на одном ядре: монитор всегда цел, stlxr успешен.
 * Использование: a64emu [-s песочница] [-i заголовки] [-a "аргументы"] prog.elf */
#define _GNU_SOURCE
#include "../../elf.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define USER_LO 0x808000000000ULL
#define USER_HI 0x1000000000000ULL
#define HEAP_BASE 0x808800000000ULL
#define HEAP_MAX_PAGES 8192
#define STACK_TOP 0x80ff00800000ULL
#define STACK_SIZE 0x800000ULL

typedef struct { uint64_t va, len; uint8_t *host; int r, w, x; } Reg;
static Reg regs[16];
static int nregs;
static unsigned long heap_pages;
static const char *sandbox_dir, *inc_dir, *prog_args = "";
static uint64_t X[32], SP, PC;
static int fN, fZ, fC, fV;
static uint64_t V[32][2];                                                 /* v0-v31: младшие и старшие 64 бит */
static unsigned long long icount;

static Reg *find(uint64_t va, uint64_t n) {
    static Reg *last;
    if (last && va >= last->va && va + n <= last->va + last->len && va + n >= va) return last;
    for (int i = 0; i < nregs; i++)
        if (va >= regs[i].va && va + n <= regs[i].va + regs[i].len && va + n >= va) return last = &regs[i];
    return NULL;
}
static void die(int code, const char *what, uint64_t a) {
    fflush(stdout);
    fprintf(stderr, "a64emu: %s at 0x%llx (pc 0x%llx, insn #%llu)\n", what, (unsigned long long)a, (unsigned long long)PC, icount);
    exit(code);
}
static uint8_t *tr(uint64_t va, uint64_t n, int w) {
    Reg *r = find(va, n);
    if (!r || !r->r) die(96, "memory fault (read)", va);
    if (w && !r->w) die(96, "memory fault (write to read-only)", va);
    return r->host + (va - r->va);
}
static uint64_t ld(uint64_t va, int n) { uint64_t v = 0; memcpy(&v, tr(va, (uint64_t)n, 0), (size_t)n); return v; }
static void st(uint64_t va, int n, uint64_t v) { memcpy(tr(va, (uint64_t)n, 1), &v, (size_t)n); }

/* ---------- системные вызовы Киры ---------- */
typedef struct { uint32_t op, name_len; uint64_t name_ptr, buf_ptr, buf_len, off; } fsreq_t;
static int name_ok(const char *n, uint32_t len) {
    if (len == 0 || len > 40) return 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = n[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}
static long fs_op(uint64_t reqva) {
    fsreq_t q;
    memcpy(&q, tr(reqva, sizeof q, 0), sizeof q);
    char path[256], name[48];
    if (q.name_len > 40 || !name_ok((const char *)tr(q.name_ptr, q.name_len ? q.name_len : 1, 0), q.name_len)) return -100;
    memcpy(name, tr(q.name_ptr, q.name_len, 0), q.name_len); name[q.name_len] = 0;
    int inc = q.op == 11 || q.op == 12;
    const char *dir = inc ? inc_dir : sandbox_dir;
    if (!dir || (q.op != 1 && q.op != 2 && q.op != 3 && !inc)) return -101;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (q.op == 2) {
        FILE *f = fopen(path, "wb");
        if (!f) return -6;
        size_t w = q.buf_len ? fwrite(tr(q.buf_ptr, q.buf_len, 0), 1, q.buf_len, f) : 0;
        fclose(f);
        return w == q.buf_len ? 0 : -6;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f), r = sz;
    if (q.op == 1 || q.op == 11) {
        if (q.buf_len == 0 || fseek(f, (long)q.off, SEEK_SET) != 0) r = -101;
        else r = (long)fread(tr(q.buf_ptr, q.buf_len, 1), 1, q.buf_len, f);
    }
    fclose(f);
    return r;
}
static void svc(void) {
    uint64_t n = X[8], a = X[0], b = X[1];
    long r = 0;
    switch (n) {
    case 1: if (write(1, tr(a, b ? b : 1, 0), b) < 0) r = -1; else r = (long)b; break;
    case 2: fflush(stdout); printf("%d\n", (int)a); fflush(stdout); exit(0);
    case 3: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); r = ts.tv_sec * 100 + ts.tv_nsec / 10000000; break; }
    case 4: break;
    case 5: break;
    case 6: r = 1; break;
    case 10: r = fs_op(a); break;
    case 30: { size_t k = strlen(prog_args); if (k + 1 > b) { r = -1; break; } memcpy(tr(a, k + 1, 1), prog_args, k + 1); r = (long)k; break; }
    case 11:
        if (a == 0 || heap_pages + a > HEAP_MAX_PAGES) { r = -1; break; }
        if (heap_pages == 0) {                    /* вся куча резервируется лениво одним куском */
            void *p = mmap(NULL, HEAP_MAX_PAGES * 4096ULL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED) { r = -1; break; }
            regs[nregs++] = (Reg){ HEAP_BASE, HEAP_MAX_PAGES * 4096ULL, p, 1, 1, 0 };
        }
        r = (long)(HEAP_BASE + heap_pages * 4096); heap_pages += a;
        break;
    default: fflush(stdout); fprintf(stderr, "a64emu: unsupported Kira syscall %llu\n", (unsigned long long)n); exit(98);
    }
    X[0] = (uint64_t)r;
}

/* ---------- ядро ---------- */
static uint64_t rx(int r) { return r == 31 ? 0 : X[r]; }                 /* xzr */
static uint64_t rs(int r) { return r == 31 ? SP : X[r]; }                /* sp */
static void wx(int r, uint64_t v) { if (r != 31) X[r] = v; }
static void ws(int r, uint64_t v) { if (r == 31) SP = v; else X[r] = v; }
static int cond(int c) {
    int r;
    switch (c >> 1) {
    case 0: r = fZ; break;
    case 1: r = fC; break;
    case 2: r = fN; break;
    case 3: r = fV; break;
    case 4: r = fC && !fZ; break;
    case 5: r = fN == fV; break;
    case 6: r = fN == fV && !fZ; break;
    default: r = 1;
    }
    return (c & 1) && c != 15 ? !r : r;
}
static void flags_sub(uint64_t a, uint64_t b, uint64_t res) {
    fN = res >> 63; fZ = res == 0; fC = a >= b; fV = (int)(((a ^ b) & (a ^ res)) >> 63);
}
static void flags_add(uint64_t a, uint64_t b, uint64_t res) {
    fN = res >> 63; fZ = res == 0; fC = res < a; fV = (int)((~(a ^ b) & (a ^ res)) >> 63);
}
static int64_t sx(uint32_t v, int bits) { return (int64_t)((uint64_t)v << (64 - bits)) >> (64 - bits); }


/* ---------- скалярный FP (хост-арифметика binary32/binary64, округление к ближайшему чётному) ---------- */
static double fd_get(int r, int dbl) {
    if (dbl) { double d; memcpy(&d, &V[r][0], 8); return d; }
    float f; uint32_t b = (uint32_t)V[r][0]; memcpy(&f, &b, 4); return (double)f;
}
static void fd_put(int r, int dbl, double v, float fv) {
    V[r][1] = 0;
    if (dbl) memcpy(&V[r][0], &v, 8);
    else { uint32_t b; memcpy(&b, &fv, 4); V[r][0] = b; }
}
/* fcvtzs/fcvtzu: усечение с насыщением, NaN -> 0 (как на железе) */
static uint64_t fcvt_int(double v, int sg) {
    if (v != v) return 0;
    if (sg) {
        if (v >= 9223372036854775808.0) return (uint64_t)INT64_MAX;
        if (v < -9223372036854775808.0) return (uint64_t)INT64_MIN;
        return (uint64_t)(int64_t)v;
    }
    if (v <= 0) return 0;
    if (v >= 18446744073709551616.0) return UINT64_MAX;
    return (uint64_t)v;
}
static int step_fp(uint32_t w) {
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31, dbl = (w >> 22) & 1;
    if ((w & 0xFFFFFC00) == 0x9E670000) { V[rd][0] = rx(rn); V[rd][1] = 0; }
    else if ((w & 0xFFFFFC00) == 0x1E270000) { V[rd][0] = (uint32_t)rx(rn); V[rd][1] = 0; }
    else if ((w & 0xFFFFFC00) == 0x9E660000) wx(rd, V[rn][0]);
    else if ((w & 0xFFFFFC00) == 0x1E260000) wx(rd, (uint32_t)V[rn][0]);
    else if ((w & 0xFF200C00) == 0x1E200800 && ((w >> 12) & 15) < 4) {
        int op = (w >> 12) & 15;
        if (dbl) {
            double a = fd_get(rn, 1), b = fd_get(rm, 1);
            fd_put(rd, 1, op == 0 ? a * b : op == 1 ? a / b : op == 2 ? a + b : a - b, 0);
        } else {
            float a = (float)fd_get(rn, 0), b = (float)fd_get(rm, 0);
            float r = op == 0 ? a * b : op == 1 ? a / b : op == 2 ? a + b : a - b;
            fd_put(rd, 0, 0, r);
        }
    }
    else if ((w & 0xFFBFFC00) == 0x1E214000) { if (dbl) fd_put(rd, 1, -fd_get(rn, 1), 0); else fd_put(rd, 0, 0, -(float)fd_get(rn, 0)); }
    else if ((w & 0xFFBFFC00) == 0x1E20C000) { if (dbl) fd_put(rd, 1, fabs(fd_get(rn, 1)), 0); else fd_put(rd, 0, 0, fabsf((float)fd_get(rn, 0))); }
    else if ((w & 0xFFBFFC00) == 0x1E21C000) { if (dbl) fd_put(rd, 1, sqrt(fd_get(rn, 1)), 0); else fd_put(rd, 0, 0, sqrtf((float)fd_get(rn, 0))); }
    else if ((w & 0xFFFFFC00) == 0x1E22C000) fd_put(rd, 1, fd_get(rn, 0), 0);                                        /* fcvt d, s */
    else if ((w & 0xFFFFFC00) == 0x1E624000) fd_put(rd, 0, 0, (float)fd_get(rn, 1));                                 /* fcvt s, d */
    else if ((w & 0xFF20FC1F) == 0x1E202000) {                                                                       /* fcmp */
        double a = fd_get(rn, dbl), b = fd_get(rm, dbl);
        if (a != a || b != b) { fN = 0; fZ = 0; fC = 1; fV = 1; }
        else if (a == b) { fN = 0; fZ = 1; fC = 1; fV = 0; }
        else if (a < b) { fN = 1; fZ = 0; fC = 0; fV = 0; }
        else { fN = 0; fZ = 0; fC = 1; fV = 0; }
    }
    else if ((w & 0xFFBFFC00) == 0x9E220000) { int64_t v = (int64_t)rx(rn); if (dbl) fd_put(rd, 1, (double)v, 0); else fd_put(rd, 0, 0, (float)v); }
    else if ((w & 0xFFBFFC00) == 0x9E230000) { uint64_t v = rx(rn); if (dbl) fd_put(rd, 1, (double)v, 0); else fd_put(rd, 0, 0, (float)v); }
    else if ((w & 0xFFBFFC00) == 0x9E380000) wx(rd, fcvt_int(fd_get(rn, dbl), 1));
    else if ((w & 0xFFBFFC00) == 0x9E390000) wx(rd, fcvt_int(fd_get(rn, dbl), 0));
    else if ((w & 0xFFC00000) == 0x3D800000) {                                                                       /* str q, [xn, #imm] */
        uint64_t addr = rs(rn) + ((uint64_t)((w >> 10) & 0xfff) << 4);
        st(addr, 8, V[rd][0]); st(addr + 8, 8, V[rd][1]);
    }
    else return 0;
    return 1;
}

static void step(void) {
    uint32_t w = (uint32_t)ld(PC, 4);
    if (!find(PC, 4)->x) die(96, "execute from non-executable memory", PC);
    uint64_t npc = PC + 4;
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    uint32_t hi = w >> 24;
    icount++;
    if ((w & 0xFF800000) == 0xD2800000 || (w & 0xFF800000) == 0x92800000 || (w & 0xFF800000) == 0xF2800000) {      /* movz/movn/movk x */
        int hw = (w >> 21) & 3; uint64_t imm = (uint64_t)((w >> 5) & 0xffff) << (16 * hw);
        if (hi == 0xD2) wx(rd, imm); else if (hi == 0x92) wx(rd, ~imm);
        else wx(rd, (X[rd] & ~(0xffffULL << (16 * hw))) | imm);
    } else if (hi == 0x91 || hi == 0xD1 || hi == 0xB1 || hi == 0xF1) {                                               /* add/sub imm */
        uint64_t imm = (uint64_t)((w >> 10) & 0xfff) << ((w >> 22) & 1 ? 12 : 0), a = rs(rn), res;
        int sub = hi == 0xD1 || hi == 0xF1, s = hi == 0xB1 || hi == 0xF1;
        res = sub ? a - imm : a + imm;
        if (s) { if (sub) flags_sub(a, imm, res); else flags_add(a, imm, res); wx(rd, res); }
        else ws(rd, res);
    } else if ((hi == 0x8B || hi == 0xCB || hi == 0xAB || hi == 0xEB) && ((w >> 21) & 7) == 0) {                      /* add/sub reg */
        uint64_t a = rx(rn), b = rx(rm), res;
        int sub = hi == 0xCB || hi == 0xEB, s = hi == 0xAB || hi == 0xEB;
        if ((w >> 10) & 0x3f) die(97, "shifted register operand not emitted by kcc", PC);
        res = sub ? a - b : a + b;
        if (s) { if (sub) flags_sub(a, b, res); else flags_add(a, b, res); }
        wx(rd, res);
    } else if (hi == 0x8B && ((w >> 21) & 7) == 1 && ((w >> 13) & 7) == 3 && ((w >> 10) & 7) == 0) {                  /* add ext uxtx */
        ws(rd, rs(rn) + rx(rm));
    } else if (hi == 0x8A || hi == 0xAA || hi == 0xCA || hi == 0xEA) {                                               /* logic x */
        if ((w >> 10) & 0x3f) die(97, "shifted register operand not emitted by kcc", PC);
        uint64_t a = rx(rn), b = rx(rm), res;
        if ((w >> 21) & 1) b = ~b;
        switch (hi) { case 0x8A: res = a & b; break; case 0xAA: res = a | b; break; case 0xCA: res = a ^ b; break; default: res = a & b; }
        if (hi == 0xEA) { fN = res >> 63; fZ = res == 0; fC = 0; fV = 0; }
        wx(rd, res);
    } else if (hi == 0x2A && ((w >> 21) & 1) == 0 && rn == 31 && ((w >> 10) & 0x3f) == 0) {                          /* mov w, w */
        wx(rd, (uint32_t)rx(rm));
    } else if ((w & 0xFFE0FC00) == 0x9B007C00) wx(rd, rx(rn) * rx(rm));                                              /* mul */
    else if ((w & 0xFFE08000) == 0x9B008000) wx(rd, rx((w >> 10) & 31) - rx(rn) * rx(rm));                           /* msub */
    else if ((w & 0xFFE0FC00) == 0x9AC00800) { uint64_t b = rx(rm); wx(rd, b ? rx(rn) / b : 0); }                    /* udiv */
    else if ((w & 0xFFE0FC00) == 0x9AC00C00) {                                                                       /* sdiv */
        int64_t a = (int64_t)rx(rn), b = (int64_t)rx(rm);
        wx(rd, b == 0 ? 0 : (a == INT64_MIN && b == -1) ? (uint64_t)a : (uint64_t)(a / b));
    }
    else if ((w & 0xFFE0FC00) == 0x9AC02000) wx(rd, rx(rn) << (rx(rm) & 63));                                         /* lslv */
    else if ((w & 0xFFE0FC00) == 0x9AC02400) wx(rd, rx(rn) >> (rx(rm) & 63));                                         /* lsrv */
    else if ((w & 0xFFE0FC00) == 0x9AC02800) wx(rd, (uint64_t)((int64_t)rx(rn) >> (rx(rm) & 63)));                    /* asrv */
    else if ((w & 0xFFC00000) == 0xD3400000 || (w & 0xFFC00000) == 0x93400000) {                                      /* ubfm/sbfm x */
        int immr = (w >> 16) & 63, imms = (w >> 10) & 63, sg = hi == 0x93;
        uint64_t src = rx(rn), res;
        if (imms >= immr) {
            int len = imms - immr + 1;
            res = src >> immr;
            if (len < 64) { res &= (1ULL << len) - 1; if (sg && (res >> (len - 1)) & 1) res |= ~0ULL << len; }
        } else {
            int len = imms + 1, sh = 64 - immr;
            if (sg) die(97, "sbfm lsl-form not emitted by kcc", PC);
            res = (src & (len < 64 ? (1ULL << len) - 1 : ~0ULL)) << sh;
        }
        wx(rd, res);
    } else if ((w & 0xFFC00000) == 0x53000000) {                                                                     /* ubfm w (uxtb/uxth) */
        int immr = (w >> 16) & 31, imms = (w >> 10) & 31;
        if (immr != 0) die(97, "32-bit ubfm form not emitted by kcc", PC);
        wx(rd, (uint32_t)rx(rn) & (uint32_t)((2ULL << imms) - 1));
    } else if ((w & 0xFFE00C00) == 0x1A800400) {                                                                      /* csinc w (cset) */
        int c = (w >> 12) & 15;
        wx(rd, cond(c) ? (uint32_t)rx(rn) : (uint32_t)rx(rm) + 1);
    } else if (!(w & 0x04000000) && ((w & 0x3B000000) == 0x39000000 || ((w & 0x3B200C00) == 0x38000000))) {                                 /* ldr/str imm, ldur/stur */
        int sz = w >> 30, opc = (w >> 22) & 3, n = 1 << sz;
        uint64_t addr;
        if ((w & 0x3B000000) == 0x39000000) addr = rs(rn) + ((uint64_t)((w >> 10) & 0xfff) << sz);
        else addr = rs(rn) + (uint64_t)sx((w >> 12) & 0x1ff, 9);
        if (opc == 0) st(addr, n, rx(rd));
        else {
            uint64_t v = ld(addr, n);
            if (opc == 2) v = (uint64_t)(n == 1 ? (int64_t)(int8_t)v : n == 2 ? (int64_t)(int16_t)v : (int64_t)(int32_t)v);
            else if (opc != 1) die(97, "load form not emitted by kcc", PC);
            wx(rd, v);
        }
    } else if ((w & 0x3FFFFC00) == 0x08DFFC00 || (w & 0x3FFFFC00) == 0x085FFC00) {                                  /* ldar, ldaxr */
        int n = 1 << (w >> 30);
        wx(rd, ld(rs(rn), n));
    } else if ((w & 0x3FFFFC00) == 0x089FFC00) st(rs(rn), 1 << (w >> 30), rx(rd));                                    /* stlr */
    else if ((w & 0x3FE0FC00) == 0x0800FC00) { st(rs(rn), 1 << (w >> 30), rx(rd)); wx(rm, 0); }                       /* stlxr: успех */
    else if ((w & 0xFC000000) == 0x14000000) npc = PC + (uint64_t)(sx(w & 0x3ffffff, 26) * 4);                         /* b */
    else if ((w & 0xFC000000) == 0x94000000) { X[30] = PC + 4; npc = PC + (uint64_t)(sx(w & 0x3ffffff, 26) * 4); }     /* bl */
    else if ((w & 0xFF000010) == 0x54000000) { if (cond(w & 15)) npc = PC + (uint64_t)(sx((w >> 5) & 0x7ffff, 19) * 4); }   /* b.cond */
    else if ((w & 0x7E000000) == 0x34000000) {                                                                        /* cbz/cbnz */
        uint64_t v = (w >> 31) ? rx(rd) : (uint32_t)rx(rd);
        if ((v == 0) != (int)((w >> 24) & 1)) npc = PC + (uint64_t)(sx((w >> 5) & 0x7ffff, 19) * 4);
    }
    else if ((w & 0xFFFFFC1F) == 0xD63F0000) { uint64_t t = rx(rn); X[30] = PC + 4; npc = t; }                         /* blr */
    else if (w == 0xD65F03C0) npc = X[30];                                                                            /* ret */
    else if ((w & 0x9F000000) == 0x90000000) {                                                                        /* adrp */
        int64_t imm = sx(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
        wx(rd, (PC & ~0xfffULL) + (uint64_t)(imm << 12));
    }
    else if (w == 0xD4000001) svc();
    else if ((w & 0xFFE0001F) == 0xD4200000) die(95, "brk (unreachable code executed)", PC);
    else if (w == 0xD503201F || w == 0xD503203F || w == 0xD503205F || w == 0xD503207F || w == 0xD503209F) {}         /* nop, yield, wfe, wfi, sev */
    else if ((w & 0xFFFFF0FF) == 0xD50330BF || (w & 0xFFFFF0FF) == 0xD503309F || w == 0xD5033FDF || w == 0xD5033F5F) {}   /* dmb, dsb, isb, clrex */
    else if ((w & 0xFFFFFFE0) == 0xD53BE040) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); wx(rd, (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec); }
    else if ((w & 0xFFFFFFE0) == 0xD53BE000) wx(rd, 1000000000ULL);
    else if (!step_fp(w)) die(97, "instruction not in the subset emitted by kcc", PC);
    PC = npc;
}

int main(int argc, char **argv) {
    while (argc > 3 && argv[1][0] == '-' && argv[1][1] && argv[1][2] == 0) {
        if (argv[1][1] == 's') sandbox_dir = argv[2];
        else if (argv[1][1] == 'i') inc_dir = argv[2];
        else if (argv[1][1] == 'a') prog_args = argv[2];
        else break;
        argv += 2; argc -= 2;
    }
    if (argc < 2) { fprintf(stderr, "usage: a64emu [-s dir] [-i dir] [-a args] prog.elf\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 28)) { fprintf(stderr, "a64emu: bad file size\n"); return 2; }
    unsigned char *img = malloc((size_t)len);
    if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "a64emu: read error\n"); return 2; }
    fclose(f);
    elf_info_t info;
    int e = elf_parse(img, (size_t)len, USER_LO, USER_HI, 4096, &info);
    if (e != ELF_OK) { fprintf(stderr, "a64emu: elf_parse: %s\n", elf_strerror(e)); return 3; }
    for (unsigned i = 0; i < info.nsegs; i++) {
        const elf_seg_t *s = &info.seg[i];
        uint64_t mlen = (s->mem_size + 4095) & ~4095ULL;
        uint8_t *h = calloc(1, (size_t)mlen);
        memcpy(h, img + s->file_off, (size_t)s->file_size);
        regs[nregs++] = (Reg){ s->vaddr, mlen, h, 1, s->write, s->exec };
    }
    regs[nregs++] = (Reg){ STACK_TOP - STACK_SIZE, STACK_SIZE, calloc(1, STACK_SIZE), 1, 1, 0 };
    SP = STACK_TOP - 64; PC = info.entry;
    unsigned long long limit = 4000000000ULL;
    while (icount < limit) step();
    fprintf(stderr, "a64emu: instruction limit reached\n");
    return 4;
}
