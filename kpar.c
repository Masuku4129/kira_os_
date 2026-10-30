#include "kpar.h"

#ifdef KIRA_HOST_TEST
void kpar_init(unsigned helpers) { (void)helpers; }
unsigned kpar_helpers(void) { return 0; }
void kpar_run(kpar_fn fn, void* ctx, uint32_t n) { for (uint32_t i = 0; i < n; i++) fn(ctx, i); }
#else
#include "sched.h"
#include "spinlock.h"
#include "uart.h"

/* Работа выдаётся «билетами»: старшие 32 бита - номер поколения, младшие - номер следующей части. Помощник берёт
   часть только сравнением-обменом с ожидаемым поколением, поэтому опоздавший помощник (проснулся, когда это
   поколение уже закончилось и началось следующее) не выполнит чужую часть со старой функцией. */
static spinlock_t lk = SPINLOCK_INIT;
static thread_t* helper[KPAR_MAX];
static volatile int sleeping[KPAR_MAX];
static unsigned nhelp;
static volatile uint32_t gen;
static volatile uint64_t ticket;
static volatile uint32_t done;
static kpar_fn cur_fn;
static void* cur_ctx;
static uint32_t cur_n;

static uint32_t work(uint32_t g, kpar_fn fn, void* ctx, uint32_t n) {
    uint32_t did = 0;
    for (;;) {
        uint64_t t = __atomic_load_n(&ticket, __ATOMIC_ACQUIRE);
        if ((uint32_t)(t >> 32) != g || (uint32_t)t >= n) break;
        if (!__atomic_compare_exchange_n(&ticket, &t, t + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        fn(ctx, (uint32_t)t);
        did++;
    }
    if (did) __atomic_add_fetch(&done, did, __ATOMIC_ACQ_REL);
    return did;
}

static void helper_main(void* arg) {
    unsigned me = (unsigned)(uintptr_t)arg;
    uint32_t seen = 0;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&lk);
        while (gen == seen) {
            sleeping[me] = 1;
            sched_wait_and_unlock(&lk, ~0ULL);
            irq_restore(f);
            f = spin_lock_irqsave(&lk);
        }
        sleeping[me] = 0;
        seen = gen;
        kpar_fn fn = cur_fn; void* ctx = cur_ctx; uint32_t n = cur_n;
        spin_unlock_irqrestore(&lk, f);
        work(seen, fn, ctx, n);
    }
}

void kpar_init(unsigned helpers) {
    if (helpers > KPAR_MAX) helpers = KPAR_MAX;
    for (unsigned i = 0; i < helpers; i++) {
        char name[16] = "par-"; int k = 4;
        if (i >= 10) name[k++] = (char)('0' + i / 10);
        name[k++] = (char)('0' + i % 10); name[k] = 0;
        helper[i] = sched_create(name, helper_main, (void*)(uintptr_t)i, PRIO_CRITICAL);
        if (!helper[i]) break;
        nhelp++;
    }
    uart_print("[KIRA PAR] parallel helpers: "); uart_print_dec(nhelp); uart_print(" (compositor and other split work)\n");
}

unsigned kpar_helpers(void) { return nhelp; }

void kpar_run(kpar_fn fn, void* ctx, uint32_t n) {
    if (n == 0) return;
    if (nhelp == 0 || n == 1) { for (uint32_t i = 0; i < n; i++) fn(ctx, i); return; }
    uint64_t f = spin_lock_irqsave(&lk);
    uint32_t g = gen + 1;
    cur_fn = fn; cur_ctx = ctx; cur_n = n;
    __atomic_store_n(&done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ticket, (uint64_t)g << 32, __ATOMIC_RELEASE);
    gen = g;
    unsigned want = n - 1 < nhelp ? n - 1 : nhelp;             /* будить не больше, чем есть частей */
    for (unsigned i = 0, woke = 0; i < nhelp && woke < want; i++)
        if (sleeping[i]) { sleeping[i] = 0; sched_wake_waiter(helper[i]); woke++; }
    spin_unlock_irqrestore(&lk, f);
    work(g, fn, ctx, n);
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < n) __asm__ volatile ("pause");   /* части короткие (доли мс) */
}
#endif
