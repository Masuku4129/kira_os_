#include "smp.h"
#include "percpu.h"
#include "apic.h"
#include "clock.h"
#include "cpu.h"
#include "fpu.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "syscall.h"
#include "uart.h"
#include "vmm.h"
#include "sched.h"
#include "spinlock.h"
#include "proc.h"

uint64_t smp_work_checksum(uint64_t iters) {
    uint64_t x = 1, s = 0;
    for (uint64_t i = 0; i < iters; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s = (s ^ (x >> 11)) * 0x9E3779B97F4A7C15ULL;
    }
    return s;
}

unsigned smp_plan(const uint32_t* ids, unsigned n, uint32_t bsp_apic_id, uint32_t* out, unsigned max) {
    unsigned k = 0;
    for (unsigned i = 0; i < n && k < max; i++) {
        if (ids[i] == bsp_apic_id) continue;
        int dup = 0;
        for (unsigned j = 0; j < k; j++) if (out[j] == ids[i]) dup = 1;
        if (!dup) out[k++] = ids[i];
    }
    return k;
}

#ifndef KIRA_HOST_TEST

extern char ap_tramp_start[], ap_tramp_end[];
extern const char ap_tramp_params_off[];       /* абсолютный символ: смещение блока параметров в трамплине */

typedef struct { uint64_t cr3, stack_top, entry, arg; } ap_params_t;

static void delay_us(uint64_t us) {
    uint64_t end = clock_ns() + us * 1000;
    while (clock_ns() < end) __asm__ volatile ("pause");
}

// ---- Сверка TSC между ядрами ----
// Часы ядра (clock_ns) читают TSC того ядра, где выполняется код. Если счётчики ядер расходятся, время, измеренное на
// разных ядрах, нельзя сравнивать. При запуске каждое ядро отвечает на SYNC_ROUNDS запросов своим TSC; по самому
// быстрому обмену загрузочное ядро оценивает расхождение (с точностью до половины времени обмена).
#define SYNC_ROUNDS 8
static volatile uint32_t sync_ready, sync_req, sync_ack;
static volatile uint64_t sync_tsc;

static void tsc_sync_ap(void) {
    uint64_t limit = clock_tsc_khz() * 50;                       /* 50 мс на раунд: загрузочное ядро не ответило - идём дальше */
    __atomic_store_n(&sync_ready, 1, __ATOMIC_RELEASE);
    for (uint32_t k = 1; k <= SYNC_ROUNDS; k++) {
        uint64_t start = rdtsc();
        while (__atomic_load_n(&sync_req, __ATOMIC_ACQUIRE) != k) {
            if (rdtsc() - start > limit) return;
            __asm__ volatile ("pause");
        }
        sync_tsc = rdtsc();
        __atomic_store_n(&sync_ack, k, __ATOMIC_RELEASE);
    }
}

static void tsc_sync_bsp(percpu_t* p) {
    uint64_t best = ~0ULL; int64_t skew = 0;
    for (uint32_t k = 1; k <= SYNC_ROUNDS; k++) {
        uint64_t t0 = rdtsc();
        __atomic_store_n(&sync_req, k, __ATOMIC_RELEASE);
        while (__atomic_load_n(&sync_ack, __ATOMIC_ACQUIRE) != k) {
            if (rdtsc() - t0 > clock_tsc_khz() * 50) return;
            __asm__ volatile ("pause");
        }
        uint64_t t1 = rdtsc();
        if (t1 - t0 < best) { best = t1 - t0; skew = (int64_t)(sync_tsc - (t0 + (t1 - t0) / 2)); }
    }
    p->tsc_skew = skew;
    p->tsc_rtt = best;
}

