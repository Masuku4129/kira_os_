/* Интерпретатор AML, часть 1: арена, пространство имён, поля, вызов методов, внешний интерфейс. Часть 2 (термы) - aml_exec.c. */
#include "aml_int.h"

static void mcpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static void mset(void* d, int v, size_t n) { uint8_t* a = (uint8_t*)d; while (n--) *a++ = (uint8_t)v; }
static int seg_eq(const uint8_t* a, const uint8_t* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

/* ---------------- арена ---------------- */

aml_ctx_t* aml_create(void* mem, size_t size, const aml_ops_t* ops) {
    uintptr_t a = ((uintptr_t)mem + 15) & ~(uintptr_t)15;
    size_t lost = (size_t)(a - (uintptr_t)mem);
    if (!mem || !ops || size < lost + sizeof(aml_ctx_t) + 16384) return 0;
    aml_ctx_t* c = (aml_ctx_t*)a;
    mset(c, 0, sizeof *c);
    size -= lost;
    c->base = (uint8_t*)a;
    c->end = c->base + size;
    c->lo = c->base + ((sizeof(aml_ctx_t) + 15) & ~(size_t)15);
    c->hi = c->hi0 = (uint8_t*)((uintptr_t)c->end & ~(uintptr_t)15);
    c->ops = *ops;
    c->intmask = 0xFFFFFFFFull; c->intbits = 32;
    c->max_steps = AML_DEF_MAX_STEPS; c->max_tdepth = AML_DEF_MAX_TDEPTH; c->max_mdepth = AML_DEF_MAX_MDEPTH; c->max_sleep = AML_DEF_MAX_SLEEP;
    c->os = "Microsoft Windows NT"; c->rev = 2;
    size_t cap = size / 8; if (cap < 65536) cap = 65536;
    c->persist_cap = cap;
    aml_node_t* r = (aml_node_t*)a_alloc(c, sizeof *r, 1);
    if (!r) return 0;
    r->name[0] = '\\'; r->name[1] = r->name[2] = r->name[3] = '_'; r->type = AML_N_SCOPE;
    c->root = r; c->nodes = 1;
    /* Встроенные объекты: \_OS_, \_REV, \_OSI и \_GL_ - их ждёт любая прошивка. */
    static const uint8_t s_os[4] = { '_', 'O', 'S', '_' }, s_rev[4] = { '_', 'R', 'E', 'V' }, s_osi[4] = { '_', 'O', 'S', 'I' }, s_gl[4] = { '_', 'G', 'L', '_' };
    aml_node_t* n;
    if (a_new_node(c, 0, r, s_os, AML_N_NAME, &n) != 0) return 0;
    n->u.d.state = 3;           /* значение задаёт aml_set_os */
    if (a_new_node(c, 0, r, s_rev, AML_N_NAME, &n) != 0) return 0;
    n->u.d.state = 3;
    if (a_new_node(c, 0, r, s_osi, AML_N_METHOD, &n) != 0) return 0;
    n->u.m.argc = 1; n->u.m.native = 1;
    if (a_new_node(c, 0, r, s_gl, AML_N_MUTEX, &n) != 0) return 0;
    /* Предопределённые корневые области (ACPI 5.3.1): без них Scope(\_SB) в таблице некуда открыть. */
    static const char pre[5][4] = { { '_', 'S', 'B', '_' }, { '_', 'P', 'R', '_' }, { '_', 'G', 'P', 'E' }, { '_', 'T', 'Z', '_' }, { '_', 'S', 'I', '_' } };
    for (int i = 0; i < 5; i++) if (a_new_node(c, 0, r, (const uint8_t*)pre[i], AML_N_SCOPE, &n) != 0) return 0;
    aml_set_os(c, c->os, c->rev);
    return c;
}

void aml_set_limits(aml_ctx_t* c, uint32_t steps, uint32_t td, uint32_t md, uint32_t sl) {
    if (steps) c->max_steps = steps;
    if (td) c->max_tdepth = td > 1000 ? 1000 : td;
    if (md) c->max_mdepth = md > 200 ? 200 : md;
    if (sl) c->max_sleep = sl;
}

void aml_set_os(aml_ctx_t* c, const char* os, uint32_t rev) {
    static const uint8_t s_os[4] = { '_', 'O', 'S', '_' }, s_rev[4] = { '_', 'R', 'E', 'V' };
    c->os = os; c->rev = rev;
    aml_node_t* n = a_child(c->root, s_os);
    if (n) {
        uint32_t l = 0; while (os[l]) l++;
        uint8_t* b = (uint8_t*)a_alloc(c, l + 1, 1);
        if (!b) return;
        mcpy(b, os, l + 1);
        n->u.d.v.t = AML_V_STR; n->u.d.v.len = l; n->u.d.v.u.b = b; n->u.d.state = 0;
    }
    n = a_child(c->root, s_rev);
    if (n) { n->u.d.v.t = AML_V_INT; n->u.d.v.u.i = rev; n->u.d.state = 0; }
}

void* a_alloc(aml_ctx_t* c, size_t n, int persistent) {
    size_t avail = (size_t)(c->hi - c->lo);
    if (n > avail) return 0;
    size_t r = (n + 7) & ~(size_t)7;
    if (r > avail) return 0;
    if (persistent) {
        if (c->loaded && (size_t)(c->lo - c->lo_loaded) + r > c->persist_cap) return 0;
        void* p = c->lo; c->lo += r; mset(p, 0, r); return p;
    }
    c->hi -= r; mset(c->hi, 0, r); return c->hi;
}
int a_is_persistent(const aml_ctx_t* c, const void* p) { return (const uint8_t*)p >= c->base && (const uint8_t*)p < c->lo; }

void aml_load_done(aml_ctx_t* c) { c->loaded = 1; c->lo_loaded = c->lo; }

/* Глубокая копия значения: строки, буферы и пакеты получают свою память (постоянную или временную). */
int a_copy(aml_ctx_t* c, const aml_val_t* s, aml_val_t* d, int persistent, int depth) {
    if (depth > 16) return AML_E_DEPTH;
    aml_val_t t = *s;
    switch (s->t) {
    case AML_V_STR: case AML_V_BUF: {
        uint8_t* b = (uint8_t*)a_alloc(c, (size_t)s->len + 1, persistent);
        if (!b) return AML_E_NOMEM;
        if (s->len) mcpy(b, s->u.b, s->len);
        t.u.b = b; break; }
    case AML_V_PKG: {
        if (s->len > (1u << 20)) return AML_E_NOMEM;
        aml_val_t* e = (aml_val_t*)a_alloc(c, (size_t)s->len * sizeof(aml_val_t) + 8, persistent);
        if (!e) return AML_E_NOMEM;
        for (uint32_t i = 0; i < s->len; i++) TRY(a_copy(c, &s->u.p[i], &e[i], persistent, depth + 1));
        t.u.p = e; break; }
    case AML_V_REF:
        if (persistent && s->rk != RK_NODE) {                    /* ссылка на временное в постоянном месте не удержится: берём значение */
            aml_val_t tmp = *s;
            TRY(a_deref(c, &tmp));
            if (tmp.t == AML_V_REF) return AML_E_TYPE;
            return a_copy(c, &tmp, d, persistent, depth + 1);
        }
        break;
    default: break;
    }
    *d = t;
    return 0;
}

/* ---------------- имена ---------------- */

static int seg_ok(const uint8_t* s) {
    for (int i = 0; i < 4; i++) {
        uint8_t ch = s[i];
        int lead = (ch >= 'A' && ch <= 'Z') || ch == '_';
        if (!(lead || (i && ch >= '0' && ch <= '9'))) return 0;
    }
    return 1;
}

/* NameString: [\ | ^...] (NameSeg | Dual | Multi | Null). */
int a_parse_name(cur_t* cu, nm_t* nm) {
    nm->nseg = 0; nm->up = 0; nm->root = 0; nm->seg = 0;
    NEED(cu, 1);
    if (*cu->p == '\\') { nm->root = 1; cu->p++; }
    else while (cu->p < cu->end && *cu->p == '^') { if (nm->up == 255) return AML_FAIL(AML_E_PARSE); nm->up++; cu->p++; }
    NEED(cu, 1);
    uint8_t b = *cu->p;
    if (b == 0x00) { cu->p++; return 0; }
    unsigned n;
    if (b == 0x2E) { n = 2; cu->p++; }
    else if (b == 0x2F) { NEED(cu, 2); n = cu->p[1]; cu->p += 2; if (n == 0) return AML_FAIL(AML_E_PARSE); }
    else n = 1;
    if ((size_t)(cu->end - cu->p) / 4 < n) return AML_FAIL(AML_E_PARSE);
    nm->seg = cu->p; nm->nseg = (uint8_t)(n > 255 ? 255 : n);
    for (unsigned i = 0; i < n; i++) if (!seg_ok(cu->p + 4 * i)) return AML_FAIL(AML_E_PARSE);
    cu->p += 4 * n;
    return 0;
}

aml_node_t* a_child(const aml_node_t* par, const uint8_t* seg) {
    for (aml_node_t* n = par->child; n; n = n->next) if (seg_eq((const uint8_t*)n->name, seg)) return n;
    return 0;
}

int a_lookup(aml_ctx_t* c, frame_t* f, aml_node_t* scope, const nm_t* nm, int search, aml_node_t** out) {
    aml_node_t* s = nm->root ? c->root : scope;
    if (!s) s = c->root;
    for (unsigned i = 0; i < nm->up; i++) { if (!s->parent) return AML_E_NOTFOUND; s = s->parent; }
    if (nm->nseg == 0) { *out = s; return 0; }
    aml_node_t* cur = 0;
    unsigned i = 0;
    if (search && !nm->root && !nm->up) {
        const uint8_t* sg = nm->seg;
        if (f) for (aml_node_t* d = f->dyn; d && !cur; d = d->next) if (seg_eq((const uint8_t*)d->name, sg)) cur = d;
        for (aml_node_t* p = s; p && !cur; p = p->parent) cur = a_child(p, sg);
        if (!cur) return AML_E_NOTFOUND;
        i = 1;
    } else cur = s;
    for (; i < nm->nseg; i++) {
        if (cur->type == AML_N_ALIAS && cur->u.alias) cur = cur->u.alias;
        cur = a_child(cur, nm->seg + 4 * i);
        if (!cur) return AML_E_NOTFOUND;
    }
    *out = cur;
    return 0;
}

/* Создать объект. В режиме загрузки - постоянный и вписан в дерево (повтор имени: 1 и существующий узел в *out). Внутри метода - временный, виден только ему. */
int a_new_node(aml_ctx_t* c, frame_t* f, aml_node_t* parent, const uint8_t* seg, int type, aml_node_t** out) {
    int persistent = !f || f->loading;
    if (persistent) {
        aml_node_t* ex = a_child(parent, seg);
        if (ex) { c->dups++; *out = ex; return 1; }
    }
    aml_node_t* n = (aml_node_t*)a_alloc(c, sizeof *n, persistent);
    if (!n) return AML_E_NOMEM;
    n->name[0] = (char)seg[0]; n->name[1] = (char)seg[1]; n->name[2] = (char)seg[2]; n->name[3] = (char)seg[3];
    n->type = (uint8_t)type; n->parent = parent;
    if (persistent) {
        if (parent->last) parent->last->next = n; else parent->child = n;
        parent->last = n;
        c->nodes++;
    } else { n->next = f->dyn; f->dyn = n; }
    *out = n;
    return 0;
}

/* ---------------- PkgLength ---------------- */

int a_pkg_len(cur_t* cu, const uint8_t** pkg_end) {
    NEED(cu, 1);
    const uint8_t* start = cu->p;
    uint8_t b = *cu->p++;
    unsigned n = b >> 6;
    uint32_t len;
    if (n == 0) len = b & 0x3F;
    else { NEED(cu, n); len = b & 0x0F; for (unsigned i = 0; i < n; i++) len |= (uint32_t)cu->p[i] << (4 + 8 * i); cu->p += n; }
    if (len < 1 + n || len > (size_t)(cu->end - start)) return AML_FAIL(AML_E_PARSE);
    *pkg_end = start + len;
    return 0;
}
int a_field_raw_len(cur_t* cu, uint32_t* bits) {
    NEED(cu, 1);
    uint8_t b = *cu->p++;
    unsigned n = b >> 6;
    uint32_t len;
    if (n == 0) len = b & 0x3F;
    else { NEED(cu, n); len = b & 0x0F; for (unsigned i = 0; i < n; i++) len |= (uint32_t)cu->p[i] << (4 + 8 * i); cu->p += n; }
    *bits = len;
    return 0;
}

/* ---------------- значения ---------------- */

aml_val_t aml_int(uint64_t v) { aml_val_t r; mset(&r, 0, sizeof r); r.t = AML_V_INT; r.u.i = v; return r; }

int a_bytes_of(const aml_val_t* v, const uint8_t** p, uint32_t* n) {
    if (v->t == AML_V_BUF || v->t == AML_V_STR) { *p = v->u.b; *n = v->len; return 0; }
    return AML_E_TYPE;
}

/* Развернуть ссылки до значения. */
int a_deref(aml_ctx_t* c, aml_val_t* v) {
    for (int guard = 0; v->t == AML_V_REF; guard++) {
        if (guard > 8) return AML_E_DEPTH;
        aml_val_t r;
        switch (v->rk) {
        case RK_SLOT: if (!v->u.p) return AML_E_TYPE; r = *v->u.p; break;
        case RK_BYTE: r = aml_int(*v->u.b); break;
        case RK_NODE: {
            aml_node_t* n = v->u.n;
            if (n->type == AML_N_DEVICE || n->type == AML_N_SCOPE || n->type == AML_N_PROC || n->type == AML_N_THERMAL ||
                n->type == AML_N_POWERRES || n->type == AML_N_MUTEX || n->type == AML_N_EVENT || n->type == AML_N_REGION) return 0;   /* объект без значения: остаётся ссылкой */
            TRY(a_call_node(c, n, 0, 0, &r));
            break; }
        default: return AML_E_TYPE;
        }
        *v = r;
    }
    return 0;
}

int a_to_int(aml_ctx_t* c, const aml_val_t* in, uint64_t* out) {
    aml_val_t v = *in;
    TRY(a_deref(c, &v));
    switch (v.t) {
    case AML_V_INT: *out = v.u.i & c->intmask; return 0;
    case AML_V_BUF: { uint64_t r = 0; for (uint32_t i = 0; i < v.len && i < 8; i++) r |= (uint64_t)v.u.b[i] << (8 * i); *out = r & c->intmask; return 0; }
    case AML_V_STR: {                                            /* строка -> число: шестнадцатеричная запись (как у ToInteger) */
        uint64_t r = 0; uint32_t i = 0;
        if (v.len >= 2 && v.u.b[0] == '0' && (v.u.b[1] == 'x' || v.u.b[1] == 'X')) i = 2;
        for (; i < v.len; i++) {
            uint8_t ch = v.u.b[i]; unsigned d;
            if (ch >= '0' && ch <= '9') d = ch - '0'; else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10; else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10; else break;
            r = r << 4 | d;
        }
        *out = r & c->intmask; return 0; }
    default: return AML_E_TYPE;
    }
}

int aml_val_int(const aml_val_t* v, uint64_t* out) {
    if (v->t == AML_V_INT) { *out = v->u.i; return 0; }
    if (v->t == AML_V_BUF) { uint64_t r = 0; for (uint32_t i = 0; i < v->len && i < 8; i++) r |= (uint64_t)v->u.b[i] << (8 * i); *out = r; return 0; }
    return AML_E_TYPE;
}
const aml_val_t* aml_pkg_at(const aml_val_t* v, unsigned i) { return (v && v->t == AML_V_PKG && i < v->len) ? &v->u.p[i] : 0; }
int aml_val_node(const aml_val_t* v, aml_node_t** n) {
    if (v && v->t == AML_V_REF && v->rk == RK_NODE) { *n = v->u.n; return 0; }
    return AML_E_TYPE;
}

void aml_eisaid(uint32_t id, char out[8]) {
    uint32_t b0 = id & 0xFF, b1 = (id >> 8) & 0xFF, b2 = (id >> 16) & 0xFF, b3 = (id >> 24) & 0xFF;
    uint32_t m = b0 << 8 | b1;
    static const char hx[] = "0123456789ABCDEF";
    out[0] = (char)(((m >> 10) & 31) + 0x40); out[1] = (char)(((m >> 5) & 31) + 0x40); out[2] = (char)((m & 31) + 0x40);
    out[3] = hx[b2 >> 4]; out[4] = hx[b2 & 15]; out[5] = hx[b3 >> 4]; out[6] = hx[b3 & 15]; out[7] = 0;
}

static int str_is(const aml_val_t* v, const char* s) {
    uint32_t i = 0;
    if (v->t == AML_V_INT) {
        char id[8]; aml_eisaid((uint32_t)v->u.i, id);
        for (; s[i]; i++) if (id[i] != s[i]) return 0;
        return id[i] == 0;
    }
    if (v->t != AML_V_STR) return 0;
    for (; s[i]; i++) if (i >= v->len || v->u.b[i] != (uint8_t)s[i]) return 0;
    return i == v->len;
}

/* Вызов дочернего объекта по имени из 4 знаков (внутри уже идущего вычисления: арена не сбрасывается). */
static int child_eval(aml_ctx_t* c, aml_node_t* dev, const char* nm, aml_val_t* out) {
    uint8_t seg[4] = { (uint8_t)nm[0], (uint8_t)nm[1], (uint8_t)nm[2], (uint8_t)nm[3] };
    aml_node_t* n = a_child(dev, seg);
    if (!n) return AML_E_NOTFOUND;
    TRY(a_call_node(c, n, 0, 0, out));
    (void)a_deref(c, out);
    return 0;
}

static int hid_match_in(aml_ctx_t* c, aml_node_t* dev, const char* id) {
    aml_val_t v;
    if (child_eval(c, dev, "_HID", &v) == 0 && str_is(&v, id)) return 1;
    if (child_eval(c, dev, "_CID", &v) == 0) {
        if (v.t == AML_V_PKG) { for (uint32_t i = 0; i < v.len; i++) if (str_is(&v.u.p[i], id)) return 1; }
        else if (str_is(&v, id)) return 1;
    }
    return 0;
}

int aml_hid_match(aml_ctx_t* c, aml_node_t* dev, const char* id) {
    if (!c || !dev || c->busy) return 0;
    c->busy = 1; c->hi = c->hi0; c->steps = 0; c->tdepth = 0; c->mdepth = 0; c->slept = 0; c->fdepth = 0;
    int r = hid_match_in(c, dev, id);
    c->steps_last = c->steps; c->busy = 0;
    return r;
}

/* ---------------- операционные области и поля ---------------- */

static int region_init(aml_ctx_t* c, aml_node_t* r) {
    if (r->u.r.ready) return 0;
    uint64_t off = r->u.r.off, len = r->u.r.len;
    frame_t fr; mset(&fr, 0, sizeof fr); fr.scope = r->parent;
    aml_val_t v;
    if (r->u.r.off_pc) { cur_t cu = { r->u.r.off_pc, r->u.r.end }; TRY(a_term(c, &fr, &cu, &v)); TRY(a_to_int(c, &v, &off)); }
    if (r->u.r.len_pc) { cur_t cu = { r->u.r.len_pc, r->u.r.end }; TRY(a_term(c, &fr, &cu, &v)); TRY(a_to_int(c, &v, &len)); }
    r->u.r.off = off; r->u.r.len = len; r->u.r.ready = 1;
    return 0;
}

static int pci_loc(aml_ctx_t* c, aml_node_t* dev, uint32_t* out, int depth);
static int pci_bus_of(aml_ctx_t* c, aml_node_t* from, uint32_t* seg, uint32_t* bus, int depth) {
    for (aml_node_t* p = from; p; p = p->parent) {
        if (p->type == AML_N_DEVICE && (hid_match_in(c, p, "PNP0A03") || hid_match_in(c, p, "PNP0A08"))) {
            uint64_t v = 0;
            *bus = 0; *seg = 0;
            { aml_val_t t; if (child_eval(c, p, "_BBN", &t) == 0 && aml_val_int(&t, &v) == 0) *bus = (uint32_t)v & 0xFF; }
            { aml_val_t t; if (child_eval(c, p, "_SEG", &t) == 0 && aml_val_int(&t, &v) == 0) *seg = (uint32_t)v & 0xFFFF; }
            return 0;
        }
        if (p->type == AML_N_DEVICE && a_child(p, (const uint8_t*)"_ADR") && p != from) {   /* мост PCI-PCI: вторичная шина - в его конфигурации */
            if (depth > 6) return AML_E_DEPTH;
            uint32_t loc; TRY(pci_loc(c, p, &loc, depth + 1));
            uint64_t sec = 0;
            if (!c->ops.rw || c->ops.rw(c->ops.user, 0, AML_SP_PCI, 0x19, 8, &sec, loc) != 0) return AML_E_HW;
            *seg = loc >> 16; *bus = (uint32_t)sec & 0xFF;
            return 0;
        }
    }
    *seg = 0; *bus = 0;
    return 0;
}
/* Адрес устройства PCI: seg<<16 | bus<<8 | dev<<3 | fn - по _ADR устройства и шине его корневого моста. */
static int pci_loc(aml_ctx_t* c, aml_node_t* dev, uint32_t* out, int depth) {
    uint64_t adr = 0; aml_val_t t;
    if (dev->type == AML_N_DEVICE && child_eval(c, dev, "_ADR", &t) == 0) (void)aml_val_int(&t, &adr);
    uint32_t seg, bus;
    TRY(pci_bus_of(c, dev->parent, &seg, &bus, depth));
    *out = seg << 16 | bus << 8 | (uint32_t)(((adr >> 16) & 0x1F) << 3) | (uint32_t)(adr & 7);
    return 0;
}

/* Один доступ к области: byte_off - смещение в области в байтах, bits - ширина. */
static int region_unit(aml_ctx_t* c, aml_node_t* r, uint64_t byte_off, unsigned bits, int write, uint64_t* val) {
    TRY(region_init(c, r));
    if (byte_off > r->u.r.len || (uint64_t)(bits / 8) > r->u.r.len - byte_off) return AML_E_BOUNDS;
    unsigned sp = r->u.r.space;
    if (sp == AML_SP_PCI && !r->u.r.pci_ready) { TRY(pci_loc(c, r->parent, &r->u.r.pci, 0)); r->u.r.pci_ready = 1; }
    if (sp == AML_SP_SMBUS || sp > AML_SP_CMOS || !c->ops.rw) return AML_E_UNSUP;
    if (c->ops.rw(c->ops.user, write, sp, r->u.r.off + byte_off, bits, val, sp == AML_SP_PCI ? r->u.r.pci : 0) != 0) return AML_E_HW;
    return 0;
}

static int field_write_int(aml_ctx_t* c, aml_node_t* n, uint64_t v);
static int field_read_int(aml_ctx_t* c, aml_node_t* n, uint64_t* v);

/* Единица доступа поля: номер unit при ширине w бит. */
static int field_unit(aml_ctx_t* c, aml_node_t* n, uint32_t unit, unsigned w, int write, uint64_t* val) {
    if (c->fdepth > 4) return AML_E_DEPTH;
    c->fdepth++;
    int r;
    uint64_t byte_off = (uint64_t)unit * (w / 8);
    if (n->u.f.kind == FK_INDEX) {
        r = field_write_int(c, n->u.f.idx, byte_off);
        if (!r) r = write ? field_write_int(c, n->u.f.data, *val) : field_read_int(c, n->u.f.data, val);
    } else {
        r = 0;
        if (n->u.f.kind == FK_BANK) {
            if (!n->u.f.bankready) {
                frame_t fr; mset(&fr, 0, sizeof fr); fr.scope = n->parent;
                aml_val_t v; cur_t cu = { n->u.f.bankpc, n->u.f.bankend }; uint64_t bv = 0;
                r = a_term(c, &fr, &cu, &v);
                if (!r) r = a_to_int(c, &v, &bv);
                if (!r) { n->u.f.bankval = bv; n->u.f.bankready = 1; }
            }
            if (!r) r = field_write_int(c, n->u.f.bank, n->u.f.bankval);
        }
        if (!r) r = region_unit(c, n->u.f.region, byte_off, w, write, val);
    }
    c->fdepth--;
    return r;
}

static unsigned field_width(const aml_node_t* n, unsigned space) {
    static const unsigned wt[6] = { 8, 8, 16, 32, 64, 8 };
    unsigned w = wt[n->u.f.access > 5 ? 0 : n->u.f.access];
    if (space == AML_SP_EC) w = 8;
    else if ((space == AML_SP_IO || space == AML_SP_PCI) && w > 32) w = 32;
    return w;
}

static void bits_copy(uint8_t* dst, uint32_t doff, const uint8_t* src, uint32_t soff, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = soff + i, d = doff + i;
        unsigned bit = (src[s >> 3] >> (s & 7)) & 1;
        if (bit) dst[d >> 3] |= (uint8_t)(1u << (d & 7)); else dst[d >> 3] &= (uint8_t)~(1u << (d & 7));
    }
}

