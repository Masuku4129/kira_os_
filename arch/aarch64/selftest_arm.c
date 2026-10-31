#include "selftest_arm.h"
#include "sched.h"
#include "percpu.h"
#include "spinlock.h"
#include "kmutex.h"
#include "uart.h"
#include "timer.h"
#include "cpu.h"

#define WORK_ITERS  3000000ull
#define LOCK_ROUNDS 20000u
#define MX_THREADS  4
#define MX_ROUNDS   2000u
#define MAXW        (PERCPU_MAX_CPUS + 1)

typedef struct { uint32_t cpu, role; volatile uint32_t migrated; uint64_t sum; int preempt_failed; } worker_t;
static worker_t w[MAXW];
static unsigned nw;
static volatile uint32_t left, running, b_ran;
static spinlock_t lk = SPINLOCK_INIT;
static uint64_t plain;
static volatile uint64_t atomic_cnt;
static kmutex_t mx = KMUTEX_INIT;
static uint64_t mx_count;
static uint64_t expected_sum;
static volatile uint32_t sleep_err_ms, sleep_done, sleep_early;

static uint64_t work(void) {
    uint64_t x = 1, s = 0;
    for (uint64_t i = 0; i < WORK_ITERS; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s = (s ^ (x >> 11)) * 0x9E3779B97F4A7C15ULL;
    }
    return s;
}

static void report(void) {
    int bad = 0;
    uart_print("[KIRA SELFTEST] workers finished:\n");
    for (unsigned i = 0; i < nw; i++) {
        int ok = w[i].sum == expected_sum && !w[i].migrated && !w[i].preempt_failed;
        if (!ok) bad = 1;
        uart_print("  worker "); uart_print_dec(i); uart_print(": core "); uart_print_dec(w[i].cpu);
        uart_print(w[i].sum == expected_sum ? ", checksum ok" : ", checksum WRONG");
        uart_print(w[i].migrated ? ", MOVED to another core" : ", stayed on its core");
        if (w[i].role == 1) uart_print(w[i].preempt_failed ? ", NOT preempted" : ", preempted by tick");
        uart_print(ok ? "\n" : "  <-- FAIL\n");
    }
    uint64_t want = (uint64_t)nw * LOCK_ROUNDS;
    if (plain != want || atomic_cnt != want) bad = 1;
    uart_print("  spin-lock counter "); uart_print_dec(plain); uart_print(" of "); uart_print_dec(want);
    uart_print(plain == want ? " ok" : " LOST UPDATES");
    uart_print(", atomic "); uart_print_dec(atomic_cnt); uart_print(atomic_cnt == want ? " ok\n" : " LOST UPDATES\n");
    uint64_t mw = (uint64_t)MX_THREADS * MX_ROUNDS;
    if (mx_count != mw) bad = 1;
    uart_print("  kernel mutex (threads sleep while waiting): "); uart_print_dec(mx_count); uart_print(" of "); uart_print_dec(mw);
    uart_print(mx_count == mw ? " ok\n" : " LOST UPDATES\n");
    /* Раньше срока - ошибка планировщика (допуск - тик). Позже - ещё и задержка самой машины: на эмуляции 8 ядер
       хозяин может не давать ядру гостя время десятки мс, поэтому опоздание до 50 мс допустимо. */
    if (!sleep_done || (sleep_early && sleep_err_ms > 2) || (!sleep_early && sleep_err_ms > 50)) bad = 1;
    uart_print("  sleep 200 ms: "); uart_print(sleep_early ? "early by " : "late by "); uart_print_dec(sleep_err_ms); uart_print(" ms\n");
    uart_print(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
}

static void finish(void) { if (__atomic_sub_fetch(&left, 1, __ATOMIC_ACQ_REL) == 0) report(); }

static void worker(void* arg) {
    worker_t* me = (worker_t*)arg;
    uint64_t t0 = rdtsc();
    if (me->role == 2) __atomic_store_n(&b_ran, 1, __ATOMIC_RELEASE);
    if (me->role == 1) {
        /* ядро не отдаём сами: второй поток этого ядра пойдёт, только если тик вытеснит нас по кванту */
        uint64_t limit = 2 * (uint64_t)TIMER_HZ;
        uint64_t start = timer_get_ticks();
        while (!__atomic_load_n(&b_ran, __ATOMIC_ACQUIRE)) {
            if (timer_get_ticks() - start > limit) { me->preempt_failed = 1; break; }
        }
    }
    (void)t0;
    me->sum = work();
    if (this_cpu()->cpu_id != me->cpu) me->migrated = 1;
    for (unsigned k = 0; k < LOCK_ROUNDS; k++) {
        uint64_t f = spin_lock_irqsave(&lk);
        uint64_t v = plain;
        __asm__ volatile ("yield");
        plain = v + 1;
        spin_unlock_irqrestore(&lk, f);
        __atomic_fetch_add(&atomic_cnt, 1, __ATOMIC_RELAXED);
        if ((k & 0xFFF) == 0 && this_cpu()->cpu_id != me->cpu) me->migrated = 1;
    }
    finish();
}

static void mx_worker(void* arg) {
    (void)arg;
    for (unsigned i = 0; i < MX_ROUNDS; i++) {
        kmutex_lock(&mx);
        uint64_t v = mx_count;
        if ((i % 128) == 0) sched_yield();                   /* держатель уступает ядро: остальные ждут сном */
        mx_count = v + 1;
        kmutex_unlock(&mx);
    }
    finish();
}

static void sleeper(void* arg) {
    (void)arg;
    extern uint64_t arm_counter_khz(void);
    uint64_t khz = arm_counter_khz(), t0 = rdtsc();
    sched_sleep_ticks(TIMER_HZ / 5);
    uint64_t ms = (rdtsc() - t0) / (khz ? khz : 1);
    sleep_early = ms < 200;
    sleep_err_ms = (uint32_t)(ms > 200 ? ms - 200 : 200 - ms);
    sleep_done = 1;
    finish();
}

void selftest_arm_start(void) {
    if (__atomic_exchange_n(&running, 1, __ATOMIC_ACQ_REL)) { uart_print("test is already running\n"); return; }
    expected_sum = work();
    nw = 0; plain = 0; atomic_cnt = 0; b_ran = 0; mx_count = 0; sleep_done = 0;
    unsigned first = ~0u;
    for (unsigned i = 0; i < percpu_count() && nw < MAXW - 1; i++) {
        if (!percpu_table[i].sched_on) continue;
        if (first == ~0u) first = i;
        w[nw] = (worker_t){ .cpu = i, .role = i == first ? 1u : 0u };
        nw++;
    }
    w[nw++] = (worker_t){ .cpu = first, .role = 2 };            /* второй поток на первом ядре: проверка вытеснения */
    left = nw + MX_THREADS + 1;
    for (unsigned i = 0; i < nw; i++)
        if (!sched_create_on("smpwork", worker, &w[i], PRIO_NORMAL, cm_one(w[i].cpu))) { w[i].sum = 0; finish(); }
    for (unsigned i = 0; i < MX_THREADS; i++)
        if (!sched_create("mxwork", mx_worker, 0, PRIO_NORMAL)) finish();
    /* сон проверяется отдельно от очереди: критический поток не ждёт квант рабочих, ошибка - только от таймера */
    if (!sched_create("sleeper", sleeper, 0, PRIO_CRITICAL)) finish();
    uart_print("[KIRA SELFTEST] started: "); uart_print_dec(nw); uart_print(" pinned workers, ");
    uart_print_dec(MX_THREADS); uart_print(" mutex threads, 1 sleeper\n");
}
