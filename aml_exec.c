/* Интерпретатор AML, часть 2: выполнение термов (операторы, выражения, определения объектов). */
#include "aml_int.h"

static void mcpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static void mset(void* d, int v, size_t n) { uint8_t* a = (uint8_t*)d; while (n--) *a++ = (uint8_t)v; }

enum { TG_NONE = 0, TG_SLOT, TG_ARG, TG_NODE, TG_BYTE, TG_DEBUG };
typedef struct { uint8_t k; aml_val_t* slot; aml_node_t* node; uint8_t* byte; } tgt_t;

static int term_in(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out);

static int is_name_lead(uint8_t b) { return (b >= 'A' && b <= 'Z') || b == '_' || b == '\\' || b == '^' || b == 0x2E || b == 0x2F; }

int a_term(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out) {
    if (++c->steps > c->max_steps) return AML_E_STEPS;
    if (c->tdepth >= c->max_tdepth) return AML_E_DEPTH;
    if (c->stack_floor && (uintptr_t)__builtin_frame_address(0) < c->stack_floor) return AML_E_DEPTH;
    c->tdepth++;
    mset(out, 0, sizeof *out);
    int r = term_in(c, f, cu, out);
    c->tdepth--;
    return r;
}

int a_exec_block(aml_ctx_t* c, frame_t* f, cur_t* cu) {
    while (cu->p < cu->end) {
        aml_val_t v;
        TRY(a_term(c, f, cu, &v));
    }
    return 0;
}

/* Значение-операнд: вычислить терм и раскрыть ссылки. */
static int arg_val(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* v) {
    TRY(a_term(c, f, cu, v));
    return a_deref(c, v);
}
static int arg_int(aml_ctx_t* c, frame_t* f, cur_t* cu, uint64_t* x) {
    aml_val_t v;
    TRY(a_term(c, f, cu, &v));
    return a_to_int(c, &v, x);
}

/* ---------------- цели записи ---------------- */

static int target(aml_ctx_t* c, frame_t* f, cur_t* cu, tgt_t* t) {
    mset(t, 0, sizeof *t);
    NEED(cu, 1);
    uint8_t b = *cu->p;
    if (b == 0x00) { cu->p++; t->k = TG_NONE; return 0; }
    if (b >= 0x60 && b <= 0x67) { cu->p++; t->k = TG_SLOT; t->slot = &f->loc[b - 0x60]; if (!f->loc) return AML_FAIL(AML_E_PARSE); return 0; }
    if (b >= 0x68 && b <= 0x6E) {
        cu->p++;
        if (!f->args || (unsigned)(b - 0x68) >= f->nargs) return AML_FAIL(AML_E_PARSE);
        t->k = TG_ARG; t->slot = &f->args[b - 0x68]; return 0;
    }
    if (b == 0x5B && (size_t)(cu->end - cu->p) >= 2 && cu->p[1] == 0x31) { cu->p += 2; t->k = TG_DEBUG; return 0; }
    if (is_name_lead(b)) {
        nm_t nm; aml_node_t* n;
        TRY(a_parse_name(cu, &nm));
        TRY(a_lookup(c, f, f->scope, &nm, 1, &n));
        if (n->type == AML_N_ALIAS && n->u.alias) n = n->u.alias;
        if (n->type != AML_N_NAME && n->type != AML_N_FIELD && n->type != AML_N_BUFFIELD) return AML_E_TYPE;
        t->k = TG_NODE; t->node = n; return 0;
    }
    if (b == 0x83 || b == 0x88 || b == 0x71) {              /* DerefOf, Index, RefOf: дают ссылку */
        aml_val_t v;
        TRY(a_term(c, f, cu, &v));
        if (v.t != AML_V_REF) return AML_E_TYPE;
        if (v.rk == RK_SLOT) { t->k = TG_SLOT; t->slot = v.u.p; if (!t->slot) return AML_E_TYPE; }
        else if (v.rk == RK_BYTE) { t->k = TG_BYTE; t->byte = v.u.b; }
        else { t->k = TG_NODE; t->node = v.u.n; }
        return 0;
    }
    return AML_FAIL(AML_E_PARSE);
}

static int tgt_read(aml_ctx_t* c, tgt_t* t, aml_val_t* out) {
    switch (t->k) {
    case TG_SLOT: case TG_ARG: *out = *t->slot; return 0;
    case TG_NODE: return a_node_read(c, t->node, out);
    case TG_BYTE: *out = aml_int(*t->byte); return 0;
    default: return AML_E_TYPE;
    }
}

static void log_val(aml_ctx_t* c, const aml_val_t* v) {
    if (!c->ops.log) return;
    char b[80]; unsigned n = 0;
    if (v->t == AML_V_INT) {
        static const char hx[] = "0123456789ABCDEF";
        b[n++] = '0'; b[n++] = 'x';
        int started = 0;
        for (int s = 60; s >= 0; s -= 4) { unsigned d = (unsigned)(v->u.i >> s) & 15; if (d || started || s == 0) { b[n++] = hx[d]; started = 1; } }
    } else if (v->t == AML_V_STR) { for (uint32_t i = 0; i < v->len && n < 70; i++) b[n++] = (char)(v->u.b[i] >= 32 && v->u.b[i] < 127 ? v->u.b[i] : '?'); }
    else { b[n++] = '?'; }
    b[n] = 0;
    c->ops.log(c->ops.user, b);
}

/* Записать значение в цель; результат (то, что осело в цели) - в *res, если задан. */
static int store(aml_ctx_t* c, frame_t* f, tgt_t* t, const aml_val_t* v) {
    (void)f;
    switch (t->k) {
    case TG_NONE: return 0;
    case TG_DEBUG: { aml_val_t d = *v; if (a_deref(c, &d) == 0) log_val(c, &d); return 0; }
    case TG_BYTE: { uint64_t x; TRY(a_to_int(c, v, &x)); *t->byte = (uint8_t)x; return 0; }
    case TG_NODE: return a_node_write(c, t->node, v);
    case TG_ARG:
        if (t->slot->t == AML_V_REF) {                                  /* аргумент - ссылка: запись идёт в то, на что она указывает */
            aml_val_t r = *t->slot;
            tgt_t u; mset(&u, 0, sizeof u);
            if (r.rk == RK_SLOT) { u.k = TG_SLOT; u.slot = r.u.p; if (!u.slot) return AML_E_TYPE; }
            else if (r.rk == RK_BYTE) { u.k = TG_BYTE; u.byte = r.u.b; }
            else { u.k = TG_NODE; u.node = r.u.n; }
            return store(c, f, &u, v);
        }
        /* fallthrough */
    case TG_SLOT: {
        aml_val_t p;
        int pers = a_is_persistent(c, t->slot);
        if (pers && t->slot->t != AML_V_NONE && t->slot->t == v->t && v->t == AML_V_INT) { t->slot->u.i = v->u.i; return 0; }
        TRY(a_copy(c, v, &p, pers, 0));
        *t->slot = p;
        return 0; }
    }
    return AML_E_TYPE;
}

