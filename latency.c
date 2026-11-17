#include "latency.h"

/* Писатель один (своё ядро): чтение и запись релаксированные, читатель с другого ядра видит целые слова. */
#define LD(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)
#define ST(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELAXED)

/* Корзины: 0 - до 16 нс; дальше по 4 корзины на степень двойки: v в [2^o, 2^(o+1)), o >= 4, корзина 1 + (o-4)*4 + (v >> (o-2) & 3). */
unsigned lat_bin(uint64_t ns) {
    if (ns < 16) return 0;
    unsigned o = 63u - (unsigned)__builtin_clzll(ns);
    unsigned b = 1u + (o - 4u) * 4u + (unsigned)((ns >> (o - 2u)) & 3u);
    return b < LAT_BINS ? b : LAT_BINS - 1;
}

uint64_t lat_bin_lo(unsigned b) {
    if (b == 0) return 0;
    if (b >= LAT_BINS) b = LAT_BINS - 1;
    unsigned o = 4u + (b - 1u) / 4u, q = (b - 1u) % 4u;
    return (1ULL << o) + ((uint64_t)q << (o - 2u));
}

uint64_t lat_bin_hi(unsigned b) {
    if (b >= LAT_BINS - 1) return ~0ULL;
    return lat_bin_lo(b + 1);
}

void lat_hist_clear(lat_hist_t* h) {
    ST(h->count, 0); ST(h->sum_ns, 0); ST(h->max_ns, 0); ST(h->max_site, 0); ST(h->over, 0);
    for (unsigned i = 0; i < LAT_BINS; i++) ST(h->bin[i], 0);
}

int lat_hist_add(lat_hist_t* h, uint64_t ns, uint64_t site, uint64_t limit_ns) {
    unsigned b = lat_bin(ns);
    ST(h->bin[b], LD(h->bin[b]) + 1);
    ST(h->count, LD(h->count) + 1);
    ST(h->sum_ns, LD(h->sum_ns) + ns);
    if (ns > LD(h->max_ns)) { ST(h->max_ns, ns); ST(h->max_site, site); }
    if (limit_ns && ns > limit_ns) { ST(h->over, LD(h->over) + 1); return 1; }
    return 0;
}

void lat_hist_merge(lat_hist_t* d, const lat_hist_t* s) {
    for (unsigned i = 0; i < LAT_BINS; i++) d->bin[i] += LD(s->bin[i]);
    d->count += LD(s->count);
    d->sum_ns += LD(s->sum_ns);
    d->over += LD(s->over);
    uint64_t m = LD(s->max_ns);
    if (m > d->max_ns) { d->max_ns = m; d->max_site = LD(s->max_site); }
}

uint64_t lat_hist_quantile(const lat_hist_t* h, uint32_t ppm) {
    uint64_t total = 0;
    for (unsigned i = 0; i < LAT_BINS; i++) total += LD(h->bin[i]);     /* по корзинам, а не по count: чтение живое, итог должен сходиться с ними */
    if (!total) return 0;
    if (ppm > 1000000u) ppm = 1000000u;
    /* ранг (с единицы): наименьшее r, при котором доля замеров не выше r не меньше ppm/1e6; r = ceil(total * ppm / 1e6) */
    uint64_t rank = (total / 1000000u) * ppm + ((total % 1000000u) * ppm + 999999u) / 1000000u;     /* без 128-битного деления: его нет в ядре */
    if (rank < 1) rank = 1;
    uint64_t acc = 0;
    for (unsigned i = 0; i < LAT_BINS; i++) {
        acc += LD(h->bin[i]);
        if (acc >= rank) {
            uint64_t hi = lat_bin_hi(i), mx = LD(h->max_ns);
            return hi < mx ? hi : mx;
        }
    }
    return LD(h->max_ns);
}

uint64_t lat_mul_for_khz(uint64_t khz) {
    if (!khz) return 0;
    return (1000000ULL << 32) / khz;
}

