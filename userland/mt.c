/* Проверка многопоточности: потоки одного процесса на разных ядрах, мьютекс на futex, ожидание события,
   завершение процесса при работающих потоках. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }

#define NT 4
#define ROUNDS 20000
static volatile u32 lock;
static volatile u64 counter;
static volatile u32 finished;               /* futex: сколько рабочих закончило */
static volatile u32 never;                  /* futex, по которому никто не будит */
static volatile u64 tids[NT];

static void worker(u64 i) {
    tids[i] = k_gettid();
    for (u32 r = 0; r < ROUNDS; r++) {
        k_mutex_lock(&lock);
        u64 v = counter;                    /* окно для гонки: без мьютекса обновления терялись бы */
        for (volatile int d = 0; d < 3; d++) { }
        counter = v + 1;
        k_mutex_unlock(&lock);
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELEASE);
    k_futex_wake(&finished, 1);
}

static void spinner(u64 i) { (void)i; for (;;) { } }                       /* не делает системных вызовов */
static void sleeper(u64 i) { (void)i; for (;;) k_futex_wait(&never, 0, K_FOREVER); }

int umain(void) {
    say("mt: "); say_u(NT); say(" threads x "); say_u(ROUNDS); say(" locked increments\n");
    u8* stacks = (u8*)k_heap_grow(NT * 4 + 8);
    if (!stacks) { say("mt: FAIL no memory\n"); return 1; }
    u64 t0 = k_clock_ns();
    for (u64 i = 0; i < NT; i++) {
        i64 r = k_thread_create(worker, stacks + i * 16384, 16384, i);
        if (r < 0) { say("mt: FAIL thread_create "); say_u((u64)-r); say("\n"); return 1; }
    }
    for (;;) {                                                         /* ждём событие, а не крутимся */
        u32 f = __atomic_load_n(&finished, __ATOMIC_ACQUIRE);
        if (f == NT) break;
        k_futex_wait(&finished, f, 500);
    }
    u64 ms = (k_clock_ns() - t0) / 1000000;
    say("mt: counter "); say_u(counter); say(" (expected "); say_u((u64)NT * ROUNDS); say(") in "); say_u(ms); say(" ms, thread ids");
    for (int i = 0; i < NT; i++) { say(" "); say_u(tids[i]); }
    say(counter == (u64)NT * ROUNDS ? " - PASS\n" : " - FAIL\n");

    /* Завершение процесса при живых потоках: один крутится без системных вызовов, другой спит без срока.
       Ядро обязано снять оба: первый - по прерыванию таймера, второй - разбудив его. */
    k_thread_create(spinner, stacks + NT * 16384, 16384, 0);
    k_thread_create(sleeper, stacks + NT * 16384 + 16384, 16384, 0);
    k_sleep(5);
    say("mt: exiting with a spinning and a sleeping thread still alive (ps must show no 'mt' afterwards)\n");
    k_exit(0);
}
