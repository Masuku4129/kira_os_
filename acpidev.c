/* ACPI в работе: события SCI/GPE, встроенный контроллер, батарея, сеть питания, крышка, температура, _PRT. См. acpidev.h. */
#include "acpidev.h"

#define ACPI_MAX_SSDT_ 64

#define PM1_PWRBTN  (1u << 8)
#define PM1_SLPBTN  (1u << 9)
#define FADT_PWR_BUTTON_AML (1u << 4)   /* кнопка питания - не фиксированная, а устройство PNP0C0C с Notify */
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI 0x20

enum { K_BAT = 1, K_AC, K_LID, K_PWRB, K_SLPB, K_EC, K_ROOT };
#define MAX_DEV 48
#define MAX_BAT 4
#define NQ 32

typedef struct { aml_node_t* n; uint8_t kind; } dev_t_;
typedef struct { aml_node_t* n; int present, info_ok; uint32_t design, last_full; } bat_t;

static struct {
    aml_ctx_t* ctx;
    aml_ops_t uops;
    acpidev_hw_t hw;
    int ready, apic_known;
    void (*runner)(void (*)(void*), void*);
    void (*hook)(int, int, void*); void* hook_user;
    dev_t_ dev[MAX_DEV]; unsigned ndev;
    bat_t bat[MAX_BAT]; unsigned nbat;
    aml_node_t* therm[8]; unsigned ntherm;
    aml_node_t* gpe_scope;
    int ec_ok; uint16_t ec_cmd, ec_data; aml_node_t* ec_node; int ec_gpe;
    struct { aml_node_t* n; uint32_t v; } q[NQ]; unsigned qh, qt;
    struct { int ev, arg; } evq[16]; unsigned eh, et;
    volatile uint32_t sci_flag;
    int power_dirty; uint64_t last_service_ms, last_power_ms; int inited_time;
    acpidev_power_t pw;
    acpidev_stats_t st;
    int fixed_pwr_enabled;
    uint8_t warned_gpe[32];
} S;

/* ---------------- мелочи ---------------- */

/* Одновременно - один вызов AML: аргументы заданий лежат в общих переменных, а AML может уснуть (Sleep), и тогда в модуль способен
   зайти другой поток ядра. Второй вход не ждёт, а получает отказ (занято) - вызывающий повторит позже или обойдётся без ответа. */
static int g_gate;
static int gate_take(void) { return !__atomic_exchange_n(&g_gate, 1, __ATOMIC_ACQUIRE); }
static void gate_put(void) { __atomic_store_n(&g_gate, 0, __ATOMIC_RELEASE); }
static void run_job(void (*fn)(void*), void* a) { if (S.runner) S.runner(fn, a); else fn(a); }
static const void* g_floor;
void acpidev_set_runner(void (*run)(void (*fn)(void*), void* arg), const void* stack_floor) { S.runner = run; g_floor = stack_floor; if (S.ctx) aml_set_stack_floor(S.ctx, stack_floor); }
void acpidev_set_hook(void (*fn)(int, int, void*), void* user) { S.hook = fn; S.hook_user = user; }
void acpidev_sci(void) { __atomic_store_n(&S.sci_flag, 1, __ATOMIC_RELEASE); }
int  acpidev_sci_take(void) { return (int)__atomic_exchange_n(&S.sci_flag, 0, __ATOMIC_ACQ_REL); }
int  acpidev_ready(void) { return S.ready; }
aml_ctx_t* acpidev_ctx(void) { return S.ctx; }
void acpidev_stats(acpidev_stats_t* s) { *s = S.st; }
const acpidev_power_t* acpidev_power(void) { return &S.pw; }

static void ev_push(int ev, int arg) {
    unsigned n = (S.et + 1) % 16;
    if (n == S.eh) return;              /* очередь полна: самое старое событие важнее - новое отбрасываем */
    S.evq[S.et].ev = ev; S.evq[S.et].arg = arg; S.et = n;
}

static int rd(unsigned space, uint64_t addr, unsigned bits, uint64_t* v) {
    if (!S.uops.rw) return -1;
    *v = 0; return S.uops.rw(S.uops.user, 0, space, addr, bits, v, 0);
}
static int wr(unsigned space, uint64_t addr, unsigned bits, uint64_t v) {
    if (!S.uops.rw) return -1;
    return S.uops.rw(S.uops.user, 1, space, addr, bits, &v, 0);
}
static void stall(uint32_t us) { if (S.uops.stall_us) S.uops.stall_us(S.uops.user, us); }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* ---------------- встроенный контроллер (EC): порты 0x62/0x66 по протоколу ACPI 12.3 ---------------- */

static int ec_wait(uint8_t mask, int want) {
    for (uint32_t i = 0; i < 20000; i++) {                    /* до ~20 мс */
        uint64_t s;
        if (rd(AML_SP_IO, S.ec_cmd, 8, &s) != 0) return -1;
        if (((s & mask) != 0) == (want != 0)) return 0;
        stall(1);
    }
    return -1;
}
static int ec_cmd(uint8_t c) { if (ec_wait(EC_IBF, 0)) return -1; return wr(AML_SP_IO, S.ec_cmd, 8, c); }
static int ec_put(uint8_t d) { if (ec_wait(EC_IBF, 0)) return -1; return wr(AML_SP_IO, S.ec_data, 8, d); }
static int ec_get(uint8_t* d) { uint64_t v; if (ec_wait(EC_OBF, 1)) return -1; if (rd(AML_SP_IO, S.ec_data, 8, &v)) return -1; *d = (uint8_t)v; return 0; }

static int ec_rw(int write, uint64_t addr, uint64_t* val) {
    if (!S.ec_ok || addr > 255) return -1;
    uint8_t d;
    int r;
    if (write) r = ec_cmd(0x81) || ec_put((uint8_t)addr) || ec_put((uint8_t)*val) || ec_wait(EC_IBF, 0);
    else { r = ec_cmd(0x80) || ec_put((uint8_t)addr) || ec_get(&d); if (!r) *val = d; }
    if (r) S.st.ec_errors++;
    return r ? -1 : 0;
}

/* ---------------- ops для интерпретатора ---------------- */

static int w_rw(void* u, int write, unsigned sp, uint64_t addr, unsigned bits, uint64_t* val, uint32_t pci) {
    (void)u;
    if (sp == AML_SP_EC) return ec_rw(write, addr, val);
    if (!S.uops.rw) return -1;
    return S.uops.rw(S.uops.user, write, sp, addr, bits, val, pci);
}
static void w_sleep(void* u, uint32_t ms) { (void)u; if (S.uops.sleep_ms) S.uops.sleep_ms(S.uops.user, ms); }
static void w_stall(void* u, uint32_t us) { (void)u; stall(us); }
static uint64_t w_timer(void* u) { (void)u; return S.uops.timer_100ns ? S.uops.timer_100ns(S.uops.user) : 0; }
static void w_log(void* u, const char* s) { (void)u; if (S.uops.log) S.uops.log(S.uops.user, s); }
static void w_notify(void* u, aml_node_t* n, uint32_t v) {
    (void)u;
    unsigned nx = (S.qt + 1) % NQ;
    S.st.notifies++;
    if (nx == S.qh) return;
    S.q[S.qt].n = n; S.q[S.qt].v = v; S.qt = nx;
}