uint64_t lat_cycles_to_ns(uint64_t cycles, uint64_t mul) {
    return (uint64_t)(((unsigned __int128)cycles * mul) >> 32);
}

static const char* const kind_names[LAT_KINDS] = { "irq", "irq-idle", "isr", "irqoff", "spin", "sched-normal", "sched-critical", "sched-realtime", "tick-part" };
const char* lat_kind_name(unsigned k) { return k < LAT_KINDS ? kind_names[k] : "?"; }
int lat_kind_from_name(const char* n) {
    if (!n) return -1;
    for (int k = 0; k < LAT_KINDS; k++) {
        const char* a = n; const char* b = kind_names[k];
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return k;
    }
    return -1;
}

void lat_set_init(lat_set_t* s, lat_cpu_t* mem, unsigned ncpu) {
    s->cpu = mem; s->ncpu = ncpu; s->epoch = 0; s->dropped = 0;
    for (unsigned k = 0; k < LAT_KINDS; k++) s->limit[k] = 0;
    for (unsigned c = 0; c < ncpu; c++) {
        mem[c].epoch = 0;
        for (unsigned k = 0; k < LAT_KINDS; k++) {
            lat_hist_clear(&mem[c].h[k]); mem[c].pend_ns[k] = mem[c].pend_site[k] = 0; mem[c].pend_n[k] = 0;
            for (unsigned i = 0; i < LAT_SITES; i++) mem[c].sites[k][i].site = mem[c].sites[k][i].count = mem[c].sites[k][i].sum_ns = mem[c].sites[k][i].max_ns = 0;
        }
    }
}

/* Таблица мест (писатель один): есть место - прибавить; нет - вытеснить запись с наименьшим максимумом, если новый замер длиннее. */
static void sites_add(lat_site_t* t, uint64_t site, uint64_t ns) {
    unsigned free_i = LAT_SITES, min_i = 0;
    for (unsigned i = 0; i < LAT_SITES; i++) {
        if (LD(t[i].count) && LD(t[i].site) == site) {
            ST(t[i].count, LD(t[i].count) + 1); ST(t[i].sum_ns, LD(t[i].sum_ns) + ns);
            if (ns > LD(t[i].max_ns)) ST(t[i].max_ns, ns);
            return;
        }
        if (!LD(t[i].count)) { if (free_i == LAT_SITES) free_i = i; }
        else if (LD(t[i].max_ns) < LD(t[min_i].max_ns) || !LD(t[min_i].count)) min_i = i;
    }
    unsigned k = free_i != LAT_SITES ? free_i : min_i;
    if (free_i == LAT_SITES && ns <= LD(t[k].max_ns)) return;
    ST(t[k].site, site); ST(t[k].count, 1); ST(t[k].sum_ns, ns); ST(t[k].max_ns, ns);
}

/* Ядро прежней эпохи обнуляет себя само при первой записи (или выдаче превышения). */
static void sync_epoch(const lat_set_t* s, lat_cpu_t* c) {
    uint32_t e = LD(s->epoch);
    if (LD(c->epoch) == e) return;
    for (unsigned k = 0; k < LAT_KINDS; k++) {
        lat_hist_clear(&c->h[k]); c->pend_ns[k] = c->pend_site[k] = 0; c->pend_n[k] = 0;
        for (unsigned i = 0; i < LAT_SITES; i++) { ST(c->sites[k][i].count, 0); ST(c->sites[k][i].site, 0); ST(c->sites[k][i].sum_ns, 0); ST(c->sites[k][i].max_ns, 0); }
    }
    ST(c->epoch, e);
}