// Вход не загрузочного процессора (из трамплина, на своём стеке). Сюда не возвращаются.
void smp_ap_main(uint64_t idx) {
    percpu_t* p = &percpu_table[idx];
    percpu_install(p);                           /* gdt_init_ap берёт замок кучи, а замки считаются на ядро */
    void* tss = gdt_init_ap();
    if (!tss) for (;;) __asm__ volatile ("cli; hlt");
    percpu_install(p);                           /* после GDT: загрузка селектора в %gs обнуляет GS_BASE */
    p->tss = tss;
    idt_load_this_cpu();
    fpu_init_cpu();
    syscall_init_cpu();
    vmm_cpu_init_ap();
    apic_enable_this_cpu();
    p->active_cr3 = vmm_kernel_root();
    tsc_sync_ap();
    /* Ядро вступает в планирование только вместе с тиком LAPIC (тот же счёт, что у загрузочного ядра): без тика
       у него не было бы ни вытеснения, ни пробуждения спящих потоков. */
    uint32_t tick = apic_timer_initial_count();
    if (tick) {
        if (sched_attach_cpu() != 0) {          /* таблица потоков полна: ядро честно сообщает и не участвует */
            uart_print("[KIRA SMP] A core could not join the scheduler (thread table full): left out of service.\n");
            __atomic_store_n(&p->failed, 1, __ATOMIC_RELEASE);
            for (;;) __asm__ volatile ("cli; hlt");
        }
        apic_timer_start_periodic(32, tick);
    }
    __atomic_store_n(&p->online, 1, __ATOMIC_RELEASE);
    for (;;) {                                  /* стек запуска - холостой поток этого ядра */
        smp_idle_tick_off();
        __asm__ volatile ("sti; hlt");
    }
}

#include "irqmode.h"
#include "tlb_shootdown.h"
static volatile unsigned timekeeper;     /* ядро, которое тикает всегда (0 - загрузочное, пока оно живо) */

// ---- Сброс TLB на других ядрах ----
// Одна рассылка за раз (sd_busy). Поколение и ack связывают подтверждение с конкретным запросом.
// По истечении 200 мс печатается предупреждение, но запрос и блокировка остаются активными до ответа всех CPU:
// иначе страницы могут быть освобождены при живой старой трансляции.
static volatile uint64_t sd_root, sd_va;
static volatile uint32_t sd_pages;
static volatile uint64_t sd_generation;
static volatile uint64_t sd_ack[CPU_MAX];
static volatile int sd_busy;