static unsigned field_space(aml_ctx_t* c, aml_node_t* n) {
    for (int g = 0; g < 4 && n->u.f.kind == FK_INDEX; g++) n = n->u.f.data;       /* у IndexField ширина - по полю данных */
    (void)c;
    return n->u.f.kind == FK_INDEX || !n->u.f.region ? AML_SP_MEM : n->u.f.region->u.r.space;
}

#define AML_FIELD_MAX_BITS 65536u

/* Прочитать биты поля в буфер out (младшим байтом вперёд). */
static int field_bits_read(aml_ctx_t* c, aml_node_t* n, uint8_t* out) {
    unsigned w = field_width(n, field_space(c, n));
    uint32_t B = n->u.f.bitoff, N = n->u.f.bitlen;
    if (N == 0 || N > AML_FIELD_MAX_BITS) return AML_E_BOUNDS;
    uint32_t u0 = B / w, u1 = (B + N - 1) / w;
    c->steps += N / 64;
    for (uint32_t u = u0; u <= u1; u++) {
        uint64_t v = 0;
        TRY(field_unit(c, n, u, w, 0, &v));
        uint32_t us = u * w, from = B > us ? B : us, to = (B + N) < (us + w) ? (B + N) : (us + w);
        uint8_t tmp[8]; for (int i = 0; i < 8; i++) tmp[i] = (uint8_t)(v >> (8 * i));
        bits_copy(out, from - B, tmp, from - us, to - from);
    }
    return 0;
}
static int field_bits_write(aml_ctx_t* c, aml_node_t* n, const uint8_t* in) {
    unsigned w = field_width(n, field_space(c, n));
    uint32_t B = n->u.f.bitoff, N = n->u.f.bitlen;
    if (N == 0 || N > AML_FIELD_MAX_BITS) return AML_E_BOUNDS;
    uint32_t u0 = B / w, u1 = (B + N - 1) / w;
    c->steps += N / 64;
    for (uint32_t u = u0; u <= u1; u++) {
        uint32_t us = u * w, from = B > us ? B : us, to = (B + N) < (us + w) ? (B + N) : (us + w);
        uint64_t v = 0;
        if (to - from < w) {                                                       /* единица задета не целиком: правило обновления */
            if (n->u.f.update == 0) TRY(field_unit(c, n, u, w, 0, &v));
            else if (n->u.f.update == 1) v = ~0ull;
        }
        uint8_t tmp[8]; for (int i = 0; i < 8; i++) tmp[i] = (uint8_t)(v >> (8 * i));
        bits_copy(tmp, from - us, in, from - B, to - from);
        v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)tmp[i] << (8 * i);
        if (w < 64) v &= (1ull << w) - 1;
        TRY(field_unit(c, n, u, w, 1, &v));
    }
    return 0;
}

