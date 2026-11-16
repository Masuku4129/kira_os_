#include "bbsave.h"
#include "bbring.h"
#include "kfs.h"
#include "kmutex.h"
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "percpu.h"
#include "uart.h"

#define BB_DIR1   "/var"
#define BB_DIR2   "/var/log"
#define BB_FILE   "/var/log/blackbox.bin"
#define BB_PREV   "/var/log/blackbox.old.bin"

#define BB_SAVE_MIN_GAP_MS    2000u      /* тяжёлые события: не чаще одного сброса в 2 с */
#define BB_SAVE_PERIOD_MS     30000u     /* накопились обычные события: раз в 30 с */
#define BB_SAVE_BOOT_MS       10000u     /* первый сброс нового запуска (сдвигает прежний файл в .old): через 10 с */
#define BB_CORE_GAP_MS        500u       /* основной цикл ядра обычно возвращается каждые 10 мс */
#define BB_CORE_GAP_SEVERE_MS 1000u
#define BB_PAUSE_MAX_MS       2000u      /* bb pause: меньше срока сторожа (5 с), чтобы проверка сама его не будила */

static kmutex_t savemx = KMUTEX_INIT;
static uint8_t fbuf[sizeof(bb_file_hdr_t) + BB_RING * BB_REC_SIZE] __attribute__((aligned(8)));    /* общий буфер файла: под savemx */
static uint32_t boot_id;
static int rotated;                       /* прежний файл уже сдвинут в .old в этом запуске */
static volatile uint32_t saved_severe;
static volatile uint64_t saved_total, saved_ms, last_try_ms, save_cost_us;
static volatile uint32_t saved_reason, save_fail;
static volatile int inited;

static uint64_t hook_ms(void) { return clock_ms(); }
static unsigned hook_cpu(void) { return this_cpu()->cpu_id; }

/* Номер запуска - следующий после записанного в файлах прежнего запуска. */
static uint32_t read_boot_id(void) {
    uint32_t best = 0;
    static const char* const names[2] = { BB_FILE, BB_PREV };
    for (int i = 0; i < 2; i++) {
        bb_file_hdr_t h;
        if (kfs_read(names[i], 0, &h, sizeof h) == (int64_t)sizeof h && h.magic[0] == 'K' && h.magic[1] == 'B' && h.magic[2] == 'B' && h.magic[3] == '1' && h.boot_id > best)
            best = h.boot_id;
    }
    return best + 1;
}

static int ensure_dirs(void) {
    kfs_stat_t st;
    if (kfs_stat(BB_DIR1, &st) != 0 && kfs_mkdir(BB_DIR1) != 0) return -1;
    if (kfs_stat(BB_DIR2, &st) != 0 && kfs_mkdir(BB_DIR2) != 0) return -1;
    return 0;
}

int bbsave_now(unsigned reason) {
    if (!inited || !kfs_mounted()) return -1;
    kmutex_lock(&savemx);
    uint64_t t0 = clock_ns();
    int rc = -1;
    if (ensure_dirs() == 0) {
        if (!rotated) {                                    /* первый сброс запуска: файл на диске - прежнего запуска, он становится .old */
            kfs_stat_t st;
            if (kfs_stat(BB_FILE, &st) == 0) {
                (void)kfs_unlink(BB_PREV);
                if (kfs_clone(BB_FILE, BB_PREV) != 0) {    /* общих блоков не вышло: обычная копия через буфер */
                    int64_t got = kfs_read(BB_FILE, 0, fbuf, sizeof fbuf);
                    if (got > 0) (void)kfs_write(BB_PREV, fbuf, (size_t)got);
                }
            }
            rotated = 1;
        }
        uint32_t sev0 = bb_severe_count();
        bb_stats_t stt; bb_get_stats(&stt);
        unsigned n = bb_snapshot((bb_rec_t*)(fbuf + sizeof(bb_file_hdr_t)), BB_RING);
        uint64_t now = clock_ms();
        unsigned len = bb_file_seal(fbuf, n, boot_id, reason, now, stt.total, (uint32_t)stt.lost);
        rc = kfs_write(BB_FILE, fbuf, len) == 0 ? 0 : -1;
        if (rc == 0) {
            saved_severe = sev0; saved_total = stt.total; saved_ms = now; saved_reason = reason;
        } else save_fail++;
    } else save_fail++;
    save_cost_us = (clock_ns() - t0) / 1000;
    last_try_ms = clock_ms();
    kmutex_unlock(&savemx);
    return rc;
}