/* ---------------- форматирование строк ---------------- */

typedef struct { char b[200]; unsigned n; } lb_t;
static void ls(lb_t* l, const char* s) { while (*s && l->n < sizeof l->b - 1) l->b[l->n++] = *s++; l->b[l->n] = 0; }
static void ld(lb_t* l, uint64_t v) { char t[24]; unsigned k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) { if (l->n < sizeof l->b - 1) l->b[l->n++] = t[--k]; } l->b[l->n] = 0; }
static void ldi(lb_t* l, int v) { if (v < 0) { ls(l, "-"); ld(l, (uint64_t)(-(int64_t)v)); } else ld(l, (uint64_t)v); }
static void lh(lb_t* l, uint64_t v, unsigned digits) {
    static const char hx[] = "0123456789ABCDEF"; char t[20]; unsigned k = 0;
    do { t[k++] = hx[v & 15]; v >>= 4; } while (v || k < digits);
    ls(l, "0x"); while (k) { if (l->n < sizeof l->b - 1) l->b[l->n++] = t[--k]; } l->b[l->n] = 0;
}
static void lflush(lb_t* l, void (*sink)(const char*)) { if (sink) sink(l->b); l->n = 0; l->b[0] = 0; }

/* ---------------- распознавание устройств ---------------- */

static void hid_str(const aml_val_t* v, char out[12]) {
    out[0] = 0;
    if (v->t == AML_V_INT) { aml_eisaid((uint32_t)v->u.i, out); }
    else if (v->t == AML_V_STR) { unsigned n = v->len < 11 ? v->len : 11; for (unsigned i = 0; i < n; i++) out[i] = (char)v->u.b[i]; out[n] = 0; }
}
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int kind_of_id(const char* id) {
    if (streq(id, "PNP0C0A")) return K_BAT;
    if (streq(id, "ACPI0003")) return K_AC;
    if (streq(id, "PNP0C0D")) return K_LID;
    if (streq(id, "PNP0C0C")) return K_PWRB;
    if (streq(id, "PNP0C0E")) return K_SLPB;
    if (streq(id, "PNP0C09")) return K_EC;
    if (streq(id, "PNP0A03") || streq(id, "PNP0A08")) return K_ROOT;
    return 0;
}

static int classify(aml_node_t* n) {
    aml_val_t v; char id[12];
    int k = 0;
    if (aml_call_path(S.ctx, n, "_HID", 0, 0, &v) == 0) { hid_str(&v, id); k = kind_of_id(id); }
    if (!k && aml_call_path(S.ctx, n, "_CID", 0, 0, &v) == 0) {
        if (v.t == AML_V_PKG) { for (uint32_t i = 0; i < v.len && !k; i++) { char t[12]; hid_str(&v.u.p[i], t); k = kind_of_id(t); } }
        else { hid_str(&v, id); k = kind_of_id(id); }
    }
    return k;
}

/* _STA: нет метода - устройство есть и работает (0x0F). */
static uint32_t sta_of(aml_node_t* n) {
    uint64_t s;
    if (aml_call_path(S.ctx, n, "_STA", 0, 0, &(aml_val_t){ 0 }) == AML_E_NOTFOUND) return 0x0F;
    if (aml_call_int(S.ctx, n, "_STA", &s) != 0) return 0;      /* метод есть, но упал: считаем устройство отсутствующим */
    return (uint32_t)s;
}

static int enum_cb(aml_node_t* n, int depth, void* u) {
    (void)depth; (void)u;
    aml_ntype_t t = aml_node_type(n);
    if (t == AML_N_THERMAL) { if (S.ntherm < 8) S.therm[S.ntherm++] = n; return 0; }
    if (t != AML_N_DEVICE) return 0;
    int k = classify(n);
    if (!k || S.ndev >= MAX_DEV) return 0;
    S.dev[S.ndev].n = n; S.dev[S.ndev].kind = (uint8_t)k; S.ndev++;
    if (k == K_BAT && S.nbat < MAX_BAT) S.bat[S.nbat++].n = n;
    return 0;
}

/* ---------------- ресурсы (_CRS): порты EC и линии прерываний ---------------- */

/* Разбор буфера ресурсов: вызывает cb(type, body, len) для каждого дескриптора до завершающего. */
static int res_walk(const uint8_t* b, uint32_t len, int (*cb)(unsigned type, int large, const uint8_t* body, uint32_t blen, void*), void* u) {
    uint32_t o = 0;
    while (o < len) {
        uint8_t t = b[o];
        if (t & 0x80) {
            if (len - o < 3) return -1;
            uint32_t bl = (uint32_t)b[o + 1] | (uint32_t)b[o + 2] << 8;
            if (bl > len - o - 3) return -1;
            if (cb(t & 0x7F, 1, b + o + 3, bl, u)) return 0;
            o += 3 + bl;
        } else {
            unsigned ty = (t >> 3) & 15; uint32_t bl = t & 7;
            if (bl > len - o - 1) return -1;
            if (ty == 0x0F) return 0;
            if (cb(ty, 0, b + o + 1, bl, u)) return 0;
            o += 1 + bl;
        }
    }
    return 0;
}

typedef struct { uint16_t port[2]; int n; } io_acc_t;
static int io_cb(unsigned type, int large, const uint8_t* b, uint32_t bl, void* u) {
    io_acc_t* a = (io_acc_t*)u;
    if (!large && type == 0x08 && bl >= 7 && a->n < 2) { a->port[a->n++] = (uint16_t)(b[1] | b[2] << 8); }   /* IO: info, min, max, align, len */
    return a->n >= 2;
}