static int field_read_int(aml_ctx_t* c, aml_node_t* n, uint64_t* v) {
    if (n->type != AML_N_FIELD || n->u.f.bitlen > 64) return AML_E_TYPE;
    uint8_t b[8] = { 0 }; TRY(field_bits_read(c, n, b));
    uint64_t r = 0; for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (8 * i);
    *v = r; return 0;
}
static int field_write_int(aml_ctx_t* c, aml_node_t* n, uint64_t v) {
    if (n->type != AML_N_FIELD) return AML_E_TYPE;
    uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    if (n->u.f.bitlen > 64) return AML_E_TYPE;
    return field_bits_write(c, n, b);
}

static int val_to_bytes(aml_ctx_t* c, const aml_val_t* in, uint8_t* dst, uint32_t cap) {
    aml_val_t v = *in; TRY(a_deref(c, &v));
    for (uint32_t i = 0; i < cap; i++) dst[i] = 0;
    if (v.t == AML_V_INT) { for (uint32_t i = 0; i < cap && i < 8; i++) dst[i] = (uint8_t)(v.u.i >> (8 * i)); return 0; }
    if (v.t == AML_V_BUF || v.t == AML_V_STR) { for (uint32_t i = 0; i < cap && i < v.len; i++) dst[i] = v.u.b[i]; return 0; }
    return AML_E_TYPE;
}