/* ---------------- вспомогательные построители ---------------- */

static int mk_buf(aml_ctx_t* c, uint32_t n, aml_val_t* out, uint8_t** data) {
    if (n > (1u << 22)) return AML_E_NOMEM;
    uint8_t* b = (uint8_t*)a_alloc(c, (size_t)n + 1, 0);
    if (!b) return AML_E_NOMEM;
    mset(out, 0, sizeof *out); out->t = AML_V_BUF; out->len = n; out->u.b = b;
    if (data) *data = b;
    return 0;
}
static int mk_str(aml_ctx_t* c, const uint8_t* s, uint32_t n, aml_val_t* out) {
    uint8_t* b;
    TRY(mk_buf(c, n, out, &b));
    for (uint32_t i = 0; i < n; i++) b[i] = s[i];
    out->t = AML_V_STR;
    return 0;
}

/* Элементы пакета: имя - ссылка на объект, остальное - обычные термы. */
static int build_pkg(aml_ctx_t* c, frame_t* f, cur_t* sub, uint64_t n, aml_val_t* out) {
    if (n > (1u << 16)) return AML_E_NOMEM;
    aml_val_t* el = (aml_val_t*)a_alloc(c, (size_t)n * sizeof(aml_val_t) + 8, 0);
    if (!el) return AML_E_NOMEM;
    uint64_t i = 0;
    while (sub->p < sub->end) {
        if (i >= n) return AML_FAIL(AML_E_PARSE);
        if (is_name_lead(*sub->p)) {
            nm_t nm; aml_node_t* nd;
            TRY(a_parse_name(sub, &nm));
            if (a_lookup(c, f, f->scope, &nm, 1, &nd) == 0) { el[i].t = AML_V_REF; el[i].rk = RK_NODE; el[i].u.n = nd; }
        } else TRY(a_term(c, f, sub, &el[i]));
        i++;
    }
    mset(out, 0, sizeof *out); out->t = AML_V_PKG; out->len = (uint32_t)n; out->u.p = el;
    return 0;
}

/* ---------------- вызов по имени ---------------- */

static int name_term(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out) {
    nm_t nm; aml_node_t* n;
    TRY(a_parse_name(cu, &nm));
    TRY(a_lookup(c, f, f->scope, &nm, 1, &n));
    if (n->type == AML_N_ALIAS && n->u.alias) n = n->u.alias;
    if (n->type == AML_N_METHOD) {
        unsigned argc = n->u.m.argc;
        aml_val_t av[AML_MAX_ARGS];
        for (unsigned i = 0; i < argc; i++) TRY(a_term(c, f, cu, &av[i]));
        return a_call_node(c, n, av, argc, out);
    }
    return a_node_read(c, n, out);
}

/* ---------------- арифметика ---------------- */

static int cmp_vals(aml_ctx_t* c, aml_val_t* a, aml_val_t* b, int* res) {
    TRY(a_deref(c, a)); TRY(a_deref(c, b));
    if (a->t == AML_V_INT || (a->t != AML_V_STR && a->t != AML_V_BUF)) {
        uint64_t x, y; TRY(a_to_int(c, a, &x)); TRY(a_to_int(c, b, &y));
        *res = x < y ? -1 : x > y ? 1 : 0; return 0;
    }
    const uint8_t *p, *q; uint32_t n, m;
    TRY(a_bytes_of(a, &p, &n));
    if (b->t != AML_V_STR && b->t != AML_V_BUF) return AML_E_TYPE;
    TRY(a_bytes_of(b, &q, &m));
    uint32_t k = n < m ? n : m;
    for (uint32_t i = 0; i < k; i++) if (p[i] != q[i]) { *res = p[i] < q[i] ? -1 : 1; return 0; }
    *res = n < m ? -1 : n > m ? 1 : 0;
    return 0;
}

static void hex_str(uint64_t v, int prefix, uint8_t* buf, uint32_t* n) {
    static const char hx[] = "0123456789ABCDEF";
    uint32_t k = 0;
    if (prefix) { buf[k++] = '0'; buf[k++] = 'x'; }
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) { unsigned d = (unsigned)(v >> s) & 15; if (d || started || s == 0) { buf[k++] = (uint8_t)hx[d]; started = 1; } }
    *n = k;
}

static int do_concat(aml_ctx_t* c, aml_val_t* a, aml_val_t* b, aml_val_t* out) {
    TRY(a_deref(c, a)); TRY(a_deref(c, b));
    uint8_t tmp[24]; uint32_t tn;
    if (a->t == AML_V_STR) {
        const uint8_t* q; uint32_t m;
        if (b->t == AML_V_INT) { hex_str(b->u.i, 0, tmp, &tn); q = tmp; m = tn; }
        else TRY(a_bytes_of(b, &q, &m));
        if (a->len > (1u << 20) || m > (1u << 20)) return AML_E_NOMEM;
        uint8_t* d; TRY(mk_buf(c, a->len + m, out, &d)); out->t = AML_V_STR;
        mcpy(d, a->u.b, a->len); mcpy(d + a->len, q, m);
        return 0;
    }
    uint32_t w = c->intbits / 8;
    uint32_t la = a->t == AML_V_INT ? w : a->len, lb = b->t == AML_V_INT ? w : (b->t == AML_V_BUF || b->t == AML_V_STR) ? b->len : 0;
    if (a->t != AML_V_INT && a->t != AML_V_BUF) return AML_E_TYPE;
    if (b->t != AML_V_INT && b->t != AML_V_BUF && b->t != AML_V_STR) return AML_E_TYPE;
    if (la > (1u << 20) || lb > (1u << 20)) return AML_E_NOMEM;
    uint8_t* d; TRY(mk_buf(c, la + lb, out, &d));
    if (a->t == AML_V_INT) for (uint32_t i = 0; i < w; i++) d[i] = (uint8_t)(a->u.i >> (8 * i)); else mcpy(d, a->u.b, la);
    if (b->t == AML_V_INT) for (uint32_t i = 0; i < w; i++) d[la + i] = (uint8_t)(b->u.i >> (8 * i)); else mcpy(d + la, b->u.b, lb);
    return 0;
}