static void tlb_flush_local(uint64_t root, uint64_t va, uint32_t pages) {
    if (__atomic_load_n(&this_cpu()->active_cr3, __ATOMIC_ACQUIRE) != root) return;
    if (pages > 32) { uint64_t cr3; __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory"); return; }
    for (uint32_t i = 0; i < pages; i++) __asm__ volatile ("invlpg (%0)" : : "r"(va + (uint64_t)i * 4096) : "memory");
}

void vmm_tlb_shootdown(uint64_t root, uint64_t va, uint32_t pages) {
    uint64_t f = irq_save();
    tlb_flush_local(root, va, pages);
    irq_restore(f);
    if (percpu_count() < 2 || !apic_is_enabled()) return;
    while (__atomic_exchange_n(&sd_busy, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause");
    unsigned me = this_cpu()->cpu_id;
    cpumask_t mask = cm_none();
    for (unsigned i = 0; i < percpu_count(); i++) {
        percpu_t* p = &percpu_table[i];
        if (i == me || !p->online || p->failed) continue;
        if (__atomic_load_n(&p->active_cr3, __ATOMIC_SEQ_CST) == root) cm_set(&mask, i);
    }
    if (!cm_empty(&mask)) {
        uint64_t generation = __atomic_load_n(&sd_generation, __ATOMIC_RELAXED) + 1;
        if (!generation) generation = 1; /* нулевое поколение зарезервировано как «нет ack» */
        sd_root = root; sd_va = va; sd_pages = pages;
        __atomic_store_n(&sd_generation, generation, __ATOMIC_RELEASE); /* публикуем payload до отправки IPI */
        for (unsigned i = 0; i < percpu_count(); i++)
            if (cm_test(&mask, i)) apic_send_ipi(percpu_table[i].apic_id, ICR_FIXED(IPI_TLB_VECTOR));

        uint64_t t0 = rdtsc(), limit = clock_tsc_khz() * 200;
        int warned = 0;
        while (!tlb_shootdown_all_acked(&mask, sd_ack, generation)) {
            if (!warned && limit && rdtsc() - t0 > limit) {
                uart_print("[KIRA SMP] TLB shootdown: CPU acknowledgement exceeded 200 ms; holding request and backing memory until every target responds.\\n");
                warned = 1;
            }
            __asm__ volatile ("pause");
        }
    }
    __atomic_store_n(&sd_busy, 0, __ATOMIC_RELEASE);
}

void ipi_dispatch(uint64_t vector, uint64_t cs) {
    percpu_t* c = this_cpu();
    c->irq_depth++;
    if (c->cpu_id == timekeeper) smp_idle_tick_on();                /* роль хранителя времени перешла к этому ядру */
    if (vector == IPI_RESCHED_VECTOR) c->need_resched = 1;           // выбор потока - ниже, после EOI
    else if (vector == IPI_TLB_VECTOR) {
        uint64_t generation = __atomic_load_n(&sd_generation, __ATOMIC_ACQUIRE);
        tlb_flush_local(sd_root, sd_va, sd_pages);
        __atomic_store_n(&sd_ack[c->cpu_id], generation, __ATOMIC_RELEASE);
    }
    else __atomic_fetch_add(&c->ipi_count, 1, __ATOMIC_RELAXED);
    apic_send_eoi();
    c->irq_depth--;
    if ((cs & 3) == 3) proc_return_check();
    sched_irq_exit();
}

static volatile int halting;
int smp_halting(void) { return __atomic_load_n(&halting, __ATOMIC_ACQUIRE); }

void smp_halt_others(void) {
    if (__atomic_exchange_n(&halting, 1, __ATOMIC_ACQ_REL)) {      // авария уже ведётся на другом ядре
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (percpu_count() > 1) apic_send_ipi(0, ICR_NMI_ALL_BUT_SELF);
}

unsigned smp_timekeeper(void) { return timekeeper; }

void smp_idle_tick_off(void) {
    percpu_t* c = this_cpu();
    if (c->tick_off || c->cpu_id == timekeeper || !irqmode_is_apic() || !apic_timer_initial_count()) return;
    uint64_t f = irq_save();
    if (c->sched_on && c->current == c->idle && !c->need_resched) {
        apic_timer_mask_local();
        c->tick_off = 1;
    }
    irq_restore(f);
}

void smp_idle_tick_on(void) {
    percpu_t* c = this_cpu();
    if (!c->tick_off) return;
    c->tick_off = 0;
    apic_timer_start_periodic(32, apic_timer_initial_count());
}

void smp_timekeeper_failed(unsigned failed_cpu) {
    if (timekeeper != failed_cpu) return;
    for (unsigned i = 0; i < percpu_count(); i++) {
        percpu_t* p = &percpu_table[i];
        if (i == failed_cpu || !p->sched_on || !p->online || p->failed) continue;
        timekeeper = i;
        apic_send_ipi(p->apic_id, ICR_FIXED(IPI_RESCHED_VECTOR));   /* обработчик включит ему тик */
        return;
    }
}

void smp_kick_idle(cpumask_t affinity) {
    percpu_t* me = this_cpu();
    for (unsigned i = 0; i < percpu_count(); i++) {
        percpu_t* p = &percpu_table[i];
        if (p == me || !p->sched_on || !cm_test(&affinity, i)) continue;
        if (p->current != p->idle || p->need_resched) continue;     // это ядро занято или уже разбужено
        apic_send_ipi(p->apic_id, ICR_FIXED(IPI_RESCHED_VECTOR));
        return;
    }
}

void smp_resched_cpu(unsigned cpu) {
    if (cpu >= percpu_count()) return;
    percpu_t* p = &percpu_table[cpu];
    if (p == this_cpu() || !p->sched_on || p->need_resched) return;
    apic_send_ipi(p->apic_id, ICR_FIXED(IPI_RESCHED_VECTOR));
}

unsigned smp_online(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < percpu_count(); i++) if (percpu_table[i].online) n++;
    return n;
}

static int start_one(percpu_t* p) {
    uint8_t* raw = (uint8_t*)kmalloc(SMP_AP_STACK);          /* блоки кучи выровнены не меньше чем по 16 байт */
    if (!raw) return -1;
    uint64_t top = (uint64_t)(uintptr_t)raw + SMP_AP_STACK;
    uint8_t* dst = (uint8_t*)(uintptr_t)SMP_TRAMP_BASE;
    uint64_t len = (uint64_t)(ap_tramp_end - ap_tramp_start);
    for (uint64_t i = 0; i < len; i++) dst[i] = (uint8_t)ap_tramp_start[i];
    ap_params_t* par = (ap_params_t*)(uintptr_t)(SMP_TRAMP_BASE + (uint64_t)(uintptr_t)ap_tramp_params_off);
    par->cr3 = vmm_kernel_root();
    par->stack_top = top;
    par->entry = (uint64_t)(uintptr_t)smp_ap_main;
    par->arg = p->cpu_id;
    sync_ready = 0; sync_req = 0; sync_ack = 0;
    __sync_synchronize();

    apic_send_ipi(p->apic_id, ICR_INIT_ASSERT);
    delay_us(10000);
    apic_send_ipi(p->apic_id, ICR_INIT_DEASSERT);
    for (int attempt = 0; attempt < 2 && !__atomic_load_n(&sync_ready, __ATOMIC_ACQUIRE); attempt++) {
        apic_send_ipi(p->apic_id, ICR_SIPI(SMP_TRAMP_BASE >> 12));
        delay_us(300);
    }
    for (int i = 0; i < 500 && !__atomic_load_n(&sync_ready, __ATOMIC_ACQUIRE); i++) delay_us(1000);
    if (!__atomic_load_n(&sync_ready, __ATOMIC_ACQUIRE)) return -2;     /* ядро не дошло до своего кода */
    {
        uint64_t f = irq_save();                                 /* прерывание посреди обмена исказило бы замер */
        tsc_sync_bsp(p);
        irq_restore(f);
    }
    for (int i = 0; i < 500 && !__atomic_load_n(&p->online, __ATOMIC_ACQUIRE) && !__atomic_load_n(&p->failed, __ATOMIC_ACQUIRE); i++) delay_us(1000);
    if (__atomic_load_n(&p->failed, __ATOMIC_ACQUIRE)) return -3;
    return __atomic_load_n(&p->online, __ATOMIC_ACQUIRE) ? 0 : -2;
}

unsigned smp_start(const acpi_info_t* acpi) {
    percpu_table[0].online = 1;
    if (!acpi || !acpi->valid || acpi->cpu_count < 2 || !apic_is_enabled() || !clock_ready()) {
        uart_print("[KIRA SMP] One processor: ");
        uart_print(!acpi || !acpi->valid ? "no processor list from firmware (ACPI MADT / Intel MP)" : acpi->cpu_count < 2 ? "firmware lists one processor"
                   : !apic_is_enabled() ? "no Local APIC" : "no clock");
        uart_print(" - other cores are not started.\n");
        return 0;
    }
    uint32_t ap[PERCPU_MAX_CPUS];
    unsigned n = acpi->cpu_count < ACPI_MAX_CPU ? acpi->cpu_count : ACPI_MAX_CPU;
    unsigned cnt = smp_plan(acpi->cpu_apic_id, n, apic_get_id(), ap, PERCPU_MAX_CPUS - 1);
    unsigned ok = 0;
    for (unsigned i = 0; i < cnt; i++) {
        percpu_t* p = percpu_alloc(ap[i]);
        if (!p) break;
        int rc = start_one(p);
        if (rc == 0) {
            ok++;
        } else {
            uart_print("[KIRA SMP] Processor with APIC id "); uart_print_dec(ap[i]);
            uart_print(rc == -1 ? " not started: no memory for its stack.\n" : rc == -3 ? " started but could not enter the scheduler.\n" : " did not answer INIT-SIPI within 0.5 s.\n");
        }
    }
    uart_print("[KIRA SMP] Processors online: "); uart_print_dec(1 + ok);
    uart_print(" of "); uart_print_dec(acpi->cpu_count); uart_print(acpi->source ? " (Intel MP tables).\n" : " (MADT).\n");
    if (acpi->cpu_count > PERCPU_MAX_CPUS)
        uart_print("[KIRA SMP] More processors than the core table holds (CPU_MAX): the rest stay unused.\n");

    // Проверка доставки между ядрами: пинг каждому запущенному, ответ - рост счётчика у получателя.
    unsigned pong = 0;
    for (unsigned i = 1; i < percpu_count(); i++) {
        percpu_t* p = &percpu_table[i];
        if (!p->online) continue;
        uint64_t before = __atomic_load_n(&p->ipi_count, __ATOMIC_ACQUIRE);
        apic_send_ipi(p->apic_id, ICR_FIXED(IPI_PING_VECTOR));
        for (int k = 0; k < 50 && __atomic_load_n(&p->ipi_count, __ATOMIC_ACQUIRE) == before; k++) delay_us(1000);
        if (__atomic_load_n(&p->ipi_count, __ATOMIC_ACQUIRE) != before) pong++;
    }
    if (ok) {
        uart_print("[KIRA SMP] IPI ping answered by "); uart_print_dec(pong);
        uart_print(" of "); uart_print_dec(ok);
        uart_print(" started processors. Each core runs its own scheduler tick and its own ready queue; idle cores take waiting work from busy ones.\n");
        uint64_t worst = 0, rtt = 0;
        for (unsigned i = 1; i < percpu_count(); i++) {
            const percpu_t* p = &percpu_table[i];
            if (!p->online || !p->tsc_rtt) continue;
            uint64_t a = (uint64_t)(p->tsc_skew < 0 ? -p->tsc_skew : p->tsc_skew);
            if (a > worst) { worst = a; rtt = p->tsc_rtt; }
            if (p->tsc_rtt > rtt && a == worst) rtt = p->tsc_rtt;
        }
        uint64_t khz = clock_tsc_khz();
        uart_print("[KIRA SMP] TSC skew between cores: max ");
        uart_print_dec(khz ? worst * 1000000 / khz : worst);
        uart_print(" ns (measurement precision +-");
        uart_print_dec(khz ? rtt * 500000 / khz : rtt);
        uart_print(" ns)");
        uart_print(worst > rtt + khz / 10 ? ": WARNING, clocks read on different cores disagree by more than 100 us.\n" : ": clocks agree.\n");
    }
    return ok;
}

static void put_dec(void (*out)(const char*), uint64_t v, unsigned width) {
    char t[21], b[22]; int k = 0; unsigned n = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 20);
    for (unsigned pad = (unsigned)k; pad < width; pad++) b[n++] = ' ';
    while (k) b[n++] = t[--k];
    b[n] = 0; out(b);
}

void smp_describe(void (*out)(const char*)) {
    out("\n  CPU  APIC  STATE     IPI  SWITCHES  BUSY\n");
    for (unsigned i = 0; i < percpu_count(); i++) {
        const percpu_t* p = &percpu_table[i];
        out("  "); put_dec(out, i, 3); out("  "); put_dec(out, p->apic_id, 4);
        out(p->failed ? "  FAILED  " : p->online ? "  online  " : "  offline ");
        put_dec(out, p->ipi_count, 4); out("  ");
        uint64_t idle, total, sw;
        if (p->online && sched_cpu_stats(i, &idle, &total, &sw) == 0 && total) {
            put_dec(out, sw, 8); out("  ");
            put_dec(out, 100 - (idle > total ? total : idle) * 100 / total, 3); out("%");
        } else {
            out("       -    -");
        }
        out("\n");
    }
    out("\n");
}

// ---- Проверка многоядерности (команда test smp) ----

#define ST_MAX (PERCPU_MAX_CPUS + 1)      // по рабочему потоку на каждое ядро и ещё один для проверки вытеснения
#define ST_LOCK_ROUNDS 20000

typedef struct {
    uint32_t cpu_want;               // ядро, на котором поток обязан исполняться (маска из одного бита)
    uint32_t role;                   // 0 - обычный, 1 - ждёт вытеснения (не отдаёт ядро сам), 2 - его вытесняет
    volatile uint32_t migrated;      // замечено исполнение не на своём ядре
    uint64_t sum;
    uint64_t cycles;
    int preempt_failed;
} st_worker_t;

static st_worker_t st_w[ST_MAX];
static unsigned st_n;
static volatile uint32_t st_left;
static volatile uint32_t st_running;
static volatile uint32_t st_b_ran;
static spinlock_t st_lock = SPINLOCK_INIT;
static uint64_t st_plain;            // обычное (не атомарное) поле: целость даёт только замок
static volatile uint64_t st_atomic;
static uint64_t st_t0;

static void st_report(void) {
    uint64_t wall = rdtsc() - st_t0;
    uint64_t khz = clock_tsc_khz();
    int bad = 0;
    uart_print("[KIRA SMPTEST] workers finished:\n");
    for (unsigned i = 0; i < st_n; i++) {
        const st_worker_t* w = &st_w[i];
        int ok = w->sum == SMP_WORK_EXPECTED && !w->migrated && !w->preempt_failed;
        if (!ok) bad = 1;
        uart_print("  worker "); uart_print_dec(i);
        uart_print(": core "); uart_print_dec(w->cpu_want);
        uart_print(khz ? ", " : ", cycles ");
        uart_print_dec(khz ? w->cycles / khz : w->cycles);
        uart_print(khz ? " ms, checksum " : ", checksum ");
        uart_print(w->sum == SMP_WORK_EXPECTED ? "ok" : "WRONG");
        uart_print(w->migrated ? ", MOVED to another core" : ", stayed on its core");
        if (w->role == 1) uart_print(w->preempt_failed ? ", NOT preempted" : ", preempted by tick");
        uart_print(ok ? "\n" : "  <-- FAIL\n");
    }
    uint64_t want = (uint64_t)st_n * ST_LOCK_ROUNDS;
    int lock_ok = st_plain == want, at_ok = st_atomic == want;
    if (!lock_ok || !at_ok) bad = 1;
    uart_print("  lock-protected counter "); uart_print_dec(st_plain);
    uart_print(" of "); uart_print_dec(want); uart_print(lock_ok ? " ok" : " LOST UPDATES");
    uart_print(", atomic counter "); uart_print_dec(st_atomic); uart_print(at_ok ? " ok\n" : " LOST UPDATES\n");
    uart_print("  wall time "); uart_print_dec(khz ? wall / khz : wall); uart_print(khz ? " ms. " : " cycles. ");
    uart_print(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    __atomic_store_n(&st_running, 0, __ATOMIC_RELEASE);
}

static void st_worker(void* arg) {
    st_worker_t* w = (st_worker_t*)arg;
    uint64_t t0 = rdtsc();
    if (w->role == 2) __atomic_store_n(&st_b_ran, 1, __ATOMIC_RELEASE);
    if (w->role == 1) {
        // Не отдаём ядро сами: второй поток этого ядра запустится, только если тик вытеснит нас по кванту.
        uint64_t limit = clock_tsc_khz() * 2000;                 // ждём не дольше 2 с
        while (!__atomic_load_n(&st_b_ran, __ATOMIC_ACQUIRE)) {
            if (rdtsc() - t0 > limit) { w->preempt_failed = 1; break; }
            spin_relax();
        }
    }
    uint64_t x = 1, s = 0;
    for (uint64_t i = 0; i < SMP_WORK_ITERS; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s = (s ^ (x >> 11)) * 0x9E3779B97F4A7C15ULL;
        if ((i & 0xFFFF) == 0 && this_cpu()->cpu_id != w->cpu_want) w->migrated = 1;
    }
    w->sum = s;
    for (unsigned k = 0; k < ST_LOCK_ROUNDS; k++) {
        uint64_t f = spin_lock_irqsave(&st_lock);
        uint64_t v = st_plain;
        spin_relax();                                            // окно для гонки: без замка обновления терялись бы
        st_plain = v + 1;
        spin_unlock_irqrestore(&st_lock, f);
        __atomic_fetch_add(&st_atomic, 1, __ATOMIC_RELAXED);
        if ((k & 0xFFF) == 0 && this_cpu()->cpu_id != w->cpu_want) w->migrated = 1;
    }
    w->cycles = rdtsc() - t0;
    if (__atomic_sub_fetch(&st_left, 1, __ATOMIC_ACQ_REL) == 0) st_report();
}

int smp_selftest_start(void) {
    if (smp_online() < 2) return -1;
    if (!apic_timer_initial_count()) return -2;                  // без тика LAPIC у ядер нет вытеснения
    if (__atomic_exchange_n(&st_running, 1, __ATOMIC_ACQ_REL)) return -3;
    st_n = 0; st_plain = 0; st_atomic = 0; st_b_ran = 0;
    unsigned first = 0;
    for (unsigned i = 1; i < percpu_count() && st_n < ST_MAX - 1; i++) {
        if (!percpu_table[i].online) continue;
        if (!first) first = i;
        st_w[st_n++] = (st_worker_t){ .cpu_want = i, .role = (i == first) ? 1u : 0u };
    }
    st_w[st_n++] = (st_worker_t){ .cpu_want = first, .role = 2 };      // второй поток на первом ядре
    st_left = st_n;
    st_t0 = rdtsc();
    unsigned made = 0;
    for (unsigned i = 0; i < st_n; i++) {
        thread_t* t = sched_create_on("smpwork", st_worker, &st_w[i], PRIO_NORMAL, cm_one(st_w[i].cpu_want));
        if (t) made++;
        else { st_w[i].sum = 0; __atomic_sub_fetch(&st_left, 1, __ATOMIC_ACQ_REL); }
    }
    if (!made) { __atomic_store_n(&st_running, 0, __ATOMIC_RELEASE); return -4; }
    return (int)made;
}

// ---- Проверка изоляции сбоев (команда test crash) ----
// Каждый режим запускает поток, который намеренно ломается: по результату видно, что система выжила или, где
// изолировать нельзя, остановилась аварийно с понятной причиной.
static spinlock_t crash_lock = SPINLOCK_INIT;

static uint64_t crash_recurse(uint64_t n) { volatile uint8_t pad[512]; pad[0] = (uint8_t)n; if (n > (1u << 30)) return 0; return crash_recurse(n + 1) + pad[0]; }

static void crash_worker(void* arg) {
    unsigned mode = (unsigned)(uintptr_t)arg;
    if (mode == 3) {
        (void)crash_recurse(0);                                  // переполнение стека потока: охранная страница, #DF на IST
    } else if (mode == 1) {
        __asm__ volatile ("int $18");                            // вектор машинной проверки: отказ ядра
    } else if (mode == 2) {
        (void)spin_lock_irqsave(&crash_lock);                    // сбой под спин-замком: изолировать нельзя
        *(volatile uint64_t*)0 = 1;
    } else {
        *(volatile uint64_t*)0 = 1;                              // обычная ошибка кода ядра: гибнет только поток
    }
    for (;;) sched_yield();
}

int smp_crash_test(unsigned mode, unsigned cpu) {
    cpumask_t mask = SCHED_AFFINITY_ALL;
    if (mode == 1 || mode == 2) {
        if (cpu >= percpu_count() || !percpu_table[cpu].online) return -1;
        if (!percpu_table[cpu].sched_on) return -3;              /* ядро без тика LAPIC не исполняет потоки */
        mask = cm_one(cpu);
    }
    return sched_create_on("smpcrash", crash_worker, (void*)(uintptr_t)mode, PRIO_NORMAL, mask) ? 0 : -2;
}

#else
void ipi_dispatch(uint64_t vector, uint64_t cs) { (void)vector; (void)cs; }
void smp_kick_idle(cpumask_t affinity) { (void)affinity; }
void smp_resched_cpu(unsigned cpu) { (void)cpu; }
#endif