static int bufield_read(aml_ctx_t* c, aml_node_t* n, aml_val_t* out) {
    uint32_t B = n->u.bf.bitoff, N = n->u.bf.bitlen;
    if (N == 0 || (uint64_t)B + N > (uint64_t)n->u.bf.datalen * 8) return AML_E_BOUNDS;
    uint32_t nb = (N + 7) / 8;
    if (N <= c->intbits) {
        uint8_t tmp[8] = { 0 }; bits_copy(tmp, 0, n->u.bf.data, B, N);
        uint64_t r = 0; for (int i = 0; i < 8; i++) r |= (uint64_t)tmp[i] << (8 * i);
        *out = aml_int(r); return 0;
    }
    uint8_t* b = (uint8_t*)a_alloc(c, nb + 1, 0);
    if (!b) return AML_E_NOMEM;
    bits_copy(b, 0, n->u.bf.data, B, N);
    mset(out, 0, sizeof *out); out->t = AML_V_BUF; out->len = nb; out->u.b = b;
    return 0;
}
static int bufield_write(aml_ctx_t* c, aml_node_t* n, const aml_val_t* v) {
    uint32_t B = n->u.bf.bitoff, N = n->u.bf.bitlen;
    if (N == 0 || (uint64_t)B + N > (uint64_t)n->u.bf.datalen * 8) return AML_E_BOUNDS;
    uint32_t nb = (N + 7) / 8;
    uint8_t* tmp = (uint8_t*)a_alloc(c, nb + 8, 0);
    if (!tmp) return AML_E_NOMEM;
    TRY(val_to_bytes(c, v, tmp, nb));
    bits_copy(n->u.bf.data, B, tmp, 0, N);
    return 0;
}

