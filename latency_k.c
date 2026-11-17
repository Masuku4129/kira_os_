#include "latency.h"
#include "cpu.h"
#include "percpu.h"
#include "heap.h"
#include "uart.h"
#include "kptr.h"
#include "bbring.h"
#include "timer.h"

/* Подключение измерений времени реакции к ядру (А8). Что и как меряется - docs/LATENCY.md.
   Горячий путь: без замков и выделения памяти, только своё ядро пишет в свою гистограмму (latency.c). */

extern char kernel_start[];

volatile int lat_irqoff_on;                 /* флаг замера секций с выключенными прерываниями */
static lat_set_t g;
static int ready;
static uint64_t khz;                        /* частота счётчика времени, кГц */
static uint64_t mul_tsc;                    /* такты TSC -> нс (lat_mul_for_khz) */
static volatile uint32_t ioff_gen;          /* поколение включения флага: секции, начатые до включения, не считаются */

typedef struct { uint64_t t0, site; uint32_t gen, inj_n; } __attribute__((aligned(64))) ioff_t;
static ioff_t* ioff;
static lat_set_t* ioff_set = &g;            /* куда пишутся секции (на время bench - в запасной набор) */

/* Пороги записи в чёрный ящик по умолчанию - только «явно неисправно» (границы-требования лежат в политике ci/latency_policy.json и ставятся
   командой latency limit). normal порога не имеет: ждать своей очереди в круговом разделении времени ему положено. */
static const uint64_t default_limit_ns[LAT_KINDS] = { 10000000ULL, 0, 10000000ULL, 10000000ULL, 10000000ULL, 0, 50000000ULL, 50000000ULL, 0 };

static bb_gate_t gate[LAT_KINDS];

void lat_init(unsigned cpus, uint64_t tsc_khz) {
    if (ready || !cpus || !tsc_khz) return;
    lat_cpu_t* mem = (lat_cpu_t*)kmalloc((size_t)cpus * sizeof(lat_cpu_t));
    ioff = (ioff_t*)kmalloc((size_t)cpus * sizeof(ioff_t));
    if (!mem || !ioff) { uart_print("[KIRA LAT] no memory for latency tables: measurements off.\n"); return; }
    lat_set_init(&g, mem, cpus);
    for (unsigned c = 0; c < cpus; c++) { ioff[c].t0 = 0; ioff[c].site = 0; ioff[c].gen = 0; }
    for (unsigned k = 0; k < LAT_KINDS; k++) lat_set_limit(&g, k, default_limit_ns[k]);
    khz = tsc_khz;
    mul_tsc = lat_mul_for_khz(tsc_khz);
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    uart_print("[KIRA LAT] latency histograms online: "); uart_print_dec(cpus);
    uart_print(" cores, interrupt entry from the timer's own counter");
    uart_print(", scheduler wakeups, handler time; critical sections under 'latency irqoff on'.\n");
}

static inline unsigned my_cpu(void) { return this_cpu()->cpu_id; }

static inline void note(unsigned kind, uint64_t ns, uint64_t site) {
    lat_set_record(&g, my_cpu(), kind, ns, site);
}

/* Задержка прерывания таймера = от срока срабатывания до входа в обработчик. Счётчик таймера говорит, сколько прошло с ПОСЛЕДНЕГО срока (elapsed).
   Если прерывание задержалось дольше периода, несколько сроков слипаются в одно прерывание (контроллер хранит один бит запроса), и по счётчику
   видна только мелкая часть. Поэтому ядро помнит момент последнего срока, до которого прерывание дошло (percpu.lat_fire_ns): первый
   непринятый срок - на период позже него, и задержка считается от него. После остановки и запуска таймера (холостое ядро без тика) отсчёт
   начинается заново (lat_timer_restart). Всё в наносекундах по TSC, делений в 128 бит нет. */