/* ---------------- определения полей ---------------- */

int a_define_field(aml_ctx_t* c, frame_t* f, cur_t* cu, int kind, aml_node_t* region, aml_node_t* idx, aml_node_t* data, aml_node_t* bank,
                   const uint8_t* bankpc, const uint8_t* bankend, uint8_t flags) {
    uint64_t off = 0;
    unsigned access = flags & 0x0F, update = (flags >> 5) & 3;
    while (cu->p < cu->end) {
        uint8_t b = *cu->p;
        if (b == 0x00) { cu->p++; uint32_t n; TRY(a_field_raw_len(cu, &n)); off += n; }
        else if (b == 0x01) { NEED(cu, 3); access = cu->p[1] & 0x0F; cu->p += 3; }
        else if (b == 0x03) { NEED(cu, 4); access = cu->p[1] & 0x0F; cu->p += 4; }
        else if (b == 0x02) {
            cu->p++; NEED(cu, 1);
            if (*cu->p == 0x11) { cu->p++; const uint8_t* pe; TRY(a_pkg_len(cu, &pe)); cu->p = pe; }
            else { nm_t nm; TRY(a_parse_name(cu, &nm)); }
        }
        else if (b >= 'A' || b == '_') {
            NEED(cu, 4);
            const uint8_t* seg = cu->p;
            for (int i = 0; i < 4; i++) {
                uint8_t ch = seg[i];
                if (!((ch >= 'A' && ch <= 'Z') || ch == '_' || (i && ch >= '0' && ch <= '9'))) return AML_FAIL(AML_E_PARSE);
            }
            cu->p += 4;
            uint32_t n; TRY(a_field_raw_len(cu, &n));
            if (off > 0x7FFFFFFFull || n > 0x7FFFFFFFu) return AML_FAIL(AML_E_PARSE);
            aml_node_t* nd;
            int r = a_new_node(c, f, f->scope, seg, AML_N_FIELD, &nd);
            if (r < 0) return r;
            if (r == 0) {
                nd->u.f.kind = (uint8_t)kind; nd->u.f.region = region; nd->u.f.idx = idx; nd->u.f.data = data; nd->u.f.bank = bank;
                nd->u.f.bankpc = bankpc; nd->u.f.bankend = bankend;
                nd->u.f.bitoff = (uint32_t)off; nd->u.f.bitlen = n; nd->u.f.access = (uint8_t)access; nd->u.f.update = (uint8_t)update;
            }
            off += n;
        }
        else return AML_FAIL(AML_E_PARSE);
    }
    return 0;
}

/* Константа или имя в загрузке: пропустить, не вычисляя (вычислится при первом обращении). 1 - пропущено. */
static int skip_simple(cur_t* cu) {
    if (cu->p >= cu->end) return 0;
    uint8_t b = *cu->p;
    size_t left = (size_t)(cu->end - cu->p);
    switch (b) {
    case 0x00: case 0x01: case 0xFF: cu->p += 1; return 1;
    case 0x0A: if (left < 2) return 0; cu->p += 2; return 1;
    case 0x0B: if (left < 3) return 0; cu->p += 3; return 1;
    case 0x0C: if (left < 5) return 0; cu->p += 5; return 1;
    case 0x0E: if (left < 9) return 0; cu->p += 9; return 1;
    default:
        if (is_name_lead(b)) { cur_t t = *cu; nm_t nm; if (a_parse_name(&t, &nm) == 0) { *cu = t; return 1; } }
        return 0;
    }
}

/* Данные для Name в загрузке: пропустить, не вычисляя, если это строка, буфер, пакет или константа. 1 - пропущено. */
static int skip_data(cur_t* cu) {
    if (skip_simple(cu)) {
        /* имя в Name(X, Y) - это вызов или чтение: отдаём на вычисление сразу */
        return 1;
    }
    if (cu->p >= cu->end) return 0;
    uint8_t b = *cu->p;
    if (b == 0x0D) {
        const uint8_t* q = cu->p + 1;
        while (q < cu->end && *q) q++;
        if (q >= cu->end) return 0;
        cu->p = q + 1; return 1;
    }
    if (b == 0x11 || b == 0x12 || b == 0x13) {
        cur_t t = *cu; t.p++;
        const uint8_t* pe;
        if (a_pkg_len(&t, &pe) != 0) return 0;
        cu->p = pe; return 1;
    }
    return 0;
}

static int obj_type_code(aml_ctx_t* c, const aml_val_t* v) {
    (void)c;
    switch (v->t) { case AML_V_INT: return 1; case AML_V_STR: return 2; case AML_V_BUF: return 3; case AML_V_PKG: return 4; default: return 0; }
}

/* ---------------- главный разбор терма ---------------- */

static int define_scope_obj(aml_ctx_t* c, frame_t* f, cur_t* cu, uint8_t op) {
    if (!f->loading) return AML_E_UNSUP;
    const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
    cur_t sub = { cu->p, pe };
    nm_t nm; TRY(a_parse_name(&sub, &nm));
    aml_node_t* n = 0;
    int type;
    switch (op) { case 0x82: type = AML_N_DEVICE; break; case 0x83: type = AML_N_PROC; break; case 0x84: type = AML_N_POWERRES; break; default: type = AML_N_THERMAL; break; }
    if (nm.nseg == 0) return AML_FAIL(AML_E_PARSE);
    aml_node_t* par;
    nm_t pn = nm; pn.nseg--;
    if (a_lookup(c, f, f->scope, &pn, 0, &par) != 0) { c->load_errors++, (void)AML_FAIL(0); cu->p = pe; return 0; }
    int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), type, &n);
    if (r < 0) return r;
    if (op == 0x83) { NEED(&sub, 6); sub.p += 6; }
    else if (op == 0x84) { NEED(&sub, 3); sub.p += 3; }
    aml_node_t* save = f->scope;
    f->scope = n;
    r = a_exec_block(c, f, &sub);
    f->scope = save;
    cu->p = pe;
    return r;
}

