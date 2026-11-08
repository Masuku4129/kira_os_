/* Интерпретатор текстового IR kcc (kcc -S-ir): выполняет main() и печатает результат.
 * Нужен для проверки ПРАВИЛЬНОСТИ lowering: результат программы = результат той же программы, собранной clang. */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { O_LABEL, O_CONST, O_GADDR, O_LADDR, O_PARAM, O_LOAD, O_STORE, O_MOV, O_BIN, O_NEG, O_NOT, O_CMP, O_EXT, O_JMP, O_BRZ, O_BRNZ,
       O_CALL, O_RET, O_COPY, O_ZERO, O_NOP, O_UNREACH, O_FBIN, O_FUN, O_FCMP, O_I2F, O_F2I, O_F2D, O_D2F, O_ALLOCA, O_SPSAVE, O_SPREST };
typedef struct { int op, sub, dst, a, b, w, sgn, nargs; int64_t imm; char *sym; int args[32]; } In;
typedef struct { char name[96]; int nparams, nslots, nregs; int slotsz[256], slotal[256]; In *ins; int nin; int *lab; int nlab; } Fn;
typedef struct { char name[96]; int64_t addr; int size; } Gv;

#define MEMSZ (1 << 24)
#define BASE 0x10000
static unsigned char *mem;
static int64_t heap_top = BASE + 64, stack_top = BASE + MEMSZ - 8;
static Fn fns[256]; static int nf;
static Gv gvs[2048]; static int ng;

static void die(const char *m, const char *a) { fprintf(stderr, "irrun: %s %s\n", m, a ? a : ""); exit(3); }
static unsigned char *at(int64_t a, int n) {
    if (a < BASE || a + n > BASE + MEMSZ) { fprintf(stderr, "irrun: bad address 0x%llx\n", (long long)a); exit(4); }
    return mem + (a - BASE);
}
static int64_t rd(int64_t a, int w, int sg) {
    uint64_t v = 0;
    unsigned char *p = at(a, w);
    for (int i = w - 1; i >= 0; i--) v = v << 8 | p[i];
    if (sg && w < 8) { int sh = 64 - 8 * w; return (int64_t)(v << sh) >> sh; }
    return (int64_t)v;
}
static void wr(int64_t a, int w, int64_t v) {
    unsigned char *p = at(a, w);
    for (int i = 0; i < w; i++) p[i] = (unsigned char)((uint64_t)v >> (8 * i));
}
static int64_t norm(int64_t v, int w, int sg) {
    if (w == 4) return sg ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
    if (w == 2) return sg ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
    if (w == 1) return sg ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
    return v;
}

static Gv *find_gv(const char *n) { for (int i = 0; i < ng; i++) if (!strcmp(gvs[i].name, n)) return &gvs[i]; return NULL; }
static int find_fn(const char *n) { for (int i = 0; i < nf; i++) if (!strcmp(fns[i].name, n)) return i; return -1; }

static int tokenize(char *line, char **tk) {
    int n = 0;
    for (char *p = line; *p; p++) if (*p == ',' || *p == '[' || *p == ']' || *p == '(' || *p == ')' || *p == '=' || *p == '\n') *p = ' ';
    for (char *t = strtok(line, " \t"); t && n < 40; t = strtok(NULL, " \t")) tk[n++] = t;
    return n;
}
static int reg(const char *t) { return atoi(t + 1); }

typedef struct { const char *name; int sub; } OpN;
static const OpN binops[] = { {"add", 0}, {"sub", 1}, {"mul", 2}, {"div", 3}, {"mod", 4}, {"and", 5}, {"or", 6}, {"xor", 7}, {"shl", 8}, {"shr", 9}, {0, 0} };
static const OpN cmpops[] = { {"eq", 0}, {"ne", 1}, {"lt", 2}, {"le", 3}, {0, 0} };

static int boff;