/* Поток сброса: проверяет пять раз в секунду, не нужен ли сброс. Сам диска в горячем пути не касается: события пишутся в кольцо в памяти. */
static void bb_thread(void* arg) {
    (void)arg;
    for (;;) {
        sched_sleep_ticks(TIMER_HZ / 5);
        uint64_t now = clock_ms();
        unsigned reason = 0;
        uint64_t since = now - last_try_ms;
        if (bb_severe_count() != saved_severe && since >= BB_SAVE_MIN_GAP_MS) reason = BB_SAVE_SEVERE;
        else if (!saved_ms && now >= BB_SAVE_BOOT_MS && since >= BB_SAVE_MIN_GAP_MS) reason = BB_SAVE_BOOT;
        else if (bb_total() != saved_total && since >= BB_SAVE_PERIOD_MS) reason = BB_SAVE_PERIODIC;
        if (reason) (void)bbsave_now(reason);
    }
}

void bbsave_init(void) {
    bb_set_hooks(hook_ms, hook_cpu);
    boot_id = kfs_mounted() ? read_boot_id() : 1;
    inited = 1;
    bb_event(BB_BOOT, BB_SEV_INFO, boot_id, 0, 0, 0, 0, "boot");
    if (!sched_create("blackbox", bb_thread, 0, PRIO_NORMAL)) uart_print("[KIRA BLACKBOX] cannot start the save thread.\n");
    uart_print("[KIRA BLACKBOX] flight recorder online: ring of "); uart_print_dec(BB_RING);
    uart_print(" events, saved to " BB_FILE " outside the hot path (run #"); uart_print_dec(boot_id); uart_print(").\n");
}

void bbsave_core_tick(void) {
    static uint64_t last;
    if (!inited) return;
    uint64_t now = clock_ms();
    if (last && now - last >= BB_CORE_GAP_MS)
        bb_event(BB_GAP, now - last >= BB_CORE_GAP_SEVERE_MS ? BB_SEV_SEVERE : BB_SEV_WARN, (uint32_t)(now - last), BB_GAP_CORE, 0, 0, 0, "core");
    last = now;
}

/* ---------------- консоль ---------------- */

static int same(const char* a, const char* w) { while (*a && *a == *w) { a++; w++; } return *a == *w; }
static void print_rec(const bb_rec_t* r) { char l[256]; bb_format(r, l, sizeof l); uart_print("  "); uart_print(l); uart_print("\n"); }

static const char* reason_name(uint32_t r) {
    switch (r) {
    case BB_SAVE_REQUEST: return "по команде";
    case BB_SAVE_SEVERE: return "тяжёлое событие";
    case BB_SAVE_PERIODIC: return "по расписанию";
    case BB_SAVE_SHUTDOWN: return "перед выключением";
    case BB_SAVE_BOOT: return "первый сброс запуска";
    default: return "?";
    }
}

unsigned bbsave_status(char* out, unsigned cap) {
    unsigned n = 0;
    #define PUTS(t) do { for (const char* q_ = (t); *q_ && n + 1 < cap; q_++) out[n++] = *q_; } while (0)
    #define PUTU(v) do { uint64_t x_ = (v); char t_[21]; int k_ = 0; do { t_[k_++] = (char)('0' + x_ % 10); x_ /= 10; } while (x_); while (k_ && n + 1 < cap) out[n++] = t_[--k_]; } while (0)
    if (cap) out[0] = 0;
    PUTS("[BB] запуск #"); PUTU(boot_id);
    if (saved_ms) {
        PUTS(", на диске " BB_FILE " записан "); PUTU((clock_ms() - saved_ms) / 1000); PUTS(" с назад ("); PUTS(reason_name(saved_reason));
        PUTS("), запись заняла "); PUTU(save_cost_us); PUTS(" мкс");
    } else PUTS(", на диск ещё не сбрасывался");
    if (save_fail) { PUTS(", неудачных записей "); PUTU(save_fail); }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    #undef PUTS
    #undef PUTU
    return n;
}