void lat_irq_entry(uint64_t rip) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    uint64_t e, per;
    int kind = lat_arch_timer_elapsed_ns(&e, &per);
    if (!kind || !per) return;
    percpu_t* me = this_cpu();
    uint64_t now = lat_cycles_to_ns(rdtsc(), mul_tsc);
    uint64_t fire = now > e ? now - e : 0;                  /* последний срок */
    uint64_t prev = me->lat_fire_ns, d = e;
    if (kind == 1 && prev && fire > prev) {
        uint64_t n = (fire - prev + per / 2) / per;         /* сроков прошло с прошлого принятого (обычно 1) */
        if (n > 1) d = e + (n - 1) * per;                   /* первый непринятый срок - prev + per */
    }
    me->lat_fire_ns = fire ? fire : 1;
    note(me->current && me->current == me->idle ? LAT_IRQ_IDLE : LAT_IRQ, d, rip);
}

void lat_timer_restart(void) { this_cpu()->lat_fire_ns = 0; }

void lat_isr_note(uint64_t t0, uint64_t vector) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    uint64_t t1 = rdtsc();
    note(LAT_ISR, lat_cycles_to_ns(t1 > t0 ? t1 - t0 : 0, mul_tsc), vector);
}

void lat_part(unsigned part, uint64_t t0) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    uint64_t t1 = rdtsc();
    note(LAT_TICK, lat_cycles_to_ns(t1 > t0 ? t1 - t0 : 0, mul_tsc), part);
}

/* Ожидание замка: ядро крутилось от t0 до сейчас. site: замок (адрес) или (1 << 32 | номер замка планировщика). */
void lat_spin_note(uint64_t t0, uint64_t site) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    uint64_t t1 = rdtsc();
    note(LAT_SPIN, lat_cycles_to_ns(t1 > t0 ? t1 - t0 : 0, mul_tsc), site);
}

void lat_sched_note(unsigned prio, uint64_t ready_tsc, uint32_t tid) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || prio < 1 || prio > 3) return;
    uint64_t t1 = rdtsc();
    note(LAT_SCHED_NORMAL + prio - 1, lat_cycles_to_ns(t1 > ready_tsc ? t1 - ready_tsc : 0, mul_tsc), tid);
}

/* ---- критические секции ---- */

__attribute__((noinline)) void lat_irqoff_begin(uint64_t flags) {
    if (!(flags & (1ULL << 9)) || !__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;    /* вложенная: меряется внешняя */
    unsigned c = my_cpu();
    if (c >= g.ncpu) return;
    ioff[c].site = (uint64_t)(uintptr_t)__builtin_return_address(0);
    ioff[c].gen = __atomic_load_n(&ioff_gen, __ATOMIC_RELAXED);
    ioff[c].t0 = rdtsc();
}

static void irqoff_close(unsigned c) {
    uint64_t t0 = ioff[c].t0;
    if (!t0) return;
    ioff[c].t0 = 0;
    if (ioff[c].gen != __atomic_load_n(&ioff_gen, __ATOMIC_RELAXED)) return;
    uint64_t t1 = rdtsc();
    lat_set_record(ioff_set, c, LAT_IRQOFF, lat_cycles_to_ns(t1 > t0 ? t1 - t0 : 0, mul_tsc), ioff[c].site);
}

__attribute__((noinline)) void lat_irqoff_end(uint64_t flags) {
    (void)flags;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    unsigned c = my_cpu();
    if (c < g.ncpu) irqoff_close(c);
}

/* Переключение потока: секция (если открыта) заканчивается здесь, а не в чужом irq_restore другого потока. */
void lat_irqoff_cut(void) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    unsigned c = my_cpu();
    if (c < g.ncpu) irqoff_close(c);
}

/* ---- проверка границ мутацией ---- */

/* Каждый 64-й тик на каждом ядре обработчик таймера занят ещё inj_isr_ns наносекунд: это имитирует тяжёлый обработчик. Проверка границ
   (сценарий latency-mutation) обязана упасть. По умолчанию выключено; как bb pause - команда для проверки, а не режим работы. */