/* Чтение значения объекта пространства (без вызова метода с аргументами). */
int a_name_force(aml_ctx_t* c, aml_node_t* n) {
    if (n->u.d.state == 0) return 0;
    if (n->u.d.state == 2) return AML_E_DEPTH;                  /* имя определено через само себя */
    if (n->u.d.state == 3) return AML_FAIL(AML_E_PARSE);
    n->u.d.state = 2;
    frame_t fr; mset(&fr, 0, sizeof fr); fr.scope = n->parent;
    cur_t cu = { n->u.d.lazy, n->u.d.lend };
    aml_val_t v; mset(&v, 0, sizeof v);
    int r = a_term(c, &fr, &cu, &v);
    if (!r) r = a_deref(c, &v);
    aml_val_t p;
    if (!r) r = a_copy(c, &v, &p, 1, 0);
    if (r) { n->u.d.state = 3; return r; }
    n->u.d.v = p; n->u.d.state = 0;
    return 0;
}

int a_node_read(aml_ctx_t* c, aml_node_t* n, aml_val_t* out) {
    switch (n->type) {
    case AML_N_NAME: TRY(a_name_force(c, n)); *out = n->u.d.v; return 0;
    case AML_N_FIELD: {
        if (n->u.f.bitlen == 0 || n->u.f.bitlen > AML_FIELD_MAX_BITS) return AML_E_BOUNDS;
        if (n->u.f.bitlen <= c->intbits) { uint64_t v; TRY(field_read_int(c, n, &v)); *out = aml_int(v & c->intmask); return 0; }
        uint32_t nb = (n->u.f.bitlen + 7) / 8;
        uint8_t* b = (uint8_t*)a_alloc(c, nb + 1, 0);
        if (!b) return AML_E_NOMEM;
        TRY(field_bits_read(c, n, b));
        mset(out, 0, sizeof *out); out->t = AML_V_BUF; out->len = nb; out->u.b = b;
        return 0; }
    case AML_N_BUFFIELD: return bufield_read(c, n, out);
    case AML_N_ALIAS: if (!n->u.alias) return AML_E_NOTFOUND; return a_node_read(c, n->u.alias, out);
    default:
        mset(out, 0, sizeof *out); out->t = AML_V_REF; out->rk = RK_NODE; out->u.n = n; return 0;
    }
}