/* Определение имени с родителем: путь без последнего сегмента ищется от текущей области (без подъёма вверх). */
static int resolve_parent(aml_ctx_t* c, frame_t* f, const nm_t* nm, aml_node_t** par) {
    if (nm->nseg == 0) return AML_FAIL(AML_E_PARSE);
    nm_t pn = *nm; pn.nseg--;
    return a_lookup(c, f, f->scope, &pn, 0, par);
}

/* Данные Name: в загрузке - откладываем вычисление до первого чтения (кроме выражений), в методе - вычисляем сразу. */
static int def_name(aml_ctx_t* c, frame_t* f, cur_t* cu) {
    nm_t nm; aml_node_t *par, *n;
    TRY(a_parse_name(cu, &nm));
    if (resolve_parent(c, f, &nm, &par) != 0) {
        c->load_errors++, (void)AML_FAIL(0);
        if (skip_data(cu)) return 0;
        aml_val_t d; return a_term(c, f, cu, &d);
    }
    int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), AML_N_NAME, &n);
    if (r < 0) return r;
    if (r == 1) {                                                       /* имя уже есть: данные пропускаем */
        if (skip_data(cu)) return 0;
        aml_val_t d; return a_term(c, f, cu, &d);
    }
    cur_t save = *cu;
    if (f->loading && skip_data(cu)) { n->u.d.lazy = save.p; n->u.d.lend = save.end; n->u.d.state = 1; return 0; }
    aml_val_t v, p;
    TRY(a_term(c, f, cu, &v));
    TRY(a_deref(c, &v));
    TRY(a_copy(c, &v, &p, f->loading, 0));
    n->u.d.v = p; n->u.d.state = 0;
    return 0;
}

static int def_buffield(aml_ctx_t* c, frame_t* f, cur_t* cu, int kind) {
    aml_val_t src; uint64_t idx, nbits = 0;
    TRY(a_term(c, f, cu, &src)); TRY(a_deref(c, &src));
    TRY(arg_int(c, f, cu, &idx));
    if (kind == 5) TRY(arg_int(c, f, cu, &nbits));
    nm_t nm; aml_node_t *par, *n;
    TRY(a_parse_name(cu, &nm));
    if (src.t != AML_V_BUF && src.t != AML_V_STR) return AML_E_TYPE;
    uint64_t bo, bl;
    switch (kind) { case 0: bo = idx * 8; bl = 8; break; case 1: bo = idx * 8; bl = 16; break; case 2: bo = idx * 8; bl = 32; break; case 3: bo = idx * 8; bl = 64; break; case 4: bo = idx; bl = 1; break; default: bo = idx; bl = nbits; break; }
    if (bl == 0 || bo > (uint64_t)src.len * 8 || bl > (uint64_t)src.len * 8 - bo) return AML_E_BOUNDS;
    TRY(resolve_parent(c, f, &nm, &par));
    int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), AML_N_BUFFIELD, &n);
    if (r < 0) return r;
    if (r == 1) return 0;
    uint8_t* data = src.u.b;
    if (f->loading && !a_is_persistent(c, data)) {                  /* буфер из выражения в загрузке: оставляем себе копию */
        aml_val_t p; TRY(a_copy(c, &src, &p, 1, 0)); data = p.u.b;
    }
    n->u.bf.data = data; n->u.bf.datalen = src.len; n->u.bf.bitoff = (uint32_t)bo; n->u.bf.bitlen = (uint32_t)bl;
    return 0;
}

static int arith(aml_ctx_t* c, frame_t* f, cur_t* cu, uint8_t op, aml_val_t* out) {
    uint64_t a, b, r = 0; tgt_t t;
    TRY(arg_int(c, f, cu, &a)); TRY(arg_int(c, f, cu, &b));
    TRY(target(c, f, cu, &t));
    switch (op) {
    case 0x72: r = a + b; break;
    case 0x74: r = a - b; break;
    case 0x77: r = a * b; break;
    case 0x79: r = b >= c->intbits ? 0 : a << b; break;
    case 0x7A: r = b >= c->intbits ? 0 : a >> b; break;
    case 0x7B: r = a & b; break;
    case 0x7C: r = ~(a & b); break;
    case 0x7D: r = a | b; break;
    case 0x7E: r = ~(a | b); break;
    case 0x7F: r = a ^ b; break;
    default: if (b == 0) return AML_FAIL(AML_E_PARSE); r = a % b; break;       /* 0x85 Mod */
    }
    r &= c->intmask;
    *out = aml_int(r);
    return store(c, f, &t, out);
}