static volatile uint64_t inj_isr_ns;
#define LAT_INJECT_MAX_NS 50000000ULL
void lat_inject(void) {
    uint64_t n = __atomic_load_n(&inj_isr_ns, __ATOMIC_RELAXED);
    if (!n || !__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    unsigned c = my_cpu();
    if (c >= g.ncpu || (++ioff[c].inj_n & 63u)) return;
    uint64_t t_end = rdtsc() + n * khz / 1000000ULL;                              /* нс -> такты */
    while (rdtsc() < t_end) cpu_relax();
}

/* ---- чёрный ящик ---- */

void lat_flush(void) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    unsigned c = my_cpu();
    for (unsigned k = 0; k < LAT_KINDS; k++) {
        uint64_t ns, site; uint32_t n;
        if (!lat_set_take_pending(&g, c, k, &ns, &site, &n)) continue;
        uint64_t lim = g.limit[k];
        uint32_t d = kptr_in_image(site) ? (uint32_t)(site - (uint64_t)(uintptr_t)kernel_start) : 0xFFFFFFFFu;
        uint32_t skipped;
        uint8_t sev = lim && ns >= lim * 10 ? BB_SEV_SEVERE : BB_SEV_WARN;
        uint32_t a = ns > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ns;
        uint32_t cl = lim > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)lim;
        if (bb_gate_pass(&gate[k], timer_get_ticks() * 1000 / TIMER_HZ, 10, &skipped))
            bb_event(BB_LAT, sev, a, k, cl, d, n > 255 ? 255 : (uint8_t)n, lat_kind_name(k));
        else bb_note_suppressed(BB_LAT, a);
    }
}

/* ---- консоль ---- */

static int same(const char* a, const char* w) { while (*a && *a == *w) { a++; w++; } return *a == *w; }
static int starts(const char* a, const char* w) { while (*w && *a == *w) { a++; w++; } return *w == 0; }
static uint64_t parse_num(const char* s, int* ok) {
    uint64_t v = 0; int any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; any = 1; }
    while (*s == ' ') s++;
    *ok = any && !*s;
    return v;
}
static void pk(const char* label, uint64_t v) { uart_print(" "); uart_print(label); uart_print("="); uart_print_dec(v); }

static void print_site(unsigned kind, uint64_t site) {
    if (kind == LAT_IRQOFF || kind == LAT_IRQ || kind == LAT_IRQ_IDLE) { kptr_print(site); }
    else if (kind == LAT_SPIN) {
        if (site >> 32 == 1) { uart_print("замок планировщика №"); uart_print_dec(site & 0xFFFFFFFFu); }
        else { uart_print("замок "); kptr_print(site); }
    }
    else if (kind == LAT_TICK) {
        static const char* const n[LAT_PART_N] = { "?", "часы и счёт тиков", "UART (хвост строки)", "кольцо чёрного ящика (lat_flush)", "сторож", "планировщик (sched_tick)" };
        uart_print(site < LAT_PART_N ? n[site] : "?");
    }
    else if (kind == LAT_ISR) { uart_print("вектор "); uart_print_dec(site); }
    else { uart_print("поток #"); uart_print_dec(site); }
}

static void print_kind(unsigned k, int hist) {
    lat_hist_t m; lat_set_merge(&g, k, &m);
    uart_print("[LAT] "); uart_print(lat_kind_name(k)); uart_print(":");
    pk("n", m.count);
    if (m.count) {
        pk("avg", m.sum_ns / m.count);
        pk("p50", lat_hist_quantile(&m, 500000)); pk("p99", lat_hist_quantile(&m, 990000));
        pk("p99.9", lat_hist_quantile(&m, 999000)); pk("max", m.max_ns);
    }
    pk("over", m.over); pk("limit", g.limit[k]);
    uart_print(" нс");
    if (m.count) { uart_print(", максимум: "); print_site(k, m.max_site); }
    uart_print("\n");
    if (!m.count) return;
    {
        lat_site_t st[LAT_SITES]; unsigned n = lat_set_sites(&g, k, st);
        for (unsigned i = 0; i < n; i++) {
            uart_print("[LATS] "); uart_print(lat_kind_name(k)); uart_print(" место ");
            print_site(k, st[i].site);
            uart_print(": замеров >= 100 мкс "); uart_print_dec(st[i].count); uart_print(", максимум "); uart_print_dec(st[i].max_ns);
            uart_print(" нс, сумма "); uart_print_dec(st[i].sum_ns); uart_print(" нс\n");
        }
    }
    if (hist) {
        uint32_t peak = 1;
        for (unsigned b = 0; b < LAT_BINS; b++) if (m.bin[b] > peak) peak = m.bin[b];
        for (unsigned b = 0; b < LAT_BINS; b++) {
            if (!m.bin[b]) continue;
            uart_print("[LATH] "); uart_print(lat_kind_name(k)); uart_print(" ");
            uart_print_dec(lat_bin_lo(b)); uart_print("..");
            if (b == LAT_BINS - 1) uart_print("inf"); else uart_print_dec(lat_bin_hi(b));
            uart_print(" нс: "); uart_print_dec(m.bin[b]); uart_print(" ");
            unsigned w = (unsigned)((uint64_t)m.bin[b] * 40 / peak); if (!w) w = 1;
            for (unsigned i = 0; i < w; i++) uart_print("#");
            uart_print("\n");
        }
    }
    /* по ядрам: у какого ядра максимум */
    uart_print("[LATC] "); uart_print(lat_kind_name(k)); uart_print(" макс по ядрам, нс:");
    for (unsigned c = 0; c < g.ncpu; c++) {
        lat_hist_t h; lat_set_cpu_hist(&g, c, k, &h);
        if (!h.count) continue;
        uart_print(" "); uart_print_dec(c); uart_print(":"); uart_print_dec(h.max_ns);
    }
    uart_print("\n");
}