/* Запись в именованный объект: число, строка, буфер или пакет приводятся к типу того, что там лежит. */
int a_node_write(aml_ctx_t* c, aml_node_t* n, const aml_val_t* in) {
    aml_val_t v = *in;
    switch (n->type) {
    case AML_N_ALIAS: if (!n->u.alias) return AML_E_NOTFOUND; return a_node_write(c, n->u.alias, in);
    case AML_N_FIELD:
        TRY(a_deref(c, &v));
        if (n->u.f.bitlen <= 64) { uint64_t x; TRY(a_to_int(c, &v, &x)); return field_write_int(c, n, x); }
        else {
            uint32_t nb = (n->u.f.bitlen + 7) / 8;
            if (n->u.f.bitlen > AML_FIELD_MAX_BITS) return AML_E_BOUNDS;
            uint8_t* tmp = (uint8_t*)a_alloc(c, nb + 8, 0);
            if (!tmp) return AML_E_NOMEM;
            TRY(val_to_bytes(c, &v, tmp, nb));
            return field_bits_write(c, n, tmp);
        }
    case AML_N_BUFFIELD: return bufield_write(c, n, &v);
    case AML_N_NAME: {
        TRY(a_name_force(c, n));
        TRY(a_deref(c, &v));
        aml_val_t* d = &n->u.d.v;
        if (d->t == AML_V_INT) { uint64_t x; TRY(a_to_int(c, &v, &x)); d->u.i = x; return 0; }
        if (d->t == AML_V_BUF && (v.t == AML_V_INT || v.t == AML_V_BUF || v.t == AML_V_STR)) return val_to_bytes(c, &v, d->u.b, d->len);   /* буфер сохраняет размер */
        if (d->t == AML_V_STR && v.t == AML_V_STR && v.len <= d->len) { for (uint32_t i = 0; i < v.len; i++) d->u.b[i] = v.u.b[i]; d->u.b[v.len] = 0; d->len = v.len; return 0; }
        aml_val_t p;
        TRY(a_copy(c, &v, &p, 1, 0));
        *d = p;
        return 0; }
    default: return AML_E_TYPE;
    }
}

