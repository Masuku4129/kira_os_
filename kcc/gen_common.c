/* kcc: общая часть бэкендов: таблица имён, снижение функций до IR, достижимость от main и внешних символов.
 * Неиспользуемые static-функции и данные в образ не попадают (как --gc-sections). Неопределённый символ - ошибка. */
#include "target.h"
#include <string.h>

int kcc_obj;                                    /* 1 - вывод объектного файла (ET_REL), main не нужен, внешние символы разрешены */
int kcc_noopt;                                  /* 1 - не запускать ir_optimize (-O0, для сверки и отладки) */

static Obj **htab;
static unsigned hcap;
static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
int gen_is_defined(const Obj *o) { return o->is_func ? o->body != NULL : o->is_def; }
int gen_is_helper_name(const char *n) { return !strcmp(n, "memcpy") || !strcmp(n, "memset") || !strcmp(n, "memcmp") || !strcmp(n, "strlen") ||
                                                      !strcmp(n, "setjmp") || !strcmp(n, "longjmp"); }

static void hput(Obj *o) {
    for (unsigned i = hash(o->name) & (hcap - 1);; i = (i + 1) & (hcap - 1)) {
        if (!htab[i]) { htab[i] = o; return; }
        if (!strcmp(htab[i]->name, o->name)) {
            if (gen_is_defined(o) && !gen_is_defined(htab[i])) htab[i] = o;
            return;
        }
    }
}
Obj *gen_lookup(const char *name) {
    for (unsigned i = hash(name) & (hcap - 1); htab[i]; i = (i + 1) & (hcap - 1))
        if (!strcmp(htab[i]->name, name)) return htab[i];
    return NULL;
}

static Obj **work; static int nwork;

static void mark_ref(const Obj *from, const char *name) {
    Obj *o = gen_lookup(name);
    if (!o || (!kcc_obj && !gen_is_defined(o) && !(o->is_func && gen_is_helper_name(o->name))))
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

/* Таблица имён, IR всех функций, достижимость. Возвращает main (его отсутствие - ошибка). */
Obj *gen_prepare(Obj *globals) {
    int ng = 0;
    for (Obj *g = globals; g; g = g->next) ng++;
    hcap = 16;
    while (hcap < (unsigned)ng * 2 + 16) hcap <<= 1;
    htab = ka_new(sizeof(Obj *) * hcap);
    for (Obj *g = globals; g; g = g->next) hput(g);
    for (Obj *g = globals; g; g = g->next)
        if (g->is_func && g->body) g->ir = ir_lower(g);
    if (!kcc_noopt) {
        for (Obj *g = globals; g; g = g->next) if (g->is_func && g->body) ir_inline(g);
        for (Obj *g = globals; g; g = g->next) if (g->is_func && g->body) ir_optimize((IrFn *)g->ir);
    }
    work = ka_new(sizeof(Obj *) * (size_t)(ng + 1));
    nwork = 0;
    Obj *mainf = gen_lookup("main");
    if (kcc_obj) mainf = NULL;
    else if (!mainf || !mainf->is_func || !mainf->body) kerr(K.srcpath, 1, "undefined reference to 'main'");
    if (!kcc_obj) mark_ref(NULL, "main");
    for (Obj *g = globals; g; g = g->next)
        if (!g->is_static && gen_is_defined(g) && !g->emit && gen_lookup(g->name) == g) mark_ref(g, g->name);
    while (nwork) scan_refs(work[--nwork]);
    return mainf;
}

static void touch(int *vs, int *ve, int v, int idx, int nv) {
    if (v <= 0) return;
    if (v > nv) kerr(K.srcpath, 0, "internal: bad vreg %d", v);
    if (idx < vs[v]) vs[v] = idx;
    if (idx > ve[v]) ve[v] = idx;
}
/* Продлевает живой интервал vreg, которые читает или пишет инструкция idx. */
void gen_live(const Ins *i, int idx, int nv, int *vs, int *ve) {
    switch (i->op) {
    case IR_CONST: case IR_GADDR: case IR_LADDR: case IR_VAINFO: touch(vs, ve, i->dst, idx, nv); break;
    case IR_PARAM: touch(vs, ve, i->dst, idx, nv); touch(vs, ve, i->a, idx, nv); break;
    case IR_ATOMIC: touch(vs, ve, i->a, idx, nv); touch(vs, ve, i->b, idx, nv); touch(vs, ve, i->c, idx, nv); touch(vs, ve, i->dst, idx, nv); break;
    case IR_LOAD: case IR_MOV: case IR_NEG: case IR_NOT: case IR_EXT: touch(vs, ve, i->a, idx, nv); touch(vs, ve, i->dst, idx, nv); break;
    case IR_STORE: case IR_COPY: touch(vs, ve, i->a, idx, nv); touch(vs, ve, i->b, idx, nv); break;
    case IR_BIN: case IR_CMP: touch(vs, ve, i->a, idx, nv); touch(vs, ve, i->b, idx, nv); touch(vs, ve, i->dst, idx, nv); break;
    case IR_BRZ: case IR_BRNZ: case IR_ZERO: touch(vs, ve, i->a, idx, nv); break;
    case IR_RET: touch(vs, ve, i->a, idx, nv); break;
    case IR_CALL:
        if (!i->sym) touch(vs, ve, i->a, idx, nv);
        for (int k = 0; k < i->nargs; k++) touch(vs, ve, i->args[k], idx, nv);
        touch(vs, ve, i->rt, idx, nv);
        touch(vs, ve, i->dst, idx, nv);
        break;
    case IR_ASM: {
        const int *o = (const int *)(intptr_t)i->imm;
        for (int k = 0; k < i->nargs; k++) touch(vs, ve, i->args[k], idx, nv);
        for (int k = 0; k < i->nout; k++) touch(vs, ve, o[k], idx, nv);
        break;
    }
    default: break;
    }
}