#ifdef __x86_64__
/* Цена пары irq_save/irq_restore: прежний вариант (без проверки) против нового при опущенном и поднятом флаге. */
#define BENCH_N 2000000u
static inline uint64_t raw_save(void) { uint64_t f; __asm__ volatile ("pushfq\n\tcli\n\tpopq %0" : "=r"(f) : : "memory"); return f; }
static inline void raw_restore(uint64_t f) { if (f & (1ULL << 9)) __asm__ volatile ("sti" : : : "memory"); }

static void bench(void) {
    if (!g.cpu) { uart_print("[LAT] измерения не запущены\n"); return; }
    int was = lat_irqoff_on;
    lat_irqoff_on = 0;
    __atomic_fetch_add(&ioff_gen, 1, __ATOMIC_RELAXED);
    /* запасной набор на это ядро: замеры bench не попадают в настоящие гистограммы */
    unsigned me = my_cpu();
    lat_cpu_t* mem = (lat_cpu_t*)kmalloc((size_t)(me + 1) * sizeof(lat_cpu_t));
    lat_set_t sc;
    if (!mem) { lat_irqoff_on = was; uart_print("[LAT] нет памяти для bench\n"); return; }
    lat_set_init(&sc, mem, me + 1);
    uint64_t r0, r1, r2;
    uint64_t t = rdtsc();
    for (uint32_t i = 0; i < BENCH_N; i++) { uint64_t f = raw_save(); raw_restore(f); }
    r0 = rdtsc() - t;
    t = rdtsc();
    for (uint32_t i = 0; i < BENCH_N; i++) { uint64_t f = irq_save(); irq_restore(f); }
    r1 = rdtsc() - t;
    ioff_set = &sc;
    __atomic_fetch_add(&ioff_gen, 1, __ATOMIC_RELAXED);
    lat_irqoff_on = 1;
    t = rdtsc();
    for (uint32_t i = 0; i < BENCH_N; i++) { uint64_t f = irq_save(); irq_restore(f); }
    r2 = rdtsc() - t;
    lat_irqoff_on = 0;
    ioff_set = &g;
    __atomic_fetch_add(&ioff_gen, 1, __ATOMIC_RELAXED);
    lat_irqoff_on = was;
    kfree(mem);
    uart_print("[LAT] bench irq_save+irq_restore, тактов TSC на пару: прежний код "); uart_print_dec(r0 / BENCH_N);
    uart_print(".");  uart_print_dec((r0 % BENCH_N) * 100 / BENCH_N);
    uart_print(", новый при опущенном флаге "); uart_print_dec(r1 / BENCH_N);
    uart_print("."); uart_print_dec((r1 % BENCH_N) * 100 / BENCH_N);
    uart_print(", при поднятом "); uart_print_dec(r2 / BENCH_N);
    uart_print("."); uart_print_dec((r2 % BENCH_N) * 100 / BENCH_N);
    uart_print(" (замер на ядре "); uart_print_dec(me); uart_print(", пар "); uart_print_dec(BENCH_N); uart_print(")\n");
}
#else
static void bench(void) { uart_print("[LAT] bench: замер секций с выключенными прерываниями на этой архитектуре не поддержан (docs/architecture/PARITY_ARM64.md)\n"); }
#endif