static void print_file(const char* path, unsigned limit) {
    kmutex_lock(&savemx);
    int64_t got = kfs_read(path, 0, fbuf, sizeof fbuf);
    bb_file_hdr_t h;
    int rc = got > 0 ? bb_file_parse(fbuf, (unsigned)got, &h, 0, 0) : BB_FILE_SHORT;
    if (rc < 0) {
        uart_print("[BB] файл "); uart_print(path); uart_print(got < 0 ? " не читается (нет такого файла)\n" : " повреждён или пуст (код ");
        if (got >= 0) { uart_print_dec((uint64_t)(-rc)); uart_print(")\n"); }
        kmutex_unlock(&savemx);
        return;
    }
    uart_print("[BB] файл "); uart_print(path); uart_print(": запуск #"); uart_print_dec(h.boot_id);
    uart_print(", записей "); uart_print_dec(h.count); uart_print(" (событий за запуск "); uart_print_dec(h.total);
    uart_print(", вытеснено "); uart_print_dec(h.lost); uart_print("), сброшен на "); uart_print_dec(h.saved_ms / 1000); uart_print(" с, причина: ");
    uart_print(reason_name(h.reason)); uart_print("\n");
    unsigned from = limit && h.count > limit ? h.count - limit : 0;
    for (unsigned i = from; i < h.count; i++) {
        bb_rec_t r;
        const uint8_t* src = fbuf + sizeof h + (size_t)i * BB_REC_SIZE;
        for (unsigned k = 0; k < BB_REC_SIZE; k++) ((uint8_t*)&r)[k] = src[k];
        print_rec(&r);
    }
    kmutex_unlock(&savemx);
}

static unsigned parse_num(const char* a) { unsigned v = 0; while (*a >= '0' && *a <= '9') v = v * 10 + (unsigned)(*a++ - '0'); return v; }

void bbsave_command(const char* arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg) arg = "";
    char line[256];
    if (same(arg, "save")) {
        int rc = bbsave_now(BB_SAVE_REQUEST);
        if (rc == 0) { bbsave_status(line, sizeof line); uart_print(line); uart_print("\n"); }
        else uart_print("[BB] не удалось записать " BB_FILE " (нет диска KiraFS или он только для чтения)\n");
        return;
    }
    if (same(arg, "clear")) {
        bb_clear();
        bb_event(BB_NOTE, BB_SEV_INFO, 0, 0, 0, 0, 0, "кольцо очищено");
        uart_print("[BB] кольцо очищено; файлы на диске не тронуты (они перезапишутся при следующем сбросе)\n");
        return;
    }
    if (same(arg, "old")) {
        /* Прежний запуск: пока этот запуск не сбросил себя на диск, его предшественник лежит в основном файле; после - в .old */
        bb_file_hdr_t h;
        const char* p = BB_PREV;
        if (kfs_read(BB_FILE, 0, &h, sizeof h) == (int64_t)sizeof h && h.magic[0] == 'K' && h.boot_id != boot_id) p = BB_FILE;
        print_file(p, 60);
        return;
    }
    if (arg[0] == 'f' && arg[1] == 'i' && arg[2] == 'l' && arg[3] == 'e') { print_file(BB_FILE, 60); return; }
    if (arg[0] == 'p' && arg[1] == 'a' && arg[2] == 'u' && arg[3] == 's' && arg[4] == 'e' && (arg[5] == ' ' || !arg[5])) {
        const char* v = arg + 5; while (*v == ' ') v++;
        unsigned ms = parse_num(v);
        if (!ms || ms > BB_PAUSE_MAX_MS) { uart_print("bb pause мс (1.."); uart_print_dec(BB_PAUSE_MAX_MS); uart_print("): проверочная пауза основного цикла ядра; чёрный ящик должен её записать\n"); return; }
        uart_print("[BB] пауза основного цикла на "); uart_print_dec(ms); uart_print(" мс...\n");
        uint64_t t0 = clock_ms();
        while (clock_ms() - t0 < ms) { }
        return;
    }
    unsigned limit = 30;
    if (same(arg, "all")) limit = BB_RING;
    else if (arg[0] >= '0' && arg[0] <= '9') { limit = parse_num(arg); if (!limit) limit = 30; if (limit > BB_RING) limit = BB_RING; }
    else if (arg[0]) {
        uart_print("bb [N | all]: последние события; bb old: прошлый запуск с диска; bb file: файл текущего запуска; bb save: сброс на диск; bb clear: очистить кольцо; bb pause мс: проверка\n");
        return;
    }
    kmutex_lock(&savemx);                                  /* буфер файла общий: сюда же кольцо копируется для печати */
    bb_rec_t* recs = (bb_rec_t*)(fbuf + sizeof(bb_file_hdr_t));
    unsigned n = bb_snapshot(recs, limit);
    bb_summary(line, sizeof line); uart_print(line); uart_print("\n");
    bbsave_status(line, sizeof line); uart_print(line); uart_print("\n");
    for (unsigned i = 0; i < n; i++) print_rec(&recs[i]);
    if (!n) uart_print("  событий нет\n");
    kmutex_unlock(&savemx);
}