static void load_ir(FILE *f) {
    char line[4096], *tk[48];
    Fn *cf = NULL; In *buf = NULL; int cap = 0;
    Gv *cg = NULL;
    unsigned char *gdata = NULL;
    typedef struct { int64_t addr; int off; char sym[96]; int64_t add; } Rl;
    static Rl rls[8192]; static int nrl;
    while (fgets(line, sizeof line, f)) {
        char raw[4096]; strcpy(raw, line);
        int n = tokenize(line, tk);
        if (!n || tk[0][0] == ';') continue;
        if (!strcmp(tk[0], "func")) {
            cf = &fns[nf++]; memset(cf, 0, sizeof *cf);
            strcpy(cf->name, tk[1]);
            for (int i = 2; i < n; i++) if (!strncmp(tk[i], "params", 6)) cf->nparams = atoi(tk[i + 1]);
            cap = 256; buf = malloc(sizeof(In) * (size_t)cap); cf->ins = buf; cf->nin = 0;
            continue;
        }
        if (!strcmp(tk[0], "end")) { cf = NULL; continue; }
        if (!strcmp(tk[0], "data")) {
            cg = &gvs[ng++]; strcpy(cg->name, tk[1]);
            for (int i = 2; i < n; i++) if (!strncmp(tk[i], "size", 4)) cg->size = atoi(tk[i + 1]);
            int al = 1;
            for (int i = 2; i < n; i++) if (!strncmp(tk[i], "align", 5)) al = atoi(tk[i + 1]);
            heap_top = (heap_top + al - 1) / al * al;
            cg->addr = heap_top; heap_top += cg->size ? cg->size : 1;
            gdata = at(cg->addr, cg->size ? cg->size : 1);
            boff = 0;
            continue;
        }
        if (!strcmp(tk[0], "extern")) continue;
        if (!cf && cg) {
            if (!strcmp(tk[0], "bytes")) {
                for (int i = 1; i < n; i++) gdata[boff++] = (unsigned char)strtol(tk[i], NULL, 16);
            } else if (!strcmp(tk[0], "reloc")) {
                Rl *r = &rls[nrl++];
                r->addr = cg->addr; r->off = atoi(tk[1] + 1);
                char *s = tk[3]; char *pm = s + strlen(s) - 1;
                while (pm > s && *pm != '+' && *pm != '-') pm--;
                r->add = atoll(pm); memcpy(r->sym, s, (size_t)(pm - s)); r->sym[pm - s] = 0;
            }
            continue;
        }
        if (!cf) continue;
        if (!strncmp(tk[0], "slot", 4)) {
            int s = atoi(tk[0] + 4);
            for (int i = 1; i < n; i++) {
                if (!strncmp(tk[i], "size", 4)) cf->slotsz[s] = atoi(tk[i + 1]);
                if (!strncmp(tk[i], "align", 5)) cf->slotal[s] = atoi(tk[i + 1]);
            }
            if (s + 1 > cf->nslots) cf->nslots = s + 1;
            continue;
        }
        if (cf->nin + 1 >= cap) { cap *= 2; buf = realloc(buf, sizeof(In) * (size_t)cap); cf->ins = buf; }
        In *in = &cf->ins[cf->nin++];
        memset(in, 0, sizeof *in);
        if (tk[0][0] == '.') { in->op = O_LABEL; in->imm = atoi(tk[0] + 2); continue; }
        int i = 0;
        if (tk[0][0] == '%') { in->dst = reg(tk[0]); i = 1; if (in->dst > cf->nregs) cf->nregs = in->dst; }
        const char *o = tk[i];
        char base[16]; int w = 0, sg = 0;
        strncpy(base, o, 15); base[15] = 0;
        char *dot = strchr(base, '.'); char suff[16] = "";
        if (dot) { strcpy(suff, dot + 1); *dot = 0; }
        if (!strcmp(base, "const")) { in->op = O_CONST; in->imm = strtoll(tk[i + 1], NULL, 10); }
        else if (!strcmp(base, "gaddr")) { in->op = O_GADDR; in->sym = strdup(tk[i + 1]); }
        else if (!strcmp(base, "laddr")) { in->op = O_LADDR; in->imm = atoi(tk[i + 1] + 4); }
        else if (!strcmp(base, "param")) { in->op = O_PARAM; in->imm = atoi(tk[i + 1]); }
        else if (!strcmp(base, "alloca")) { in->op = O_ALLOCA; in->a = reg(tk[i + 1]); }
        else if (!strcmp(base, "spsave")) in->op = O_SPSAVE;
        else if (!strcmp(base, "sprest")) { in->op = O_SPREST; in->a = reg(tk[i + 1]); }
        else if (!strcmp(base, "mov")) { in->op = O_MOV; in->a = reg(tk[i + 1]); }
        else if (!strcmp(base, "load") || !strcmp(base, "store")) {
            char *s = suff; if (s[0] == 'v' && s[1] == '.') s += 2;
            w = atoi(s); sg = s[strlen(s) - 1] == 's';
            if (!strcmp(base, "load")) { in->op = O_LOAD; in->a = reg(tk[i + 1]); in->w = w; in->sgn = sg; }
            else { in->op = O_STORE; in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]); in->w = w; }
        }
        else if (!strcmp(base, "neg") || !strcmp(base, "not") || !strcmp(base, "ext")) {
            in->op = !strcmp(base, "neg") ? O_NEG : !strcmp(base, "not") ? O_NOT : O_EXT;
            in->w = atoi(suff); in->sgn = suff[strlen(suff) - 1] == 's'; in->a = reg(tk[i + 1]);
        }
        else if (!strcmp(base, "cmp")) {
            in->op = O_CMP; in->sgn = suff[strlen(suff) - 1] == 's';
            suff[strlen(suff) - 1] = 0;
            for (int k = 0; cmpops[k].name; k++) if (!strcmp(cmpops[k].name, suff)) in->sub = cmpops[k].sub;
            in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]);
        }
        else if (!strcmp(base, "fadd") || !strcmp(base, "fsub") || !strcmp(base, "fmul") || !strcmp(base, "fdiv")) {
            in->op = O_FBIN; in->sub = base[1] == 'a' ? 0 : base[1] == 's' ? 1 : base[2] == 'u' ? 2 : 3;
            in->w = atoi(suff); in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]);
        }
        else if (!strcmp(base, "fneg") || !strcmp(base, "fsqrt") || !strcmp(base, "fabs")) {
            in->op = O_FUN; in->sub = base[1] == 'n' ? 0 : base[1] == 's' ? 1 : 2; in->w = atoi(suff); in->a = reg(tk[i + 1]);
        }
        else if (!strcmp(base, "fcmp")) {
            in->op = O_FCMP;
            char *d2 = strchr(suff, '.');
            *d2 = 0; in->w = atoi(d2 + 1);
            for (int k = 0; cmpops[k].name; k++) if (!strcmp(cmpops[k].name, suff)) in->sub = cmpops[k].sub;
            in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]);
        }
        else if (!strcmp(base, "i2f") || !strcmp(base, "f2i")) {
            in->op = base[0] == 'i' ? O_I2F : O_F2I; in->w = atoi(suff); in->sgn = suff[strlen(suff) - 1] == 's'; in->a = reg(tk[i + 1]);
        }
        else if (!strcmp(base, "f2d") || !strcmp(base, "d2f")) { in->op = base[0] == 'f' ? O_F2D : O_D2F; in->a = reg(tk[i + 1]); }
        else if (!strcmp(base, "jmp")) { in->op = O_JMP; in->imm = atoi(tk[i + 1] + 2); }
        else if (!strcmp(base, "brz") || !strcmp(base, "brnz")) { in->op = base[2] == 'z' ? O_BRZ : O_BRNZ; in->a = reg(tk[i + 1]); in->imm = atoi(tk[i + 2] + 2); }
        else if (!strcmp(base, "call")) {
            in->op = O_CALL;
            int k = i + 1;
            if (tk[k][0] == '*') in->a = reg(tk[k] + 1); else in->sym = strdup(tk[k]);
            for (k++; k < n && tk[k][0] == '%'; k++) { if (in->nargs >= 32) die("too many call arguments", raw); in->args[in->nargs++] = reg(tk[k]); }
        }
        else if (!strcmp(base, "ret")) { in->op = O_RET; in->a = n > i + 1 ? reg(tk[i + 1]) : -1; }
        else if (!strcmp(base, "copy")) { in->op = O_COPY; in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]); in->imm = atoi(tk[i + 3]); }
        else if (!strcmp(base, "zero")) { in->op = O_ZERO; in->a = reg(tk[i + 1]); in->imm = atoi(tk[i + 2]); }
        else if (!strcmp(base, "unreachable")) in->op = O_UNREACH;
        else if (!strcmp(base, "asm")) in->op = O_NOP;
        else {
            int found = 0;
            for (int k = 0; binops[k].name; k++) if (!strcmp(binops[k].name, base)) { in->op = O_BIN; in->sub = binops[k].sub; found = 1; }
            if (!found) die("unknown instruction", raw);
            in->w = atoi(suff); in->sgn = suff[strlen(suff) - 1] == 's'; in->a = reg(tk[i + 1]); in->b = reg(tk[i + 2]);
        }
    }
    for (int i = 0; i < nrl; i++) {
        Gv *g = find_gv(rls[i].sym);
        int64_t v;
        if (g) v = g->addr; else { int fi = find_fn(rls[i].sym); if (fi < 0) die("unresolved symbol", rls[i].sym); v = 0xF0000000LL + fi * 16; }
        wr(rls[i].addr + rls[i].off, 8, v + rls[i].add);
    }
}
static double bd(int64_t v) { double d; memcpy(&d, &v, 8); return d; }
static float bf(int64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }
static int64_t pd(double d) { int64_t v; memcpy(&v, &d, 8); return v; }
static int64_t pf(float f) { uint32_t u; memcpy(&u, &f, 4); return (int64_t)u; }
static int64_t run(int fi, int64_t *args, int nargs, int depth);