static int ext_term(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out) {
    NEED(cu, 1);
    uint8_t x = *cu->p++;
    switch (x) {
    case 0x01: case 0x02: {                                          /* Mutex, Event */
        nm_t nm; aml_node_t *par, *n;
        TRY(a_parse_name(cu, &nm));
        if (x == 0x01) { NEED(cu, 1); cu->p++; }
        if (resolve_parent(c, f, &nm, &par) != 0) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), x == 1 ? AML_N_MUTEX : AML_N_EVENT, &n);
        return r < 0 ? r : 0; }
    case 0x12: {                                                     /* CondRefOf */
        nm_t nm; aml_node_t* n; tgt_t t;
        NEED(cu, 1);
        if (!is_name_lead(*cu->p)) return AML_E_UNSUP;
        TRY(a_parse_name(cu, &nm));
        int found = a_lookup(c, f, f->scope, &nm, 1, &n) == 0;
        TRY(target(c, f, cu, &t));
        if (found && t.k != TG_NONE) { aml_val_t rf; mset(&rf, 0, sizeof rf); rf.t = AML_V_REF; rf.rk = RK_NODE; rf.u.n = n; TRY(store(c, f, &t, &rf)); }
        *out = aml_int(found ? c->intmask : 0);
        return 0; }
    case 0x13: return def_buffield(c, f, cu, 5);
    case 0x21: { uint64_t us; TRY(arg_int(c, f, cu, &us)); if (us > 100) us = 100;
        c->slept += (uint32_t)us; if (c->slept > c->max_sleep * 1000u) return AML_E_TIME;
        if (c->ops.stall_us) c->ops.stall_us(c->ops.user, (uint32_t)us);
        return 0; }
    case 0x22: { uint64_t ms; TRY(arg_int(c, f, cu, &ms)); if (ms > c->max_sleep) return AML_E_TIME;
        c->slept += (uint32_t)ms * 1000u; if (c->slept > c->max_sleep * 1000u) return AML_E_TIME;
        if (c->ops.sleep_ms) c->ops.sleep_ms(c->ops.user, (uint32_t)ms);
        return 0; }
    case 0x23: { tgt_t t; TRY(target(c, f, cu, &t)); NEED(cu, 2); cu->p += 2; *out = aml_int(0); return 0; }   /* Acquire: поток один, замок всегда наш (0 - получен) */
    case 0x27: case 0x25: case 0x26: { tgt_t t; TRY(target(c, f, cu, &t)); return 0; }                         /* Release, Signal, Reset */
    case 0x24: return AML_E_UNSUP;                                   /* Wait: ждать событие некому */
    case 0x28: { uint64_t v, r = 0; tgt_t t; TRY(arg_int(c, f, cu, &v)); TRY(target(c, f, cu, &t));
        for (int s = 60; s >= 0; s -= 4) { unsigned d = (unsigned)(v >> s) & 15; if (d > 9) return AML_FAIL(AML_E_PARSE); r = r * 10 + d; }
        *out = aml_int(r & c->intmask); return store(c, f, &t, out); }
    case 0x29: { uint64_t v, r = 0; tgt_t t; TRY(arg_int(c, f, cu, &v)); TRY(target(c, f, cu, &t));
        for (int s = 0; v && s < 64; s += 4) { r |= (v % 10) << s; v /= 10; }
        *out = aml_int(r & c->intmask); return store(c, f, &t, out); }
    case 0x20: case 0x1F: case 0x2A: return AML_E_UNSUP;             /* Load, LoadTable, Unload */
    case 0x30: *out = aml_int(c->rev); return 0;
    case 0x31: out->t = AML_V_NONE; return 0;                        /* Debug */
    case 0x32: {                                                     /* Fatal: прошивка требует остановки */
        NEED(cu, 5); cu->p += 5; aml_val_t v; TRY(a_term(c, f, cu, &v));
        if (c->ops.log) c->ops.log(c->ops.user, "AML Fatal");
        return AML_E_FATAL; }
    case 0x33: *out = aml_int(c->ops.timer_100ns ? c->ops.timer_100ns(c->ops.user) : (uint64_t)c->steps); return 0;
    case 0x80: {                                                     /* OperationRegion */
        nm_t nm; aml_node_t *par, *n;
        TRY(a_parse_name(cu, &nm));
        NEED(cu, 1); uint8_t space = *cu->p++;
        cur_t o = *cu; int off_simple = skip_simple(cu); const uint8_t* off_pc = o.p;
        aml_val_t v; uint64_t off = 0, len = 0;
        if (!off_simple) { cu->p = o.p; TRY(arg_int(c, f, cu, &off)); }
        cur_t l = *cu; int len_simple = skip_simple(cu); const uint8_t* len_pc = l.p;
        if (!len_simple) { cu->p = l.p; TRY(arg_int(c, f, cu, &len)); }
        (void)v;
        if (resolve_parent(c, f, &nm, &par) != 0) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), AML_N_REGION, &n);
        if (r < 0) return r;
        if (r == 1) return 0;
        n->u.r.space = space; n->u.r.end = cu->end;
        n->u.r.off = off; n->u.r.len = len;
        if (off_simple) n->u.r.off_pc = off_pc;
        if (len_simple) n->u.r.len_pc = len_pc;
        return 0; }
    case 0x81: case 0x86: case 0x87: {                               /* Field, IndexField, BankField */
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        nm_t nm; aml_node_t *n1 = 0, *n2 = 0; int ok = 1;
        TRY(a_parse_name(&sub, &nm));
        if (a_lookup(c, f, f->scope, &nm, 1, &n1) != 0) ok = 0;
        if (x != 0x81) { TRY(a_parse_name(&sub, &nm)); if (a_lookup(c, f, f->scope, &nm, 1, &n2) != 0) ok = 0; }
        const uint8_t* bpc = 0; const uint8_t* bend = 0;
        if (x == 0x87) {
            cur_t b = sub;
            if (skip_simple(&sub)) { bpc = b.p; bend = sub.end; }
            else { aml_val_t v; uint64_t bv; sub = b; TRY(a_term(c, f, &sub, &v)); TRY(a_to_int(c, &v, &bv)); (void)bv; return AML_E_UNSUP; }
        }
        NEED(&sub, 1); uint8_t flags = *sub.p++;
        cu->p = pe;
        if (!ok || !n1) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        if (x == 0x81) { if (n1->type != AML_N_REGION) { c->load_errors++, (void)AML_FAIL(0); return 0; } return a_define_field(c, f, &sub, FK_REGION, n1, 0, 0, 0, 0, 0, flags); }
        if (x == 0x86) { if (n1->type != AML_N_FIELD || n2->type != AML_N_FIELD) { c->load_errors++, (void)AML_FAIL(0); return 0; } return a_define_field(c, f, &sub, FK_INDEX, 0, n1, n2, 0, 0, 0, flags); }
        if (n1->type != AML_N_REGION || n2->type != AML_N_FIELD) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        return a_define_field(c, f, &sub, FK_BANK, n1, 0, 0, n2, bpc, bend, flags); }
    case 0x82: case 0x83: case 0x84: case 0x85: return define_scope_obj(c, f, cu, x);
    case 0x88: return AML_E_UNSUP;                                   /* DataTableRegion */
    default: return AML_FAIL(AML_E_PARSE);
    }
}