/* ---------------- вызов метода ---------------- */

/* Встроенный метод: сейчас только \_OSI. Отвечает "да" на то, что мы и правда умеем (без выдуманных возможностей Windows). */
int a_native_call(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned argc, aml_val_t* out) {
    (void)n;
    if (argc < 1 || args[0].t != AML_V_STR) return AML_E_ARGS;
    static const char* const ok[] = { "Module Device", "Processor Device", "3.0 _SCP Extensions", "Processor Aggregator Device", "3.0 Thermal Model" };
    uint64_t yes = 0;
    for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++) if (str_is(&args[0], ok[k])) yes = 1;
    *out = aml_int(yes ? c->intmask : 0);
    return 0;
}

int a_call_node(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned argc, aml_val_t* out) {
    if (n->type == AML_N_ALIAS) { if (!n->u.alias) return AML_E_NOTFOUND; n = n->u.alias; }
    if (n->type != AML_N_METHOD) { if (argc) return AML_E_ARGS; return a_node_read(c, n, out); }
    if (argc != n->u.m.argc) return AML_E_ARGS;
    if (n->u.m.native) return a_native_call(c, n, args, argc, out);
    if (c->mdepth >= c->max_mdepth) return AML_E_DEPTH;
    size_t sz = sizeof(frame_t) + (AML_MAX_ARGS + AML_MAX_LOCALS) * sizeof(aml_val_t);
    frame_t* fr = (frame_t*)a_alloc(c, sz, 0);
    if (!fr) return AML_E_NOMEM;
    fr->args = (aml_val_t*)(fr + 1); fr->loc = fr->args + AML_MAX_ARGS;
    fr->scope = n; fr->method = n; fr->nargs = (uint8_t)argc;
    for (unsigned i = 0; i < argc && i < AML_MAX_ARGS; i++) fr->args[i] = args[i];
    cur_t cu = { n->u.m.body, n->u.m.end };
    c->mdepth++;
    int r = a_exec_block(c, fr, &cu);
    c->mdepth--;
    mset(out, 0, sizeof *out);
    if (r == C_RET) { *out = fr->ret; r = 0; }
    else if (r == C_BREAK || r == C_CONT) r = AML_E_PARSE;
    return r;
}

/* ---------------- загрузка таблицы ---------------- */

int aml_load(aml_ctx_t* c, const uint8_t* t, uint32_t len) {
    if (c->busy) return AML_E_BUSY;
    if (!t || len < 36) return AML_E_TABLE;
    uint32_t tl = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
    if (tl < 36 || tl > len) return AML_E_TABLE;
    uint8_t sum = 0; for (uint32_t i = 0; i < tl; i++) sum = (uint8_t)(sum + t[i]);
    if (sum != 0) return AML_E_TABLE;
    if (!((t[0] == 'D' && t[1] == 'S' && t[2] == 'D' && t[3] == 'T') || (t[0] == 'S' && t[1] == 'S' && t[2] == 'D' && t[3] == 'T') || (t[0] == 'P' && t[1] == 'S' && t[2] == 'D' && t[3] == 'T'))) return AML_E_TABLE;
    if (t[0] == 'D') {                                                       /* ревизия DSDT < 2: числа 32-битные */
        if (t[8] >= 2) { c->intmask = ~0ull; c->intbits = 64; } else { c->intmask = 0xFFFFFFFFull; c->intbits = 32; }
    }
    c->busy = 1;
    c->steps = 0; c->tdepth = 0; c->mdepth = 0; c->slept = 0; c->fdepth = 0;
    uint8_t* hi_save = c->hi;
    frame_t fr; mset(&fr, 0, sizeof fr); fr.scope = c->root; fr.loading = 1;
    cur_t cu = { t + 36, t + tl };
    int r = a_exec_block(c, &fr, &cu);
    c->hi = hi_save; c->busy = 0;
    if (r) { c->load_errors++; return r > 0 ? AML_E_PARSE : r; }
    c->tables++;
    return 0;
}