int lat_set_record(lat_set_t* s, unsigned cpu, unsigned kind, uint64_t ns, uint64_t site) {
    if (kind >= LAT_KINDS) return 0;
    if (cpu >= s->ncpu) { __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED); return 0; }
    lat_cpu_t* c = &s->cpu[cpu];
    sync_epoch(s, c);
    int over = lat_hist_add(&c->h[kind], ns, site, LD(s->limit[kind]));
    if (ns >= LAT_SITE_MIN_NS) sites_add(c->sites[kind], site, ns);
    if (over) {
        c->pend_n[kind]++;
        if (ns > c->pend_ns[kind]) { c->pend_ns[kind] = ns; c->pend_site[kind] = site; }
    }
    return over;
}

void lat_set_reset(lat_set_t* s) { __atomic_fetch_add(&s->epoch, 1, __ATOMIC_RELEASE); }

int lat_set_limit(lat_set_t* s, unsigned kind, uint64_t ns) {
    if (kind >= LAT_KINDS) return -1;
    ST(s->limit[kind], ns);
    return 0;
}

int lat_set_cpu_hist(const lat_set_t* s, unsigned cpu, unsigned kind, lat_hist_t* out) {
    lat_hist_clear(out);
    if (cpu >= s->ncpu || kind >= LAT_KINDS) return -1;
    const lat_cpu_t* c = &s->cpu[cpu];
    if (LD(c->epoch) != LD(s->epoch)) return 0;
    lat_hist_merge(out, &c->h[kind]);
    return 0;
}

void lat_set_merge(const lat_set_t* s, unsigned kind, lat_hist_t* out) {
    lat_hist_clear(out);
    for (unsigned c = 0; c < s->ncpu; c++) {
        lat_hist_t t;
        if (lat_set_cpu_hist(s, c, kind, &t) == 0) lat_hist_merge(out, &t);
    }
}

int lat_set_take_pending(lat_set_t* s, unsigned cpu, unsigned kind, uint64_t* ns, uint64_t* site, uint32_t* n) {
    if (cpu >= s->ncpu || kind >= LAT_KINDS) return 0;
    lat_cpu_t* c = &s->cpu[cpu];
    if (LD(c->epoch) != LD(s->epoch)) return 0;
    if (!c->pend_n[kind]) return 0;
    *ns = c->pend_ns[kind]; *site = c->pend_site[kind]; *n = c->pend_n[kind];
    c->pend_ns[kind] = c->pend_site[kind] = 0; c->pend_n[kind] = 0;
    return 1;
}

unsigned lat_set_sites(const lat_set_t* s, unsigned kind, lat_site_t* out) {
    lat_site_t acc[64];                                       /* 2 КиБ на стеке; различных мест больше 64 - лишние отбрасываются */
    unsigned cap = sizeof acc / sizeof acc[0], n = 0;
    if (kind >= LAT_KINDS) return 0;
    for (unsigned c = 0; c < s->ncpu; c++) {
        const lat_cpu_t* cp = &s->cpu[c];
        if (LD(cp->epoch) != LD(s->epoch)) continue;
        for (unsigned i = 0; i < LAT_SITES; i++) {
            uint64_t cnt = LD(cp->sites[kind][i].count);
            if (!cnt) continue;
            uint64_t site = LD(cp->sites[kind][i].site), sum = LD(cp->sites[kind][i].sum_ns), mx = LD(cp->sites[kind][i].max_ns);
            unsigned j = 0;
            while (j < n && acc[j].site != site) j++;
            if (j == n) { if (n == cap) continue; acc[n].site = site; acc[n].count = acc[n].sum_ns = acc[n].max_ns = 0; n++; }
            acc[j].count += cnt; acc[j].sum_ns += sum; if (mx > acc[j].max_ns) acc[j].max_ns = mx;
        }
    }
    for (unsigned i = 1; i < n; i++) {                       /* по убыванию максимума (вставками: n мало) */
        lat_site_t t = acc[i]; unsigned j = i;
        while (j && acc[j - 1].max_ns < t.max_ns) { acc[j] = acc[j - 1]; j--; }
        acc[j] = t;
    }
    if (n > LAT_SITES) n = LAT_SITES;
    for (unsigned i = 0; i < n; i++) out[i] = acc[i];
    return n;
}