static int term_in(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out) {
    NEED(cu, 1);
    uint8_t op = *cu->p++;
    if (is_name_lead(op)) { cu->p--; return name_term(c, f, cu, out); }
    if (op >= 0x60 && op <= 0x67) {
        if (!f->loc) return AML_FAIL(AML_E_PARSE);
        *out = f->loc[op - 0x60]; return 0;
    }
    if (op >= 0x68 && op <= 0x6E) {
        if (!f->args || (unsigned)(op - 0x68) >= f->nargs) return AML_FAIL(AML_E_PARSE);
        *out = f->args[op - 0x68]; return 0;
    }
    switch (op) {
    case 0x00: *out = aml_int(0); return 0;
    case 0x01: *out = aml_int(1); return 0;
    case 0xFF: *out = aml_int(c->intmask); return 0;
    case 0x0A: NEED(cu, 1); *out = aml_int(cu->p[0]); cu->p += 1; return 0;
    case 0x0B: NEED(cu, 2); *out = aml_int((uint64_t)cu->p[0] | (uint64_t)cu->p[1] << 8); cu->p += 2; return 0;
    case 0x0C: NEED(cu, 4); *out = aml_int((uint64_t)cu->p[0] | (uint64_t)cu->p[1] << 8 | (uint64_t)cu->p[2] << 16 | (uint64_t)cu->p[3] << 24); cu->p += 4; return 0;
    case 0x0E: { NEED(cu, 8); uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)cu->p[i] << (8 * i); *out = aml_int(v & c->intmask); cu->p += 8; return 0; }
    case 0x0D: {
        const uint8_t* q = cu->p;
        while (q < cu->end && *q) q++;
        if (q >= cu->end) return AML_FAIL(AML_E_PARSE);
        TRY(mk_str(c, cu->p, (uint32_t)(q - cu->p), out));
        cu->p = q + 1; return 0; }
    case 0x11: {
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        aml_val_t sz; uint64_t n;
        TRY(a_term(c, f, &sub, &sz)); TRY(a_to_int(c, &sz, &n));
        if (n > (1u << 22)) return AML_E_NOMEM;
        uint8_t* d; TRY(mk_buf(c, (uint32_t)n, out, &d));
        size_t init = (size_t)(pe - sub.p);
        if (init > n) init = (size_t)n;
        mcpy(d, sub.p, init);
        cu->p = pe; return 0; }
    case 0x12: case 0x13: {
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        uint64_t n;
        if (op == 0x12) { NEED(&sub, 1); n = *sub.p++; }
        else { aml_val_t sz; TRY(a_term(c, f, &sub, &sz)); TRY(a_to_int(c, &sz, &n)); }
        TRY(build_pkg(c, f, &sub, n, out));
        cu->p = pe; return 0; }
    case 0x5B: return ext_term(c, f, cu, out);
    case 0x06: {                                                     /* Alias */
        nm_t src, dst; aml_node_t *sn, *par, *n;
        TRY(a_parse_name(cu, &src)); TRY(a_parse_name(cu, &dst));
        if (!f->loading) return AML_E_UNSUP;
        if (a_lookup(c, f, f->scope, &src, 1, &sn) != 0 || resolve_parent(c, f, &dst, &par) != 0) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        int r = a_new_node(c, f, par, dst.seg + 4 * (dst.nseg - 1), AML_N_ALIAS, &n);
        if (r < 0) return r;
        if (r == 0) n->u.alias = sn;
        return 0; }
    case 0x08: return def_name(c, f, cu);
    case 0x10: {                                                     /* Scope */
        if (!f->loading) return AML_E_UNSUP;
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        nm_t nm; aml_node_t* n;
        TRY(a_parse_name(&sub, &nm));
        cu->p = pe;
        if (a_lookup(c, f, f->scope, &nm, 0, &n) != 0) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        aml_node_t* save = f->scope; f->scope = n;
        int r = a_exec_block(c, f, &sub);
        f->scope = save;
        return r; }
    case 0x14: {                                                     /* Method */
        if (!f->loading) return AML_E_UNSUP;
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        nm_t nm; aml_node_t *par, *n;
        TRY(a_parse_name(&sub, &nm));
        NEED(&sub, 1); uint8_t fl = *sub.p++;
        cu->p = pe;
        if (resolve_parent(c, f, &nm, &par) != 0) { c->load_errors++, (void)AML_FAIL(0); return 0; }
        int r = a_new_node(c, f, par, nm.seg + 4 * (nm.nseg - 1), AML_N_METHOD, &n);
        if (r < 0) return r;
        if (r == 0) { n->u.m.body = sub.p; n->u.m.end = pe; n->u.m.argc = fl & 7; n->u.m.serial = (fl >> 3) & 1; }
        return 0; }
    case 0x15: { nm_t nm; TRY(a_parse_name(cu, &nm)); NEED(cu, 2); cu->p += 2; return 0; }     /* External */
    case 0x8A: return def_buffield(c, f, cu, 2);
    case 0x8B: return def_buffield(c, f, cu, 1);
    case 0x8C: return def_buffield(c, f, cu, 0);
    case 0x8D: return def_buffield(c, f, cu, 4);
    case 0x8F: return def_buffield(c, f, cu, 3);
    case 0x70: {                                                     /* Store */
        aml_val_t v; tgt_t t;
        TRY(a_term(c, f, cu, &v));
        TRY(target(c, f, cu, &t));
        TRY(store(c, f, &t, &v));
        *out = v; return 0; }
    case 0x9D: {                                                     /* CopyObject */
        aml_val_t v; tgt_t t;
        TRY(a_term(c, f, cu, &v));
        TRY(target(c, f, cu, &t));
        TRY(store(c, f, &t, &v));
        *out = v; return 0; }
    case 0x71: {                                                     /* RefOf */
        tgt_t t; TRY(target(c, f, cu, &t));
        out->t = AML_V_REF;
        if (t.k == TG_SLOT || t.k == TG_ARG) { out->rk = RK_SLOT; out->u.p = t.slot; }
        else if (t.k == TG_NODE) { out->rk = RK_NODE; out->u.n = t.node; }
        else if (t.k == TG_BYTE) { out->rk = RK_BYTE; out->u.b = t.byte; }
        else return AML_E_TYPE;
        return 0; }
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85:
        return arith(c, f, cu, op, out);
    case 0x75: case 0x76: {                                          /* Increment, Decrement */
        tgt_t t; aml_val_t v; uint64_t x;
        TRY(target(c, f, cu, &t)); TRY(tgt_read(c, &t, &v)); TRY(a_to_int(c, &v, &x));
        x = (op == 0x75 ? x + 1 : x - 1) & c->intmask;
        *out = aml_int(x); return store(c, f, &t, out); }
    case 0x78: {                                                     /* Divide: делимое, делитель, остаток, частное */
        uint64_t a, b; tgt_t rem, quo;
        TRY(arg_int(c, f, cu, &a)); TRY(arg_int(c, f, cu, &b));
        TRY(target(c, f, cu, &rem)); TRY(target(c, f, cu, &quo));
        if (b == 0) return AML_FAIL(AML_E_PARSE);
        aml_val_t q = aml_int(a / b), r = aml_int(a % b);
        TRY(store(c, f, &rem, &r)); TRY(store(c, f, &quo, &q));
        *out = q; return 0; }
    case 0x80: case 0x81: case 0x82: {                               /* Not, FindSetLeftBit, FindSetRightBit */
        uint64_t a; tgt_t t; TRY(arg_int(c, f, cu, &a)); TRY(target(c, f, cu, &t));
        uint64_t r = 0;
        if (op == 0x80) r = ~a & c->intmask;
        else if (op == 0x81) { for (unsigned i = c->intbits; i; i--) if (a >> (i - 1) & 1) { r = i; break; } }
        else { for (unsigned i = 0; i < c->intbits; i++) if (a >> i & 1) { r = i + 1; break; } }
        *out = aml_int(r); return store(c, f, &t, out); }
    case 0x90: case 0x91: {                                          /* LAnd, LOr */
        uint64_t a, b; TRY(arg_int(c, f, cu, &a)); TRY(arg_int(c, f, cu, &b));
        *out = aml_int((op == 0x90 ? (a && b) : (a || b)) ? c->intmask : 0); return 0; }
    case 0x92: {                                                     /* LNot и составные: LNotEqual, LLessEqual, LGreaterEqual */
        NEED(cu, 1);
        uint8_t n = *cu->p;
        if (n == 0x93 || n == 0x94 || n == 0x95) {
            cu->p++;
            aml_val_t a, b; int r;
            TRY(a_term(c, f, cu, &a)); TRY(a_term(c, f, cu, &b)); TRY(cmp_vals(c, &a, &b, &r));
            int v = n == 0x93 ? r != 0 : n == 0x94 ? r <= 0 : r >= 0;
            *out = aml_int(v ? c->intmask : 0); return 0;
        }
        uint64_t a; TRY(arg_int(c, f, cu, &a)); *out = aml_int(a ? 0 : c->intmask); return 0; }
    case 0x93: case 0x94: case 0x95: {
        aml_val_t a, b; int r;
        TRY(a_term(c, f, cu, &a)); TRY(a_term(c, f, cu, &b)); TRY(cmp_vals(c, &a, &b, &r));
        int v = op == 0x93 ? r == 0 : op == 0x94 ? r > 0 : r < 0;
        *out = aml_int(v ? c->intmask : 0); return 0; }
    case 0x83: {                                                     /* DerefOf */
        aml_val_t v; TRY(a_term(c, f, cu, &v));
        if (v.t != AML_V_REF) return AML_E_TYPE;
        TRY(a_deref(c, &v));
        *out = v; return 0; }
    case 0x88: {                                                     /* Index */
        aml_val_t s; uint64_t i; tgt_t t;
        TRY(arg_val(c, f, cu, &s)); TRY(arg_int(c, f, cu, &i)); TRY(target(c, f, cu, &t));
        aml_val_t r; mset(&r, 0, sizeof r); r.t = AML_V_REF;
        if (s.t == AML_V_PKG) { if (i >= s.len) return AML_E_BOUNDS; r.rk = RK_SLOT; r.u.p = &s.u.p[i]; }
        else if (s.t == AML_V_BUF || s.t == AML_V_STR) { if (i >= s.len) return AML_E_BOUNDS; r.rk = RK_BYTE; r.u.b = &s.u.b[i]; }
        else return AML_E_TYPE;
        TRY(store(c, f, &t, &r));
        *out = r; return 0; }
    case 0x87: {                                                     /* SizeOf */
        aml_val_t v; TRY(arg_val(c, f, cu, &v));
        if (v.t != AML_V_STR && v.t != AML_V_BUF && v.t != AML_V_PKG) return AML_E_TYPE;
        *out = aml_int(v.len); return 0; }
    case 0x8E: {                                                     /* ObjectType */
        NEED(cu, 1);
        int code;
        if (is_name_lead(*cu->p)) {
            nm_t nm; aml_node_t* n;
            TRY(a_parse_name(cu, &nm));
            if (a_lookup(c, f, f->scope, &nm, 1, &n) != 0) code = 0;
            else {
                switch (n->type) {
                case AML_N_METHOD: code = 8; break; case AML_N_DEVICE: code = 6; break; case AML_N_FIELD: code = 5; break; case AML_N_BUFFIELD: code = 14; break;
                case AML_N_REGION: code = 10; break; case AML_N_MUTEX: code = 9; break; case AML_N_EVENT: code = 7; break; case AML_N_POWERRES: code = 11; break;
                case AML_N_PROC: code = 12; break; case AML_N_THERMAL: code = 13; break;
                default: { aml_val_t v; if (a_node_read(c, n, &v) == 0) { a_deref(c, &v); code = obj_type_code(c, &v); } else code = 0; } }
            }
        } else {
            aml_val_t v; TRY(a_term(c, f, cu, &v));
            if (v.t == AML_V_REF && v.rk == RK_BYTE) code = 1;
            else { if (v.t == AML_V_REF) (void)a_deref(c, &v); code = obj_type_code(c, &v); }
        }
        *out = aml_int((uint64_t)code); return 0; }
    case 0x73: {                                                     /* Concatenate */
        aml_val_t a, b; tgt_t t;
        TRY(a_term(c, f, cu, &a)); TRY(a_term(c, f, cu, &b)); TRY(target(c, f, cu, &t));
        TRY(do_concat(c, &a, &b, out));
        return store(c, f, &t, out); }
    case 0x96: case 0x97: case 0x98: case 0x99: {                    /* ToBuffer, ToDecimalString, ToHexString, ToInteger */
        aml_val_t v; tgt_t t;
        TRY(arg_val(c, f, cu, &v)); TRY(target(c, f, cu, &t));
        if (op == 0x99) { uint64_t x; TRY(a_to_int(c, &v, &x)); *out = aml_int(x); }
        else if (op == 0x96) {
            if (v.t == AML_V_INT) { uint8_t* d; uint32_t w = c->intbits / 8; TRY(mk_buf(c, w, out, &d)); for (uint32_t i = 0; i < w; i++) d[i] = (uint8_t)(v.u.i >> (8 * i)); }
            else if (v.t == AML_V_STR) { uint8_t* d; TRY(mk_buf(c, v.len + 1, out, &d)); mcpy(d, v.u.b, v.len); }
            else if (v.t == AML_V_BUF) *out = v; else return AML_E_TYPE;
        } else {
            uint8_t tmp[24]; uint32_t n;
            if (v.t == AML_V_INT) {
                if (op == 0x98) hex_str(v.u.i, 0, tmp, &n);
                else { uint64_t x = v.u.i; uint8_t rev[24]; uint32_t k = 0; do { rev[k++] = (uint8_t)('0' + x % 10); x /= 10; } while (x); n = 0; while (k) tmp[n++] = rev[--k]; }
                TRY(mk_str(c, tmp, n, out));
            } else if (v.t == AML_V_STR) *out = v;
            else if (v.t == AML_V_BUF) {                             /* буфер -> "0x1A,0x2B" / "26,43" */
                if (v.len > 4096) return AML_E_NOMEM;
                uint8_t* d; TRY(mk_buf(c, v.len * 5 + 1, out, &d)); out->t = AML_V_STR;
                uint32_t o = 0;
                for (uint32_t i = 0; i < v.len; i++) {
                    if (i) d[o++] = ',';
                    if (op == 0x98) { uint32_t k; hex_str(v.u.b[i], 0, tmp, &k); for (uint32_t j = 0; j < k; j++) d[o++] = tmp[j]; }
                    else { unsigned x = v.u.b[i]; if (x >= 100) d[o++] = (uint8_t)('0' + x / 100); if (x >= 10) d[o++] = (uint8_t)('0' + x / 10 % 10); d[o++] = (uint8_t)('0' + x % 10); }
                }
                out->len = o;
            } else return AML_E_TYPE;
        }
        return store(c, f, &t, out); }
    case 0x9C: {                                                     /* ToString(buffer, length) */
        aml_val_t v; uint64_t len; tgt_t t;
        TRY(arg_val(c, f, cu, &v)); TRY(arg_int(c, f, cu, &len)); TRY(target(c, f, cu, &t));
        if (v.t != AML_V_BUF && v.t != AML_V_STR) return AML_E_TYPE;
        uint32_t n = 0; while (n < v.len && v.u.b[n] && n < len) n++;
        if (len == c->intmask) { n = 0; while (n < v.len && v.u.b[n]) n++; }
        TRY(mk_str(c, v.u.b, n, out));
        return store(c, f, &t, out); }
    case 0x9E: {                                                     /* Mid */
        aml_val_t v; uint64_t i, n; tgt_t t;
        TRY(arg_val(c, f, cu, &v)); TRY(arg_int(c, f, cu, &i)); TRY(arg_int(c, f, cu, &n)); TRY(target(c, f, cu, &t));
        if (v.t != AML_V_BUF && v.t != AML_V_STR) return AML_E_TYPE;
        if (i >= v.len) n = 0; else if (n > v.len - i) n = v.len - i;
        uint8_t* d; TRY(mk_buf(c, (uint32_t)n, out, &d)); mcpy(d, v.u.b + (i >= v.len ? 0 : i), (size_t)n);
        out->t = v.t;
        return store(c, f, &t, out); }
    case 0x89: {                                                     /* Match */
        aml_val_t pk; TRY(arg_val(c, f, cu, &pk));
        NEED(cu, 1); uint8_t m1 = *cu->p++; aml_val_t o1; TRY(a_term(c, f, cu, &o1));
        NEED(cu, 1); uint8_t m2 = *cu->p++; aml_val_t o2; TRY(a_term(c, f, cu, &o2));
        uint64_t start; TRY(arg_int(c, f, cu, &start));
        if (pk.t != AML_V_PKG) return AML_E_TYPE;
        uint64_t found = c->intmask;
        for (uint64_t i = start; i < pk.len; i++) {
            aml_val_t e = pk.u.p[i]; int ok1 = 1, ok2 = 1;
            if (a_deref(c, &e) != 0 || (e.t != AML_V_INT && e.t != AML_V_STR && e.t != AML_V_BUF)) continue;
            for (int k = 0; k < 2; k++) {
                uint8_t m = k ? m2 : m1; aml_val_t o = k ? o2 : o1; int r, good;
                if (m == 0) { good = 1; }
                else {
                    aml_val_t e2 = e, oo = o;
                    if (cmp_vals(c, &e2, &oo, &r) != 0) { good = 0; }
                    else good = m == 1 ? r == 0 : m == 2 ? r <= 0 : m == 3 ? r < 0 : m == 4 ? r >= 0 : m == 5 ? r > 0 : 0;
                }
                if (k) ok2 = good; else ok1 = good;
            }
            if (ok1 && ok2) { found = i; break; }
        }
        *out = aml_int(found); return 0; }
    case 0x86: {                                                     /* Notify */
        tgt_t t; uint64_t v;
        NEED(cu, 1);
        if (is_name_lead(*cu->p)) {
            nm_t nm; aml_node_t* n;
            TRY(a_parse_name(cu, &nm)); TRY(a_lookup(c, f, f->scope, &nm, 1, &n));
            TRY(arg_int(c, f, cu, &v));
            if (c->ops.notify) c->ops.notify(c->ops.user, n, (uint32_t)v);
            return 0;
        }
        TRY(target(c, f, cu, &t)); TRY(arg_int(c, f, cu, &v));
        return 0; }
    case 0x84: return AML_E_UNSUP;                                   /* ConcatenateResTemplate */
    case 0xA0: {                                                     /* If */
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        cur_t sub = { cu->p, pe };
        uint64_t pred; TRY(arg_int(c, f, &sub, &pred));
        cu->p = pe;
        cur_t els = { 0, 0 };
        if (cu->p < cu->end && *cu->p == 0xA1) {
            cu->p++;
            const uint8_t* ee; TRY(a_pkg_len(cu, &ee));
            els.p = cu->p; els.end = ee; cu->p = ee;
        }
        if (pred) return a_exec_block(c, f, &sub);
        if (els.p) return a_exec_block(c, f, &els);
        return 0; }
    case 0xA2: {                                                     /* While */
        const uint8_t* pe; TRY(a_pkg_len(cu, &pe));
        const uint8_t* start = cu->p;
        cu->p = pe;
        for (;;) {
            cur_t sub = { start, pe };
            uint64_t pred; TRY(arg_int(c, f, &sub, &pred));
            if (!pred) break;
            int r = a_exec_block(c, f, &sub);
            if (r == C_BREAK) break;
            if (r != C_CONT && r) return r;
            if (++c->steps > c->max_steps) return AML_E_STEPS;
        }
        return 0; }
    case 0xA3: return 0;                                             /* Noop */
    case 0xCC: return 0;                                             /* BreakPoint */
    case 0xA4: { aml_val_t v; TRY(a_term(c, f, cu, &v)); f->ret = v; return C_RET; }
    case 0xA5: return C_BREAK;
    case 0x9F: return C_CONT;
    default: break;
    }
    cu->p--;
    return AML_FAIL(AML_E_PARSE);
}