/* ---------------- внешний интерфейс ---------------- */

aml_node_t* aml_root(aml_ctx_t* c) { return c->root; }
aml_node_t* aml_first_child(const aml_node_t* n) { return n->child; }
aml_node_t* aml_next_sibling(const aml_node_t* n) { return n->next; }
aml_node_t* aml_parent(const aml_node_t* n) { return n->parent; }
aml_ntype_t aml_node_type(const aml_node_t* n) { return (aml_ntype_t)n->type; }
void aml_node_name(const aml_node_t* n, char out[5]) { for (int i = 0; i < 4; i++) out[i] = n->name[i]; out[4] = 0; }
unsigned aml_method_argc(const aml_node_t* n) { return n->type == AML_N_METHOD ? n->u.m.argc : 0; }

int aml_node_path(const aml_node_t* n, char* buf, unsigned cap) {
    const aml_node_t* st[64]; int d = 0;
    for (const aml_node_t* p = n; p && p->parent; p = p->parent) { if (d >= 64) return AML_E_DEPTH; st[d++] = p; }
    unsigned o = 0;
    if (o + 1 >= cap) return AML_E_NOMEM;
    buf[o++] = '\\';
    for (int i = d - 1; i >= 0; i--) {
        if (o + 5 >= cap) return AML_E_NOMEM;
        for (int k = 0; k < 4; k++) buf[o++] = st[i]->name[k];
        if (i) buf[o++] = '.';
    }
    buf[o] = 0;
    return (int)o;
}

/* Текстовый путь -> узел. Короткие сегменты дополняются '_'. */
aml_node_t* aml_lookup(aml_ctx_t* c, aml_node_t* scope, const char* path) {
    uint8_t segs[16 * 4]; nm_t nm; nm.nseg = 0; nm.up = 0; nm.root = 0; nm.seg = segs;
    const char* p = path;
    if (*p == '\\') { nm.root = 1; p++; } else while (*p == '^') { nm.up++; p++; }
    while (*p) {
        if (nm.nseg >= 16) return 0;
        int k = 0;
        while (*p && *p != '.') { if (k < 4) segs[nm.nseg * 4 + k] = (uint8_t)*p; k++; p++; }
        if (k == 0 || k > 4) return 0;
        for (int j = k; j < 4; j++) segs[nm.nseg * 4 + j] = '_';
        nm.nseg++;
        if (*p == '.') p++;
    }
    aml_node_t* r;
    if (a_lookup(c, 0, scope ? scope : c->root, &nm, 0, &r) != 0) return 0;
    return r;
}

static int begin_call(aml_ctx_t* c) {
    if (c->busy) return AML_E_BUSY;
    c->busy = 1;
    c->hi = c->hi0;                                 /* всё временное от прошлого вызова отброшено */
    c->steps = 0; c->tdepth = 0; c->mdepth = 0; c->slept = 0; c->fdepth = 0;
    return 0;
}
static void end_call(aml_ctx_t* c) { c->steps_last = c->steps; c->busy = 0; }

int aml_call(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned nargs, aml_val_t* out) {
    if (!c || !n || !out) return AML_E_ARGS;
    if (nargs > AML_MAX_ARGS) return AML_E_ARGS;
    TRY(begin_call(c));
    mset(out, 0, sizeof *out);
    int r = a_call_node(c, n, args, nargs, out);
    if (!r) (void)a_deref(c, out);                  /* ссылку на значение раскрываем, на объект - оставляем */
    end_call(c);
    return r > 0 ? AML_E_PARSE : r;
}
int aml_call_path(aml_ctx_t* c, aml_node_t* scope, const char* path, const aml_val_t* args, unsigned nargs, aml_val_t* out) {
    if (c->busy) return AML_E_BUSY;
    aml_node_t* n = aml_lookup(c, scope, path);
    if (!n) return AML_E_NOTFOUND;
    return aml_call(c, n, args, nargs, out);
}
int aml_call_int(aml_ctx_t* c, aml_node_t* scope, const char* path, uint64_t* out) {
    aml_val_t v;
    TRY(aml_call_path(c, scope, path, 0, 0, &v));
    return aml_val_int(&v, out);
}

void aml_stats(const aml_ctx_t* c, aml_stats_t* s) {
    s->nodes = c->nodes; s->arena_used = (uint32_t)(c->lo - c->base); s->arena_size = (uint32_t)(c->end - c->base);
    s->persist_free = c->loaded ? (uint32_t)(c->persist_cap - (size_t)(c->lo - c->lo_loaded)) : 0;
    s->steps_last = c->steps_last; s->dup_names = c->dups; s->load_errors = c->load_errors; s->loaded_tables = c->tables;
}

static int walk_rec(aml_node_t* n, int d, int (*fn)(aml_node_t*, int, void*), void* u) {
    if (d > 64) return 0;
    for (aml_node_t* k = n->child; k; k = k->next) {
        if (fn(k, d, u)) return 1;
        if (walk_rec(k, d + 1, fn, u)) return 1;
    }
    return 0;
}
int aml_walk(aml_ctx_t* c, int (*fn)(aml_node_t*, int, void*), void* u) { return walk_rec(c->root, 0, fn, u); }
