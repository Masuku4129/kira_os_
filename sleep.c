/* Сон S3: оркестратор (см. sleep.h). Машинная часть - platform_sleep (arch/x86_64/sleep.c); на ARM64 сна S3 нет, платформа отвечает отказом с причиной. */
#include "sleep.h"
#include "devpm.h"
#include "platform.h"
#include "console.h"
#include "kdrv.h"
#include "sched.h"
#include "cpumask.h"
#include "clock.h"
#include "rtc.h"
#include "uart.h"

static volatile int busy;
static volatile int requested;
static struct { unsigned slept, refused, failed_dev; int64_t last_wall_s; uint64_t last_ms; char last_why[96]; } st;

static void log_line(const char* s) { uart_print(s); uart_print("\n"); }

static int wait_settled(void) {
    uint64_t end = clock_ms() + 2000;
    while (!sched_freeze_settled()) {
        if (clock_ms() > end) return -1;
        sched_yield();
    }
    return 0;
}

static void copy_why(const char* s) { unsigned i = 0; for (; s && s[i] && i < sizeof st.last_why - 1; i++) st.last_why[i] = s[i]; st.last_why[i] = 0; }

void sleep_request(void) { __atomic_store_n(&requested, 1, __ATOMIC_RELEASE); }
int sleep_take_request(void) { return __atomic_exchange_n(&requested, 0, __ATOMIC_ACQ_REL); }

static int wall_seconds(int64_t* out) {
    rtc_time_t t;
    if (rtc_now(0, &t) != 0) return -1;
    *out = rtc_to_epoch(&t);
    return 0;
}

int sleep_enter(const char* source) {
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) { log_line("[KIRA SLEEP] sleep is already in progress."); return 0; }
    int result = 0;
    const char* why = 0;
    devpm_set_log(log_line);
    uart_print("[KIRA SLEEP] sleep requested: "); uart_print(source ? source : "?"); uart_print("\n");
    if (!platform_sleep_supported(&why)) {
        uart_print("[KIRA SLEEP] S3 is not available on this machine: "); uart_print(why); uart_print(".\n");
        copy_why(why); st.refused++;
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    char drv[16] = "";
    if (kdrv_active_count(drv) > 0) {          /* устройством владеет программа Ring 3: ни остановить его, ни восстановить после сна ядро не может */
        uart_print("[KIRA SLEEP] sleep refused: Ring 3 driver drv-"); uart_print(drv); uart_print(" owns a device (stop it first: drv stop "); uart_print(drv); uart_print(").\n");
        copy_why("Ring 3 driver is running"); st.refused++;
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    cpumask_t old_aff = sched_pin_self(cm_one(0));            /* усыпляет и будит загрузочное ядро: поток-владелец исполняется на нём */
    thread_t* me = (thread_t*)sched_current();
    sched_freeze(1, me);                                        /* потоки процессов встают: они не должны писать на диск и трогать устройства */
    if (wait_settled() != 0) {
        log_line("[KIRA SLEEP] processes did not stop within 2 s: sleep cancelled.");
        copy_why("processes did not stop"); st.refused++;
        goto out;
    }
    uart_print("сохраняю опыт и данные дисков перед сном...\n");
    console_power_prepare();                                   /* опыт Разума, чёрный ящик, отложенная запись ФС, кэши дисков - как при выключении */
    uart_print("[KIRA SLEEP] suspending "); uart_print_dec((uint64_t)devpm_count()); uart_print(" device hooks...\n");
    if (devpm_suspend_all() < 0) {
        uart_print("[KIRA SLEEP] sleep cancelled: device '"); uart_print(devpm_refusal() ? devpm_refusal() : "?"); uart_print("' cannot survive it; the system keeps running.\n");
        copy_why(devpm_refusal()); st.refused++;
        goto out;
    }
    sched_freeze(2, me);                                        /* драйверы тихи: теперь встают и потоки ядра */
    int64_t w0 = 0, w1 = 0; int have0 = wall_seconds(&w0) == 0;
    uint64_t t0 = clock_ms();
    int r = platform_sleep();
    sched_freeze(1, me);                                        /* потоки ядра нужны хукам пробуждения (например, потоку USB) */
    int bad = devpm_resume_all();
    st.failed_dev += (unsigned)bad;
    if (r > 0) {
        st.slept++;
        st.last_ms = clock_ms() - t0;
        if (have0 && wall_seconds(&w1) == 0) st.last_wall_s = w1 - w0; else st.last_wall_s = -1;
        uart_print("[KIRA SLEEP] awake: devices restored, "); uart_print_dec((uint64_t)bad); uart_print(" failed to come back; asleep for ");
        if (st.last_wall_s >= 0) { uart_print_dec((uint64_t)st.last_wall_s); uart_print(" s by the real-time clock"); } else uart_print("an unknown time");
        uart_print(".\n");
        copy_why("");
        result = 1;
    } else {
        copy_why(r == 0 ? "the machine did not enter S3" : "platform refused"); st.refused++;
        uart_print("[KIRA SLEEP] sleep did not happen (code "); uart_print_dec((uint64_t)(r < 0 ? -r : r)); uart_print("); devices are back in work.\n");
    }
out:
    sched_thaw();
    sched_set_affinity_self(old_aff);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return result;
}

static void put_u(void (*out)(const char*), uint64_t v) {
    char t[21], b[22]; int k = 0; unsigned n = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 20);
    while (k) b[n++] = t[--k];
    b[n] = 0; out(b);
}

void sleep_describe(void (*out)(const char*)) {
    const char* why = 0;
    int ok = platform_sleep_supported(&why);
    out(ok ? "сон S3: поддерживается\n" : "сон S3: недоступен - ");
    if (!ok) { out(why); out("\n"); }
    out("сеансов сна: "); put_u(out, st.slept);
    out(", отказов: "); put_u(out, st.refused);
    out(", устройств не вернулось: "); put_u(out, st.failed_dev);
    if (st.slept) {
        out("; последний: "); if (st.last_wall_s >= 0) { put_u(out, (uint64_t)st.last_wall_s); out(" с по часам RTC"); } else out("время неизвестно");
    }
    if (st.last_why[0]) { out("; последняя причина отказа: "); out(st.last_why); }
    out("\nхуки устройств ("); put_u(out, (uint64_t)devpm_count()); out("):\n");
    for (int i = 0; ; i++) {
        devpm_info_t in;
        if (devpm_info(i, &in) != 0) break;
        out("  "); out(in.name); out(in.state == 2 ? "  НЕ ВОССТАНОВИЛОСЬ" : in.state == 1 ? "  усыплено" : "  работает"); out("\n");
    }
}
