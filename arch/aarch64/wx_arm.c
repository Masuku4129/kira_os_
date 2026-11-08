#include "wx_arm.h"

#define V       1ULL                       /* valid */
#define TBL_PG  2ULL                       /* бит 1: таблица (L0-L2) или страница (L3); 0 у блока */
#define AP2     (1ULL << 7)                /* только чтение */
#define AP1     (1ULL << 6)                /* доступ из EL0 */
#define PXN     (1ULL << 53)
#define UXN     (1ULL << 54)
#define OA      0x0000FFFFFFFFF000ULL
#define LOWATTR 0x0FFCULL                  /* биты 11:2: AttrIndx, NS, AP, SH, AF, nG */
#define UPATTR  0xFFF0000000000000ULL      /* биты 63:52 */
#define PAGE    4096ULL

static int hits(uint64_t lo, uint64_t hi, uint64_t a, uint64_t b) { return lo < hi && a < hi && lo < b; }
static int in_image(const wx_layout_t* l, uint64_t a, uint64_t b) {
    return hits(l->text_lo, l->text_hi, a, b) || hits(l->ro_lo, l->ro_hi, a, b) || hits(l->data_lo, l->data_hi, a, b);
}
static unsigned attr_idx(uint64_t e) { return (unsigned)(e >> 2) & 7; }

// Права страницы образа по адресу: возвращает биты AP2/PXN/UXN (прочие атрибуты страницы не трогаются).
static uint64_t page_rights(const wx_layout_t* l, uint64_t va) {
    if (va >= l->text_lo && va < l->text_hi) return AP2 | UXN;                 // RX для ядра
    if (va >= l->ro_lo && va < l->ro_hi) return AP2 | PXN | UXN;               // чтение
    return PXN | UXN;                                                          // данные: RW без исполнения
}

// Все ненулевые записи уровня level (1..3) под таблицей t с базой base; leaf-записи получают XN, образ дробится.
static int enforce_level(const wx_env_t* env, uint64_t* t, int level, uint64_t base, const wx_layout_t* l) {
    unsigned shift = 12 + 9 * (unsigned)(3 - level);
    uint64_t span = 1ULL << shift;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & V)) continue;
        uint64_t va = base + (uint64_t)i * span;
        int is_table = level < 3 && (e & TBL_PG);
        if (is_table) {
            if (enforce_level(env, env->ptr(e & OA), level + 1, va, l) != 0) return -1;
            continue;
        }
        int img = in_image(l, va, va + span);
        if (!img) { t[i] = e | PXN | UXN; continue; }
        if (level == 3) {                                                       // страница образа: права по секции
            if (attr_idx(e) != 2) return -1;
            t[i] = (e & ~(AP2 | PXN | UXN)) | page_rights(l, va);
            continue;
        }
        if (attr_idx(e) != 2) return -1;                                        // блок под образом - обычная память
        uint64_t np = env->alloc();
        if (!np) return -1;
        uint64_t* n = env->ptr(np);
        uint64_t attr = (e & (LOWATTR | UPATTR)) & ~(AP2 | PXN | UXN);
        uint64_t child = level == 2 ? TBL_PG | V : V;                           // страница L3: биты [1:0] = 11; блок L2: 01
        uint64_t sub = span / 512;
        for (unsigned k = 0; k < 512; k++) {
            uint64_t pa = (e & OA & ~(span - 1)) + (uint64_t)k * sub;
            uint64_t pva = va + (uint64_t)k * sub;
            uint64_t rights = (sub == PAGE && in_image(l, pva, pva + PAGE)) ? page_rights(l, pva) : (e & AP2) | PXN | UXN;
            n[k] = pa | attr | rights | child;
        }
        t[i] = np | TBL_PG | V;                                                 // дочерняя таблица готова до подмены записи
        if (enforce_level(env, n, level + 1, va, l) != 0) return -1;            // 2-МБ блоки под образом дробятся дальше
    }
    return 0;
}

int wxa_enforce(const wx_env_t* env, uint64_t* l0, const wx_layout_t* l) {
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = l0[i];
        if (!(e & V)) continue;
        if (!(e & TBL_PG)) return -1;                                           // блоков на уровне L0 не бывает
        if (enforce_level(env, env->ptr(e & OA), 1, (uint64_t)i << 39, l) != 0) return -1;
    }
    return 0;
}

static void leaf(wx_stats_t* st, uint64_t va, uint64_t size, uint64_t e) {
    int w1 = !(e & AP2), x1 = !(e & PXN);
    int w0 = (e & (AP1 | AP2)) == AP1, x0 = !(e & UXN);
    if ((w1 && x1) || (w0 && x0)) {
        if (!st->bad_count) st->bad_va = va;
        st->bad_count++;
        return;
    }
    if (attr_idx(e) < 2) return;                                                // устройства (MAIR 0, 1) в итог не входят
    if (x1 || x0) st->exec_kb += size >> 10;
    else if (w1 || w0) st->rw_kb += size >> 10;
    else st->ro_kb += size >> 10;
}

static void walk(const wx_env_t* env, const uint64_t* t, int level, uint64_t base, wx_stats_t* st) {
    unsigned shift = 12 + 9 * (unsigned)(3 - level);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & V)) continue;
        uint64_t va = base + ((uint64_t)i << shift);
        if (level < 3 && (e & TBL_PG)) walk(env, env->ptr(e & OA), level + 1, va, st);
        else if (level == 3 && !(e & TBL_PG)) continue;                         // L3 с битом 1 = 0 - зарезервировано, не отображение
        else leaf(st, va, 1ULL << shift, e);
    }
}

static int lookup(const wx_env_t* env, const uint64_t* l0, uint64_t va, uint64_t* out) {
    const uint64_t* t = l0;
    for (int level = 0; level < 4; level++) {
        uint64_t e = t[(va >> (39 - 9 * level)) & 511];
        if (!(e & V)) return -1;
        if (level == 3) { if (!(e & TBL_PG)) return -1; *out = e; return 0; }
        if (!(e & TBL_PG)) { if (level == 0) return -1; *out = e; return 0; }
        t = env->ptr(e & OA);
    }
    return -1;
}

static void check_range(const wx_env_t* env, const uint64_t* l0, wx_stats_t* st, uint64_t lo, uint64_t hi, int want_w, int want_x) {
    for (uint64_t va = lo; va < hi; va += PAGE) {
        uint64_t e;
        if (lookup(env, l0, va, &e) != 0 || (!(e & AP2)) != want_w || (!(e & PXN)) != want_x) {
            if (st->layout_ok) st->layout_bad_va = va;
            st->layout_ok = 0;
            return;
        }
    }
}

int wxa_scan(const wx_env_t* env, const uint64_t* l0, const wx_layout_t* l, wx_stats_t* st) {
    st->exec_kb = st->ro_kb = st->rw_kb = 0;
    st->bad_count = st->bad_va = 0;
    st->layout_ok = 1;
    st->layout_bad_va = 0;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = l0[i];
        if ((e & V) && (e & TBL_PG)) walk(env, env->ptr(e & OA), 1, (uint64_t)i << 39, st);
    }
    check_range(env, l0, st, l->text_lo, l->text_hi, 0, 1);
    check_range(env, l0, st, l->ro_lo, l->ro_hi, 0, 0);
    check_range(env, l0, st, l->data_lo, l->data_hi, 1, 0);
    return (st->bad_count == 0 && st->layout_ok) ? 0 : -1;
}