static int64_t builtin(const char *n, int64_t *a, int na, int *ok) {
    *ok = 1;
    if (!strcmp(n, "memcpy") && na == 3) { memmove(at(a[0], (int)a[2]), at(a[1], (int)a[2]), (size_t)a[2]); return a[0]; }
    if (!strcmp(n, "memset") && na == 3) { memset(at(a[0], (int)a[2]), (int)a[1], (size_t)a[2]); return a[0]; }
    if (!strcmp(n, "memcmp") && na == 3) { int r = memcmp(at(a[0], (int)a[2]), at(a[1], (int)a[2]), (size_t)a[2]); return r < 0 ? -1 : r > 0; }
    if (!strcmp(n, "strlen") && na == 1) { int64_t k = 0; while (*at(a[0] + k, 1)) k++; return k; }
    *ok = 0;
    return 0;
}

static int64_t run(int fi, int64_t *args, int nargs, int depth) {
    Fn *f = &fns[fi];
    if (depth > 400) die("recursion too deep", f->name);
    int64_t *r = calloc((size_t)f->nregs + 2, sizeof *r);
    int64_t sp = stack_top, save = stack_top;
    int64_t slot[256];
    for (int s = 0; s < f->nslots; s++) {
        int al = f->slotal[s] ? f->slotal[s] : 1, sz = f->slotsz[s] ? f->slotsz[s] : 1;
        sp = (sp - sz) / al * al;
        slot[s] = sp;
        memset(at(sp, sz), 0xA5, (size_t)sz);       /* неинициализированные локальные - мусор */
    }
    stack_top = sp - 16;
    int64_t ret = 0;
    int pc = 0;
    (void)nargs;
    for (;;) {
        if (pc >= f->nin) break;
        In *in = &f->ins[pc++];
        switch (in->op) {
        case O_LABEL: case O_NOP: break;
        case O_CONST: r[in->dst] = in->imm; break;
        case O_GADDR: {
            Gv *g = find_gv(in->sym);
            if (g) r[in->dst] = g->addr;
            else { int k = find_fn(in->sym); r[in->dst] = k >= 0 ? 0xF0000000LL + k * 16 : 0xF0000000LL + 4096 + (int64_t)(strlen(in->sym)); }
            break;
        }
        case O_LADDR: r[in->dst] = slot[in->imm]; break;
        case O_PARAM: r[in->dst] = args[in->imm]; break;
        case O_ALLOCA: {
            int64_t sz = (r[in->a] + 15) & ~(int64_t)15;
            if (sz < 0 || sz > MEMSZ / 2) die("alloca: too large a block in", f->name);
            stack_top = (stack_top - sz) & ~(int64_t)15;
            memset(at(stack_top, (int)(sz ? sz : 1)), 0xA5, (size_t)(sz ? sz : 1));
            r[in->dst] = stack_top;
            stack_top -= 16;
            break;
        }
        case O_SPSAVE: r[in->dst] = stack_top; break;
        case O_SPREST: stack_top = r[in->a]; break;
        case O_LOAD: r[in->dst] = rd(r[in->a], in->w, in->sgn); break;
        case O_STORE: wr(r[in->a], in->w, r[in->b]); break;
        case O_MOV: r[in->dst] = r[in->a]; break;
        case O_BIN: {
            int64_t a = r[in->a], b = r[in->b], v = 0;
            uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
            switch (in->sub) {
            case 0: v = (int64_t)(ua + ub); break;
            case 1: v = (int64_t)(ua - ub); break;
            case 2: v = (int64_t)(ua * ub); break;
            case 3: if (!b) die("division by zero", f->name); v = in->sgn ? (b == -1 ? (int64_t)(0 - ua) : a / b) : (int64_t)(ua / ub); break;
            case 4: if (!b) die("modulo by zero", f->name); v = in->sgn ? (b == -1 ? 0 : a % b) : (int64_t)(ua % ub); break;
            case 5: v = a & b; break;
            case 6: v = a | b; break;
            case 7: v = a ^ b; break;
            case 8: v = (int64_t)(ua << (ub & 63)); break;
            case 9: v = in->sgn ? a >> (ub & 63) : (int64_t)(ua >> (ub & 63)); break;
            }
            r[in->dst] = norm(v, in->w, in->sgn);
            break;
        }
        case O_NEG: r[in->dst] = norm((int64_t)(0 - (uint64_t)r[in->a]), in->w, in->sgn); break;
        case O_NOT: r[in->dst] = norm(~r[in->a], in->w, in->sgn); break;
        case O_EXT: r[in->dst] = norm(r[in->a], in->w, in->sgn); break;
        case O_CMP: {
            int64_t a = r[in->a], b = r[in->b]; int v = 0;
            switch (in->sub) {
            case 0: v = a == b; break;
            case 1: v = a != b; break;
            case 2: v = in->sgn ? a < b : (uint64_t)a < (uint64_t)b; break;
            case 3: v = in->sgn ? a <= b : (uint64_t)a <= (uint64_t)b; break;
            }
            r[in->dst] = v;
            break;
        }
        case O_FBIN:
            if (in->w == 8) {
                double x = bd(r[in->a]), y = bd(r[in->b]);
                r[in->dst] = pd(in->sub == 0 ? x + y : in->sub == 1 ? x - y : in->sub == 2 ? x * y : x / y);
            } else {
                float x = bf(r[in->a]), y = bf(r[in->b]);
                r[in->dst] = pf(in->sub == 0 ? x + y : in->sub == 1 ? x - y : in->sub == 2 ? x * y : x / y);
            }
            break;
        case O_FUN:
            if (in->w == 8) { double x = bd(r[in->a]); r[in->dst] = pd(in->sub == 0 ? -x : in->sub == 1 ? sqrt(x) : fabs(x)); }
            else { float x = bf(r[in->a]); r[in->dst] = pf(in->sub == 0 ? -x : in->sub == 1 ? sqrtf(x) : fabsf(x)); }
            break;
        case O_FCMP: {
            int v;
            if (in->w == 8) { double x = bd(r[in->a]), y = bd(r[in->b]); v = in->sub == 0 ? x == y : in->sub == 1 ? x != y : in->sub == 2 ? x < y : x <= y; }
            else { float x = bf(r[in->a]), y = bf(r[in->b]); v = in->sub == 0 ? x == y : in->sub == 1 ? x != y : in->sub == 2 ? x < y : x <= y; }
            r[in->dst] = v;
            break;
        }
        case O_I2F:
            if (in->w == 8) r[in->dst] = pd(in->sgn ? (double)r[in->a] : (double)(uint64_t)r[in->a]);
            else r[in->dst] = pf(in->sgn ? (float)r[in->a] : (float)(uint64_t)r[in->a]);
            break;
        case O_F2I: {
            double x = in->w == 8 ? bd(r[in->a]) : (double)bf(r[in->a]);
            r[in->dst] = in->sgn ? (int64_t)x : (int64_t)(uint64_t)x;
            break;
        }
        case O_F2D: r[in->dst] = pd((double)bf(r[in->a])); break;
        case O_D2F: r[in->dst] = pf((float)bd(r[in->a])); break;
        case O_JMP: case O_BRZ: case O_BRNZ: {
            if (in->op == O_BRZ && r[in->a] != 0) break;
            if (in->op == O_BRNZ && r[in->a] == 0) break;
            int t = -1;
            for (int k = 0; k < f->nin; k++) if (f->ins[k].op == O_LABEL && f->ins[k].imm == in->imm) { t = k; break; }
            if (t < 0) die("label not found in", f->name);
            pc = t;
            break;
        }
        case O_CALL: {
            int64_t av[32];
            for (int k = 0; k < in->nargs; k++) av[k] = r[in->args[k]];
            int ti;
            const char *nm = in->sym;
            if (!nm) {
                int64_t a = r[in->a];
                if (a < 0xF0000000LL || a >= 0xF0000000LL + nf * 16) die("indirect call to a bad address in", f->name);
                ti = (int)((a - 0xF0000000LL) / 16);
            } else ti = find_fn(nm);
            int64_t v;
            if (ti < 0) { int ok; v = builtin(nm, av, in->nargs, &ok); if (!ok) die("unresolved call", nm); }
            else v = run(ti, av, in->nargs, depth + 1);
            if (in->dst) r[in->dst] = v;
            break;
        }
        case O_RET: ret = in->a > 0 ? r[in->a] : 0; goto done;
        case O_COPY: memmove(at(r[in->a], (int)in->imm), at(r[in->b], (int)in->imm), (size_t)in->imm); break;
        case O_ZERO: memset(at(r[in->a], (int)in->imm), 0, (size_t)in->imm); break;
        case O_UNREACH: die("reached unreachable in", f->name);
        }
    }
done:
    stack_top = save;
    free(r);
    return ret;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f) return 2;
    mem = calloc(1, MEMSZ);
    load_ir(f);
    fclose(f);
    int m = find_fn("main");
    if (m < 0) die("no main", NULL);
    int64_t v = run(m, NULL, 0, 0);
    printf("%d\n", (int)v);
    return 0;
}