typedef struct { int found; uint32_t gsi; int level, active_low; } irq_acc_t;
static int irq_cb(unsigned type, int large, const uint8_t* b, uint32_t bl, void* u) {
    irq_acc_t* a = (irq_acc_t*)u;
    if (a->found) return 1;
    if (!large && type == 0x04 && bl >= 2) {                     /* IRQ: маска из 16 бит, необязательный байт признаков */
        uint16_t mask = (uint16_t)(b[0] | b[1] << 8);
        for (unsigned i = 0; i < 16; i++) if (mask >> i & 1) {
            a->found = 1; a->gsi = i;
            if (bl >= 3) { a->level = !(b[2] & 1); a->active_low = (b[2] >> 3) & 1; }
            else { a->level = 0; a->active_low = 0; }           /* без признаков - ISA: фронт, активный высокий */
            return 1;
        }
    } else if (large && type == 0x09 && bl >= 6) {               /* расширенное IRQ: признаки, число, номера GSI */
        if (b[1] >= 1) {
            a->found = 1; a->gsi = (uint32_t)b[2] | (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16 | (uint32_t)b[5] << 24;
            a->level = !((b[0] >> 1) & 1); a->active_low = (b[0] >> 2) & 1;
            return 1;
        }
    }
    return 0;
}

static int ec_probe(aml_node_t* n) {
    aml_val_t v; io_acc_t a; a.n = 0;
    if (aml_call_path(S.ctx, n, "_CRS", 0, 0, &v) != 0 || v.t != AML_V_BUF) return -1;
    if (res_walk(v.u.b, v.len, io_cb, &a) != 0 || a.n < 2) return -1;
    S.ec_data = a.port[0]; S.ec_cmd = a.port[1];
    S.ec_gpe = -1;
    if (aml_call_path(S.ctx, n, "_GPE", 0, 0, &v) == 0) {
        uint64_t g;
        if (v.t == AML_V_INT) S.ec_gpe = (int)v.u.i;
        else if (v.t == AML_V_PKG && v.len >= 2 && aml_val_int(&v.u.p[1], &g) == 0) S.ec_gpe = (int)g;
    }
    S.ec_node = n; S.ec_ok = 1;
    return 0;
}

/* ---------------- инициализация ---------------- */

size_t acpidev_arena_size(const acpidev_tab_t* dsdt, const acpidev_tab_t* ssdt, unsigned nssdt) {
    size_t n = ACPIDEV_ARENA_BASE + (dsdt ? dsdt->len : 0) + 64;
    for (unsigned i = 0; i < nssdt; i++) n += ssdt[i].len + 8;
    return n;
}

static int ini_walk(aml_node_t* parent, int depth) {
    if (depth > 24) return 0;
    for (aml_node_t* n = aml_first_child(parent); n; n = aml_next_sibling(n)) {
        aml_ntype_t t = aml_node_type(n);
        if (t != AML_N_DEVICE && t != AML_N_PROC && t != AML_N_THERMAL) continue;
        uint32_t sta = sta_of(n);
        if (!(sta & 1) && !(sta & 8)) continue;                   /* нет и не работает: ни _INI, ни потомков */
        aml_node_t* ini = aml_lookup(S.ctx, n, "_INI");
        if (ini && aml_node_type(ini) == AML_N_METHOD && aml_method_argc(ini) == 0) {
            aml_val_t v; S.st.ini_run++;
            if (aml_call(S.ctx, ini, 0, 0, &v) != 0) S.st.ini_fail++;
        }
        ini_walk(n, depth + 1);
    }
    return 0;
}

typedef struct { const void* a; } nop_t;

static struct { void* mem; size_t size; const aml_ops_t* ops; const acpidev_hw_t* hw; const acpidev_tab_t* dsdt; const acpidev_tab_t* ssdt; unsigned nssdt; int rc; } IJ;

static void init_job(void* p) {
    (void)p;
    uint8_t* m = (uint8_t*)((((uintptr_t)IJ.mem) + 7) & ~(uintptr_t)7);
    size_t size = IJ.size - (size_t)(m - (uint8_t*)IJ.mem);
    if (!IJ.dsdt || !IJ.dsdt->p || IJ.dsdt->len < 36) { IJ.rc = AML_E_TABLE; return; }
    size_t need = IJ.dsdt->len;
    for (unsigned i = 0; i < IJ.nssdt; i++) need += (IJ.ssdt[i].len + 7u) & ~7u;
    need = (need + 7) & ~(size_t)7;
    if (size < need + 32768) { IJ.rc = AML_E_NOMEM; return; }
    /* Копия таблиц внутри арены: окно ядра без кэша для разбора слишком медленное. */
    uint8_t* tp = m;
    const uint8_t* d = IJ.dsdt->p;
    { uint32_t n = IJ.dsdt->len; for (uint32_t i = 0; i < n; i++) tp[i] = d[i]; }
    const uint8_t* dcopy = tp; uint32_t dlen = IJ.dsdt->len;
    tp += (dlen + 7u) & ~7u;
    const uint8_t* scopy[ACPI_MAX_SSDT_]; uint32_t slen[ACPI_MAX_SSDT_]; unsigned ns = 0;
    for (unsigned k = 0; k < IJ.nssdt && ns < ACPI_MAX_SSDT_; k++) {
        const uint8_t* s = IJ.ssdt[k].p; uint32_t n = IJ.ssdt[k].len;
        if (!s || n < 36) continue;
        for (uint32_t i = 0; i < n; i++) tp[i] = s[i];
        scopy[ns] = tp; slen[ns] = n; ns++;
        tp += (n + 7u) & ~7u;
    }
    S.uops = *IJ.ops; S.hw = *IJ.hw;
    aml_ops_t o = { w_rw, w_sleep, w_stall, w_timer, w_notify, w_log, 0 };
    S.ctx = aml_create(tp, size - (size_t)(tp - m), &o);
    if (!S.ctx) { IJ.rc = AML_E_NOMEM; return; }
    aml_set_stack_floor(S.ctx, g_floor);
    int r = aml_load(S.ctx, dcopy, dlen);
    if (r) { IJ.rc = r; S.ctx = 0; return; }
    for (unsigned i = 0; i < ns; i++) (void)aml_load(S.ctx, scopy[i], slen[i]);    /* битая SSDT не должна лишать нас остальных */
    aml_load_done(S.ctx);
    aml_walk(S.ctx, enum_cb, 0);
    S.gpe_scope = aml_lookup(S.ctx, 0, "\\_GPE");
    for (unsigned i = 0; i < S.ndev; i++) if (S.dev[i].kind == K_EC && S.dev[i].n && !S.ec_ok && (sta_of(S.dev[i].n) & 1)) (void)ec_probe(S.dev[i].n);
    /* \_SB._INI и _INI устройств (ACPI 6.4, 6.5.1): сначала корневая, потом по дереву. EC уже доступен: многие _INI читают его поля. */
    aml_node_t* sb = aml_lookup(S.ctx, 0, "\\_SB");
    if (sb) {
        aml_node_t* ini = aml_lookup(S.ctx, sb, "_INI");
        if (ini && aml_node_type(ini) == AML_N_METHOD) { aml_val_t v; S.st.ini_run++; if (aml_call(S.ctx, ini, 0, 0, &v) != 0) S.st.ini_fail++; }
        ini_walk(sb, 0);
    }
    S.ready = 1;
    IJ.rc = 0;
}

int acpidev_init(void* mem, size_t size, const aml_ops_t* ops, const acpidev_hw_t* hw, const acpidev_tab_t* dsdt, const acpidev_tab_t* ssdt, unsigned nssdt) {
    void (*keep_run)(void (*)(void*), void*) = S.runner;
    for (unsigned i = 0; i < sizeof S; i++) ((volatile uint8_t*)&S)[i] = 0;   /* повторная инициализация (тесты) - с чистого листа */
    S.runner = keep_run;
    IJ.mem = mem; IJ.size = size; IJ.ops = ops; IJ.hw = hw; IJ.dsdt = dsdt; IJ.ssdt = ssdt; IJ.nssdt = nssdt; IJ.rc = 0;
    run_job(init_job, 0);
    if (IJ.rc == 0) acpidev_power_refresh();
    return IJ.rc;
}

/* ---------------- события ---------------- */

static void gpe_name(char out[5], char kind, unsigned n) { out[0] = '_'; out[1] = kind; out[2] = "0123456789ABCDEF"[(n >> 4) & 15]; out[3] = "0123456789ABCDEF"[n & 15]; out[4] = 0; }

/* Блок GPE, в который попадает номер: порт состояния и разрешения, байт и бит. 0 - вне блоков. */
static int gpe_loc(unsigned g, uint32_t* st_port, uint32_t* en_port, unsigned* bit) {
    unsigned half0 = S.hw.gpe0_len / 2u, half1 = S.hw.gpe1_len / 2u;
    if (S.hw.gpe0_blk && g < half0 * 8u) { *st_port = S.hw.gpe0_blk + g / 8; *en_port = S.hw.gpe0_blk + half0 + g / 8; *bit = g % 8; return 1; }
    if (S.hw.gpe1_blk && g >= S.hw.gpe1_base && g - S.hw.gpe1_base < half1 * 8u) {
        unsigned x = g - S.hw.gpe1_base; *st_port = S.hw.gpe1_blk + x / 8; *en_port = S.hw.gpe1_blk + half1 + x / 8; *bit = x % 8; return 1;
    }
    return 0;
}
static int gpe_has_handler(unsigned g, char* kind) {
    if (!S.gpe_scope) return 0;
    char nm[5];
    gpe_name(nm, 'L', g); if (aml_lookup(S.ctx, S.gpe_scope, nm)) { *kind = 'L'; return 1; }
    gpe_name(nm, 'E', g); if (aml_lookup(S.ctx, S.gpe_scope, nm)) { *kind = 'E'; return 1; }
    return 0;
}
static void gpe_enable(unsigned g, int on) {
    uint32_t sp, ep; unsigned bit; uint64_t v;
    if (!gpe_loc(g, &sp, &ep, &bit)) return;
    if (rd(AML_SP_IO, ep, 8, &v)) return;
    if (on) v |= 1u << bit; else v &= ~(1u << bit);
    (void)wr(AML_SP_IO, ep, 8, v);
}
static void gpe_clear(unsigned g) {
    uint32_t sp, ep; unsigned bit;
    if (gpe_loc(g, &sp, &ep, &bit)) (void)wr(AML_SP_IO, sp, 8, 1u << bit);
}

static void ec_service(void) {
    if (!S.ec_ok) return;
    for (int i = 0; i < 16; i++) {
        uint64_t s;
        if (rd(AML_SP_IO, S.ec_cmd, 8, &s) != 0 || !(s & EC_SCI)) return;
        uint8_t q;
        if (ec_cmd(0x84) || ec_get(&q)) { S.st.ec_errors++; return; }
        if (q == 0) return;                                        /* событий больше нет */
        char nm[5]; nm[0] = '_'; nm[1] = 'Q'; nm[2] = "0123456789ABCDEF"[q >> 4]; nm[3] = "0123456789ABCDEF"[q & 15]; nm[4] = 0;
        aml_node_t* m = aml_lookup(S.ctx, S.ec_node, nm);
        S.st.ec_queries++;
        if (m && aml_node_type(m) == AML_N_METHOD) { aml_val_t v; (void)aml_call(S.ctx, m, 0, 0, &v); }
    }
}

static void gpe_service_one(unsigned g) {
    char kind = 0;
    int ec = S.ec_ok && S.ec_gpe == (int)g;
    gpe_enable(g, 0);
    S.st.gpe_events++;
    if (ec) { gpe_clear(g); ec_service(); gpe_enable(g, 1); return; }
    if (!gpe_has_handler(g, &kind)) {
        gpe_clear(g);
        if (g < 256 && !S.warned_gpe[g / 8 % 32]) { S.warned_gpe[g / 8 % 32] = 1; S.st.gpe_unhandled++; }
        return;                                                    /* без метода событие не включаем обратно: иначе шторм */
    }
    char nm[5]; gpe_name(nm, kind, g);
    aml_node_t* m = aml_lookup(S.ctx, S.gpe_scope, nm);
    if (kind == 'E') gpe_clear(g);
    if (m && aml_node_type(m) == AML_N_METHOD) { aml_val_t v; (void)aml_call(S.ctx, m, 0, 0, &v); }
    if (kind == 'L') gpe_clear(g);
    gpe_enable(g, 1);
}

static void gpe_scan_block(uint32_t blk, uint8_t len, unsigned base) {
    unsigned half = len / 2u;
    for (unsigned i = 0; blk && i < half; i++) {
        uint64_t st, en;
        if (rd(AML_SP_IO, blk + i, 8, &st) || rd(AML_SP_IO, blk + half + i, 8, &en)) return;
        uint32_t p = (uint32_t)(st & en & 0xFF);
        for (unsigned b = 0; b < 8; b++) if (p >> b & 1) gpe_service_one(base + i * 8 + b);
    }
}

static void fixed_service(void) {
    if (!S.hw.pm1a_evt || S.hw.pm1_evt_len < 4) return;
    unsigned half = S.hw.pm1_evt_len / 2u;
    uint64_t st = 0, en = 0, s2 = 0, e2 = 0;
    if (rd(AML_SP_IO, S.hw.pm1a_evt, 16, &st) || rd(AML_SP_IO, S.hw.pm1a_evt + half, 16, &en)) return;
    if (S.hw.pm1b_evt) { (void)rd(AML_SP_IO, S.hw.pm1b_evt, 16, &s2); (void)rd(AML_SP_IO, S.hw.pm1b_evt + half, 16, &e2); }
    uint32_t p = (uint32_t)((st & en) | (s2 & e2));
    if (p & PM1_PWRBTN) {
        (void)wr(AML_SP_IO, S.hw.pm1a_evt, 16, PM1_PWRBTN);
        if (S.hw.pm1b_evt) (void)wr(AML_SP_IO, S.hw.pm1b_evt, 16, PM1_PWRBTN);
        S.st.pm1_events++; ev_push(ACPIDEV_EV_POWER_BUTTON, 0);
    }
    if (p & PM1_SLPBTN) {
        (void)wr(AML_SP_IO, S.hw.pm1a_evt, 16, PM1_SLPBTN);
        if (S.hw.pm1b_evt) (void)wr(AML_SP_IO, S.hw.pm1b_evt, 16, PM1_SLPBTN);
        S.st.pm1_events++; ev_push(ACPIDEV_EV_SLEEP_BUTTON, 0);
    }
}

static int dev_kind(aml_node_t* n) { for (unsigned i = 0; i < S.ndev; i++) if (S.dev[i].n == n) return S.dev[i].kind; return 0; }

static void drain_notifies(void) {
    while (S.qh != S.qt) {
        aml_node_t* n = S.q[S.qh].n; uint32_t v = S.q[S.qh].v;
        S.qh = (S.qh + 1) % NQ;
        int k = dev_kind(n);
        if (k == K_BAT || k == K_AC) { S.power_dirty = 1; if (k == K_BAT && v != 0x80) for (unsigned i = 0; i < S.nbat; i++) if (S.bat[i].n == n) S.bat[i].info_ok = 0; }
        else if (k == K_LID) S.power_dirty = 1;
        else if (k == K_PWRB && v == 0x80) ev_push(ACPIDEV_EV_POWER_BUTTON, 0);
        else if (k == K_SLPB && v == 0x80) ev_push(ACPIDEV_EV_SLEEP_BUTTON, 0);
        else {
            for (unsigned i = 0; i < S.ntherm; i++) if (S.therm[i] == n) S.power_dirty = 1;
        }
    }
}

static struct { uint64_t now; int force; } PJ;
static void power_job(void* p);
static void poll_job(void* p) {
    (void)p;
    if (!S.ready) return;
    if (PJ.force || PJ.now - S.last_service_ms >= 250 || !S.inited_time) {
        S.last_service_ms = PJ.now; S.inited_time = 1;
        fixed_service();
        gpe_scan_block(S.hw.gpe0_blk, S.hw.gpe0_len, 0);
        gpe_scan_block(S.hw.gpe1_blk, S.hw.gpe1_len, S.hw.gpe1_base);
        drain_notifies();
    }
    if (S.power_dirty || PJ.now - S.last_power_ms >= 10000) {
        S.last_power_ms = PJ.now; S.power_dirty = 0;
        power_job(0);
    }
}

void acpidev_poll(uint64_t now_ms) {
    if (!S.ready) return;
    int force = acpidev_sci_take();
    if (!force && S.qh == S.qt && !S.power_dirty && now_ms - S.last_service_ms < 250 && now_ms - S.last_power_ms < 10000 && S.inited_time) goto events;
    if (!gate_take()) { if (force) __atomic_store_n(&S.sci_flag, 1, __ATOMIC_RELEASE); goto events; }   /* занято: SCI не теряем, разберём в следующий раз */
    PJ.now = now_ms; PJ.force = force;
    run_job(poll_job, 0);
    gate_put();
events:
    while (S.eh != S.et) {                                            /* события отдаём ядру вне отдельного стека */
        int ev = S.evq[S.eh].ev, arg = S.evq[S.eh].arg; S.eh = (S.eh + 1) % 16;
        if (S.hook) S.hook(ev, arg, S.hook_user);
    }
}

/* ---------------- разрешение событий, сон, S5, PIC ---------------- */

static struct { int rc; int arg; uint8_t a, b; } MJ;

static int reg_cb(aml_node_t* n, int depth, void* u) {
    (void)depth;
    if (aml_node_type(n) != AML_N_METHOD || aml_method_argc(n) != 2) return 0;
    char nm[5]; aml_node_name(n, nm);
    if (nm[0] == '_' && nm[1] == 'R' && nm[2] == 'E' && nm[3] == 'G') {
        aml_node_t* par = aml_parent(n);
        for (int sp = 0; sp <= 3; sp++) {
            if (sp == 3 && par != S.ec_node) continue;           /* регион EC доступен только EC */
            aml_val_t a[2] = { aml_int((uint64_t)sp), aml_int(1) }, v;
            S.st.reg_run++;
            (void)aml_call(S.ctx, n, a, 2, &v);
        }
        (*(unsigned*)u)++;
    }
    return 0;
}

static void enable_job(void* p) {
    (void)p;
    unsigned nreg = 0;
    aml_walk(S.ctx, reg_cb, &nreg);
    if (S.hw.pm1a_evt && S.hw.pm1_evt_len >= 4 && !(S.hw.fadt_flags & FADT_PWR_BUTTON_AML)) {
        unsigned half = S.hw.pm1_evt_len / 2u; uint64_t en;
        (void)wr(AML_SP_IO, S.hw.pm1a_evt, 16, PM1_PWRBTN);       /* старое нажатие не считаем */
        if (rd(AML_SP_IO, S.hw.pm1a_evt + half, 16, &en) == 0) { (void)wr(AML_SP_IO, S.hw.pm1a_evt + half, 16, en | PM1_PWRBTN); S.fixed_pwr_enabled = 1; }
    }
    if (S.gpe_scope) {
        for (aml_node_t* n = aml_first_child(S.gpe_scope); n; n = aml_next_sibling(n)) {
            char nm[5]; aml_node_name(n, nm);
            if (nm[0] != '_' || (nm[1] != 'L' && nm[1] != 'E') || hexv(nm[2]) < 0 || hexv(nm[3]) < 0) continue;
            unsigned g = (unsigned)(hexv(nm[2]) * 16 + hexv(nm[3]));
            if (S.ec_ok && S.ec_gpe == (int)g) continue;           /* GPE встроенного контроллера включится ниже */
            gpe_clear(g); gpe_enable(g, 1);
        }
    }
    if (S.ec_ok && S.ec_gpe >= 0) { gpe_clear((unsigned)S.ec_gpe); gpe_enable((unsigned)S.ec_gpe, 1); }
    MJ.rc = 0;
}
int acpidev_enable_events(void) { if (!S.ready) return -1; if (!gate_take()) return AML_E_BUSY; run_job(enable_job, 0); int r = MJ.rc; gate_put(); return r; }

static void pic_job(void* p) {
    (void)p;
    aml_node_t* n = aml_lookup(S.ctx, 0, "\\_PIC");
    if (!n || aml_node_type(n) != AML_N_METHOD) { MJ.rc = AML_E_NOTFOUND; return; }
    aml_val_t a = aml_int((uint64_t)MJ.arg), v;
    MJ.rc = aml_call(S.ctx, n, &a, 1, &v);
}
int acpidev_pic_mode(int apic) { if (!S.ready) return -1; if (!gate_take()) return AML_E_BUSY; MJ.arg = apic ? 1 : 0; run_job(pic_job, 0); int r = MJ.rc; gate_put(); return r; }

static void sleep_job(void* p) {
    (void)p;
    static const char* const names[2] = { "\\_PTS", "\\_GTS" };
    MJ.rc = 0;
    for (int i = 0; i < 2; i++) {
        aml_node_t* n = aml_lookup(S.ctx, 0, names[i]);
        if (!n || aml_node_type(n) != AML_N_METHOD) continue;
        aml_val_t a = aml_int((uint64_t)MJ.arg), v;
        int r = aml_call(S.ctx, n, &a, 1, &v);
        if (r) MJ.rc = r;
    }
}
int acpidev_prepare_sleep(int state) { if (!S.ready) return AML_E_NOTFOUND; if (!gate_take()) return AML_E_BUSY; MJ.arg = state; run_job(sleep_job, 0); int r = MJ.rc; gate_put(); return r; }

static void s5_job(void* p) {
    (void)p;
    aml_val_t v;
    MJ.rc = aml_call_path(S.ctx, 0, "\\_S5", 0, 0, &v);
    if (MJ.rc) return;
    uint64_t a, b = 0;
    const aml_val_t* e0 = aml_pkg_at(&v, 0);
    if (!e0 || aml_val_int(e0, &a) != 0) { MJ.rc = AML_E_TYPE; return; }
    const aml_val_t* e1 = aml_pkg_at(&v, 1);
    if (e1) (void)aml_val_int(e1, &b);
    MJ.a = (uint8_t)a; MJ.b = (uint8_t)b;
}
int acpidev_s5(uint8_t* a, uint8_t* b) {
    if (!S.ready) return AML_E_NOTFOUND;
    if (!gate_take()) return AML_E_BUSY;
    run_job(s5_job, 0);
    int r = MJ.rc;
    if (r == 0) { *a = MJ.a; *b = MJ.b; }
    gate_put();
    return r;
}

/* ---------------- питание ---------------- */

static int pkg_u32(const aml_val_t* pk, unsigned i, uint32_t* out) {
    const aml_val_t* e = aml_pkg_at(pk, i); uint64_t v;
    if (!e || aml_val_int(e, &v) != 0) return -1;
    *out = (uint32_t)v; return 0;
}

static void bat_info(bat_t* b) {
    aml_val_t v; uint32_t d = 0, l = 0;
    if (aml_call_path(S.ctx, b->n, "_BIX", 0, 0, &v) == 0 && v.t == AML_V_PKG && pkg_u32(&v, 2, &d) == 0 && pkg_u32(&v, 3, &l) == 0) { b->design = d; b->last_full = l; b->info_ok = 1; return; }
    if (aml_call_path(S.ctx, b->n, "_BIF", 0, 0, &v) == 0 && v.t == AML_V_PKG && pkg_u32(&v, 1, &d) == 0 && pkg_u32(&v, 2, &l) == 0) { b->design = d; b->last_full = l; b->info_ok = 1; return; }
}

static void power_job(void* p) {
    (void)p;
    acpidev_power_t n; unsigned i; uint32_t rem_sum = 0, full_sum = 0, rate_sum = 0; int any_full = 1, any_rem = 0;
    n.valid = 1; n.batteries = 0; n.percent = -1; n.charging = n.discharging = n.critical = 0; n.ac = -1; n.lid = -1;
    n.temp_valid = 0; n.temp_c = 0; n.minutes_left = -1; n.remaining = n.last_full = n.rate = 0; n.version = S.pw.version;
    for (i = 0; i < S.nbat; i++) {
        bat_t* b = &S.bat[i]; uint32_t sta = sta_of(b->n);
        int present = (sta & 1) && (sta & 0x10);
        if (present != b->present) { b->present = present; b->info_ok = 0; }
        if (!present) continue;
        if (!b->info_ok) bat_info(b);
        aml_val_t v; uint32_t st = 0, rate = 0, rem = 0, volt = 0;
        if (aml_call_path(S.ctx, b->n, "_BST", 0, 0, &v) != 0 || v.t != AML_V_PKG || pkg_u32(&v, 0, &st) || pkg_u32(&v, 1, &rate) || pkg_u32(&v, 2, &rem) || pkg_u32(&v, 3, &volt)) continue;
        (void)volt;
        n.batteries++;
        if (st & 1) n.discharging = 1;
        if (st & 2) n.charging = 1;
        if (st & 4) n.critical = 1;
        uint32_t full = b->info_ok && b->last_full && b->last_full != 0xFFFFFFFFu ? b->last_full : (b->info_ok && b->design != 0xFFFFFFFFu ? b->design : 0);
        if (rem != 0xFFFFFFFFu && full) { rem_sum += rem > full ? full : rem; full_sum += full; any_rem = 1; } else any_full = 0;
        if (rate != 0xFFFFFFFFu) rate_sum += rate;
    }
    if (n.batteries && any_rem && any_full && full_sum) {
        n.percent = (int)((uint64_t)rem_sum * 100 / full_sum);
        n.remaining = rem_sum; n.last_full = full_sum; n.rate = rate_sum;
        if (n.discharging && rate_sum) n.minutes_left = (int)((uint64_t)rem_sum * 60 / rate_sum);
    }
    for (i = 0; i < S.ndev; i++) {
        uint64_t x;
        if (S.dev[i].kind == K_AC && (sta_of(S.dev[i].n) & 1) && aml_call_int(S.ctx, S.dev[i].n, "_PSR", &x) == 0) n.ac = (n.ac == 1 || x) ? 1 : 0;
        if (S.dev[i].kind == K_LID && (sta_of(S.dev[i].n) & 1) && aml_call_int(S.ctx, S.dev[i].n, "_LID", &x) == 0) n.lid = x ? 1 : 0;
    }
    for (i = 0; i < S.ntherm; i++) {
        uint64_t t;
        if (aml_call_int(S.ctx, S.therm[i], "_TMP", &t) == 0 && t >= 2732 && t <= 2732 + 1500) {      /* 0..150 °C; остальное - ошибка датчика */
            int c = (int)((t - 2732 + 5) / 10);
            if (!n.temp_valid || c > n.temp_c) { n.temp_c = c; n.temp_valid = 1; }
        }
    }
    int lid_changed = S.pw.valid && n.lid >= 0 && n.lid != S.pw.lid;
    int changed = !S.pw.valid || n.batteries != S.pw.batteries || n.percent != S.pw.percent || n.charging != S.pw.charging || n.discharging != S.pw.discharging ||
                  n.critical != S.pw.critical || n.ac != S.pw.ac || n.lid != S.pw.lid || n.temp_valid != S.pw.temp_valid || n.temp_c != S.pw.temp_c ||
                  n.minutes_left != S.pw.minutes_left;
    if (changed) { n.version = S.pw.version + 1; S.pw = n; ev_push(ACPIDEV_EV_POWER, 0); }
    else { S.pw.remaining = n.remaining; S.pw.rate = n.rate; S.pw.last_full = n.last_full; }
    if (lid_changed) ev_push(ACPIDEV_EV_LID, n.lid);
}

void acpidev_power_refresh(void) { if (!S.ready) return; if (!gate_take()) return; run_job(power_job, 0); gate_put(); }

/* ---------------- _PRT ---------------- */

#define MAX_PRT 160
typedef struct { uint32_t addr, pin, gsi; aml_node_t* src; } prt_ent_t;
static prt_ent_t prt_buf[MAX_PRT];

/* Записи _PRT копируются сразу: следующий вызов AML (например _CRS связки) отбрасывает временные значения. */
static unsigned prt_load(aml_node_t* root) {
    aml_val_t v; unsigned n = 0;
    if (aml_call_path(S.ctx, root, "_PRT", 0, 0, &v) != 0 || v.t != AML_V_PKG) return 0;
    for (uint32_t i = 0; i < v.len && n < MAX_PRT; i++) {
        const aml_val_t* e = &v.u.p[i]; uint64_t a, p, g = 0;
        if (e->t != AML_V_PKG || e->len < 4) continue;
        if (aml_val_int(aml_pkg_at(e, 0), &a) != 0 || aml_val_int(aml_pkg_at(e, 1), &p) != 0) continue;
        aml_node_t* src = 0;
        const aml_val_t* sv = aml_pkg_at(e, 2);
        if (sv->t == AML_V_REF) { if (aml_val_node(sv, &src) != 0) continue; }
        else if (sv->t == AML_V_INT && sv->u.i == 0) { if (aml_val_int(aml_pkg_at(e, 3), &g) != 0) continue; }
        else continue;
        prt_buf[n].addr = (uint32_t)a; prt_buf[n].pin = (uint32_t)p; prt_buf[n].gsi = (uint32_t)g; prt_buf[n].src = src; n++;
    }
    return n;
}

/* Линия записи _PRT: прямой номер GSI или ссылка на устройство-связку (его _CRS). */
static int prt_resolve(const prt_ent_t* e, acpidev_irq_t* o) {
    o->found = 0; o->via_link = 0; o->link[0] = 0; o->gsi = 0; o->level = 0; o->active_low = 0;
    if (!e->src) { o->found = 1; o->gsi = e->gsi; o->level = 1; o->active_low = 1; return 1; }       /* GSI напрямую: линия PCI - уровень, активный низкий */
    aml_val_t v;
    uint32_t sta = sta_of(e->src);
    if (!(sta & 2)) return 0;                                    /* связка выключена: линия не подведена */
    if (aml_call_path(S.ctx, e->src, "_CRS", 0, 0, &v) != 0 || v.t != AML_V_BUF) return 0;
    irq_acc_t a = { 0, 0, 0, 0 };
    if (res_walk(v.u.b, v.len, irq_cb, &a) != 0 || !a.found) return 0;
    o->found = 1; o->gsi = a.gsi; o->level = a.level; o->active_low = a.active_low; o->via_link = 1;
    aml_node_name(e->src, o->link);
    return 1;
}

static aml_node_t* root_for_bus(unsigned bus) {
    for (unsigned i = 0; i < S.ndev; i++) {
        if (S.dev[i].kind != K_ROOT) continue;
        uint64_t b = 0, seg = 0;
        if (aml_call_int(S.ctx, S.dev[i].n, "_BBN", &b) != 0) b = 0;
        (void)aml_call_int(S.ctx, S.dev[i].n, "_SEG", &seg);
        if (seg == 0 && (unsigned)b == bus) return S.dev[i].n;
    }
    return 0;
}

static struct { unsigned bus, dev, fn, pin; acpidev_irq_t* out; } PI;
static void pci_irq_job(void* p) {
    (void)p;
    PI.out->found = 0;
    aml_node_t* root = root_for_bus(PI.bus);
    if (!root) return;
    unsigned n = prt_load(root);
    for (unsigned i = 0; i < n; i++) {
        unsigned d = (prt_buf[i].addr >> 16) & 0xFFFF, f = prt_buf[i].addr & 0xFFFF;
        if (d != PI.dev || (f != 0xFFFF && f != PI.fn) || prt_buf[i].pin != PI.pin - 1) continue;
        (void)prt_resolve(&prt_buf[i], PI.out);
        return;
    }
}
int acpidev_pci_irq(unsigned bus, unsigned dev, unsigned fn, unsigned pin, acpidev_irq_t* out) {
    out->found = 0;
    if (!S.ready || pin < 1 || pin > 4 || bus > 255 || dev > 31) return 0;
    if (!gate_take()) return 0;
    PI.bus = bus; PI.dev = dev; PI.fn = fn; PI.pin = pin; PI.out = out;
    run_job(pci_irq_job, 0);
    gate_put();
    return out->found;
}

/* ---------------- печать ---------------- */

static void print_val(lb_t* l, const aml_val_t* v, int depth) {
    switch (v->t) {
    case AML_V_INT: lh(l, v->u.i, 1); ls(l, " ("); ld(l, v->u.i); ls(l, ")"); break;
    case AML_V_STR: ls(l, "\""); for (uint32_t i = 0; i < v->len && i < 64; i++) { char c[2] = { (char)(v->u.b[i] >= 32 && v->u.b[i] < 127 ? v->u.b[i] : '?'), 0 }; ls(l, c); } ls(l, "\""); break;
    case AML_V_BUF: ls(l, "буфер["); ld(l, v->len); ls(l, "]:"); for (uint32_t i = 0; i < v->len && i < 24; i++) { ls(l, " "); lh(l, v->u.b[i], 2); } if (v->len > 24) ls(l, " ..."); break;
    case AML_V_PKG:
        ls(l, "{");
        for (uint32_t i = 0; i < v->len && i < 16; i++) { if (i) ls(l, ", "); if (depth < 3) print_val(l, &v->u.p[i], depth + 1); else ls(l, "..."); }
        if (v->len > 16) ls(l, ", ...");
        ls(l, "}"); break;
    case AML_V_REF: { char p[48]; aml_node_t* nd; if (aml_val_node(v, &nd) == 0 && aml_node_path(nd, p, sizeof p) > 0) ls(l, p); else ls(l, "ссылка"); break; }
    default: ls(l, "(пусто)"); break;
    }
}

static int eval_impl(const char* path, void (*sink)(const char*)) {
    if (!S.ready || !path) return -1;
    static aml_val_t out;
    aml_node_t* n = aml_lookup(S.ctx, 0, path);
    lb_t l; l.n = 0; l.b[0] = 0;
    if (!n) { ls(&l, "ACPI: нет такого объекта: "); ls(&l, path); lflush(&l, sink); return AML_E_NOTFOUND; }
    unsigned argc = aml_method_argc(n);
    if (argc) { ls(&l, "ACPI: метод требует аргументов: "); ld(&l, argc); lflush(&l, sink); return AML_E_ARGS; }
    int r = aml_call(S.ctx, n, 0, 0, &out);
    ls(&l, path); ls(&l, " = ");
    if (r) { ls(&l, "ошибка "); ldi(&l, r); } else print_val(&l, &out, 0);
    lflush(&l, sink);
    return r;
}

static struct { const char* path; void (*sink)(const char*); int rc; } EJ;
static void eval_job(void* p) { (void)p; EJ.rc = eval_impl(EJ.path, EJ.sink); }
int acpidev_eval(const char* path, void (*sink)(const char*)) {
    if (!S.ready || !path) return -1;
    if (!gate_take()) return AML_E_BUSY;
    EJ.path = path; EJ.sink = sink; run_job(eval_job, 0);
    int r = EJ.rc;
    gate_put();
    return r;
}

static const char* kind_name(int k) {
    switch (k) { case K_BAT: return "батарея"; case K_AC: return "сеть питания"; case K_LID: return "крышка"; case K_PWRB: return "кнопка питания";
                 case K_SLPB: return "кнопка сна"; case K_EC: return "встроенный контроллер"; case K_ROOT: return "корневой мост PCI"; default: return "?"; }
}

static struct { void (*sink)(const char*); } DJ;
static void dump_job(void* p) {
    (void)p;
    void (*sink)(const char*) = DJ.sink;
    lb_t l; l.n = 0; l.b[0] = 0;
    aml_stats_t st; aml_stats(S.ctx, &st);
    ls(&l, "ACPI AML: таблиц "); ld(&l, st.loaded_tables); ls(&l, ", объектов "); ld(&l, st.nodes); ls(&l, ", арена "); ld(&l, st.arena_used / 1024); ls(&l, " из ");
    ld(&l, st.arena_size / 1024); ls(&l, " КБ, повторных имён "); ld(&l, st.dup_names); ls(&l, ", ошибок загрузки "); ld(&l, st.load_errors); lflush(&l, sink);
    ls(&l, "ACPI события: SCI линия "); ld(&l, S.hw.sci_int); ls(&l, ", кнопка питания "); ls(&l, S.fixed_pwr_enabled ? "фиксированная включена" : (S.hw.fadt_flags & FADT_PWR_BUTTON_AML) ? "через AML" : "не включена");
    ls(&l, ", PM1 "); ld(&l, S.st.pm1_events); ls(&l, ", GPE "); ld(&l, S.st.gpe_events); ls(&l, " (без метода "); ld(&l, S.st.gpe_unhandled); ls(&l, "), Notify "); ld(&l, S.st.notifies);
    ls(&l, ", _INI "); ld(&l, S.st.ini_run); ls(&l, " (ошибок "); ld(&l, S.st.ini_fail); ls(&l, "), _REG "); ld(&l, S.st.reg_run); lflush(&l, sink);
    ls(&l, "ACPI EC: ");
    if (S.ec_ok) { ls(&l, "порты данных "); lh(&l, S.ec_data, 2); ls(&l, " и команд "); lh(&l, S.ec_cmd, 2); ls(&l, ", GPE "); ldi(&l, S.ec_gpe); ls(&l, ", запросов "); ld(&l, S.st.ec_queries); ls(&l, ", ошибок обмена "); ld(&l, S.st.ec_errors); }
    else ls(&l, "нет");
    lflush(&l, sink);
    for (unsigned i = 0; i < S.ndev; i++) {
        char p[64]; if (aml_node_path(S.dev[i].n, p, sizeof p) <= 0) continue;
        ls(&l, "ACPI устройство "); ls(&l, p); ls(&l, ": "); ls(&l, kind_name(S.dev[i].kind)); ls(&l, ", _STA "); lh(&l, sta_of(S.dev[i].n), 1); lflush(&l, sink);
    }
    for (unsigned i = 0; i < S.ntherm; i++) { char p[64]; if (aml_node_path(S.therm[i], p, sizeof p) > 0) { ls(&l, "ACPI термозона "); ls(&l, p); lflush(&l, sink); } }
    const acpidev_power_t* w = &S.pw;
    ls(&l, "ACPI питание: батарей "); ld(&l, (uint64_t)w->batteries);
    if (w->percent >= 0) { ls(&l, ", заряд "); ld(&l, (uint64_t)w->percent); ls(&l, "%"); }
    ls(&l, w->charging ? ", заряжается" : w->discharging ? ", разряжается" : ""); if (w->critical) ls(&l, ", КРИТИЧЕСКИЙ");
    if (w->minutes_left >= 0) { ls(&l, ", осталось ~"); ld(&l, (uint64_t)w->minutes_left); ls(&l, " мин"); }
    ls(&l, ", сеть "); ls(&l, w->ac > 0 ? "есть" : w->ac == 0 ? "нет" : "нет данных");
    ls(&l, ", крышка "); ls(&l, w->lid > 0 ? "открыта" : w->lid == 0 ? "закрыта" : "нет данных");
    if (w->temp_valid) { ls(&l, ", температура "); ld(&l, (uint64_t)w->temp_c); ls(&l, " °C"); }
    lflush(&l, sink);
    for (unsigned i = 0; i < S.ndev; i++) {
        if (S.dev[i].kind != K_ROOT) continue;
        char p[64]; if (aml_node_path(S.dev[i].n, p, sizeof p) <= 0) continue;
        unsigned n = prt_load(S.dev[i].n);
        ls(&l, "ACPI _PRT "); ls(&l, p); ls(&l, ": записей "); ld(&l, n); lflush(&l, sink);
        for (unsigned k = 0; k < n; k++) {
            acpidev_irq_t o; (void)prt_resolve(&prt_buf[k], &o);
            ls(&l, "  устр "); ld(&l, (prt_buf[k].addr >> 16) & 0xFFFF); ls(&l, " INT"); { char c[2] = { (char)('A' + (prt_buf[k].pin & 3)), 0 }; ls(&l, c); }
            if (o.found) { ls(&l, " -> GSI "); ld(&l, o.gsi); ls(&l, o.level ? " уровень" : " фронт"); ls(&l, o.active_low ? " активный низкий" : " активный высокий"); if (o.via_link) { ls(&l, " (связка "); ls(&l, o.link); ls(&l, ")"); } }
            else ls(&l, " -> не подведена");
            lflush(&l, sink);
        }
    }
}

void acpidev_dump(void (*sink)(const char*)) {
    if (!S.ready) { if (sink) sink("ACPI: интерпретатор AML не запущен (таблицы DSDT нет или она не принята)"); return; }
    if (!gate_take()) { if (sink) sink("ACPI: интерпретатор занят другим вызовом, повторите команду"); return; }
    DJ.sink = sink;
    run_job(dump_job, 0);
    gate_put();
}