void lat_command(const char* arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg) arg = "";
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) { uart_print("[LAT] измерения не запущены (нет калибровки TSC)\n"); return; }
    if (same(arg, "reset")) {
        lat_set_reset(&g);
        uart_print("[LAT] гистограммы и счётчики превышений сброшены (пороги сохранены)\n");
        return;
    }
    if (same(arg, "bench")) { bench(); return; }
    if (starts(arg, "inject")) {
        const char* v = arg + 6; while (*v == ' ') v++;
        if (starts(v, "isr")) {
            int ok = 0; uint64_t ns = parse_num(v + 3, &ok);
            if (!ok || ns > LAT_INJECT_MAX_NS) { uart_print("latency inject isr нс (0..50000000): проверка границ - каждый 64-й тик обработчик таймера занят ещё столько нс; 0 - выключить\n"); return; }
            __atomic_store_n(&inj_isr_ns, ns, __ATOMIC_RELAXED);
            uart_print("[LAT] проверочная задержка обработчика таймера: "); uart_print_dec(ns); uart_print(" нс на каждом 64-м тике каждого ядра\n");
            return;
        }
        uart_print("latency inject isr нс\n"); return;
    }
    if (starts(arg, "irqoff")) {
        const char* v = arg + 6; while (*v == ' ') v++;
#ifndef __x86_64__
        uart_print("[LAT] замер секций с выключенными прерываниями на этой архитектуре не поддержан (отличие - docs/architecture/PARITY_ARM64.md)\n"); return;
#endif
        if (same(v, "on")) { __atomic_fetch_add(&ioff_gen, 1, __ATOMIC_RELAXED); lat_irqoff_on = 1; }
        else if (same(v, "off")) { lat_irqoff_on = 0; __atomic_fetch_add(&ioff_gen, 1, __ATOMIC_RELAXED); }
        else if (*v) { uart_print("latency irqoff on|off\n"); return; }
        uart_print("[LAT] замер секций с выключенными прерываниями: "); uart_print(lat_irqoff_on ? "включён\n" : "выключен\n");
        return;
    }
    if (starts(arg, "limit")) {
        const char* v = arg + 5; while (*v == ' ') v++;
        if (!*v) {
            for (unsigned k = 0; k < LAT_KINDS; k++) { uart_print("[LAT] порог "); uart_print(lat_kind_name(k)); uart_print(" = "); uart_print_dec(g.limit[k]); uart_print(" нс\n"); }
            return;
        }
        char name[24]; unsigned n = 0;
        while (v[n] && v[n] != ' ' && n < sizeof name - 1) { name[n] = v[n]; n++; }
        name[n] = 0;
        int k = lat_kind_from_name(name);
        int ok = 0; uint64_t ns = parse_num(v + n, &ok);
        if (k < 0 || !ok) {
            uart_print("latency limit ИМЯ нс (0 - без порога); ИМЯ: ");
            for (unsigned i = 0; i < LAT_KINDS; i++) { uart_print(lat_kind_name(i)); uart_print(i + 1 < LAT_KINDS ? ", " : "\n"); }
            return;
        }
        lat_set_limit(&g, (unsigned)k, ns);
        uart_print("[LAT] порог "); uart_print(lat_kind_name((unsigned)k)); uart_print(" = "); uart_print_dec(ns); uart_print(" нс\n");
        return;
    }
    if (*arg && !same(arg, "hist") && !same(arg, "short")) {
        uart_print("latency [short|hist] | reset | limit [ИМЯ нс] | irqoff on|off | bench | inject isr нс\n"); return;
    }
    int hist = !same(arg, "short");
    uart_print("[LAT] время реакции с последнего сброса (ядер "); uart_print_dec(g.ncpu);
    uart_print(", замер секций с выключенными прерываниями: "); uart_print(lat_irqoff_on ? "включён" : "выключен"); uart_print(")\n");
    for (unsigned k = 0; k < LAT_KINDS; k++) print_kind(k, hist);
    if (g.dropped) { uart_print("[LAT] замеров вне таблицы ядер: "); uart_print_dec(g.dropped); uart_print("\n"); }
}
