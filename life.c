#include "life.h"
#include "kfs.h"
#include "cpu.h"
#include "spinlock.h"
#include "timer.h"
#include "uart.h"

#define QN 16
#define LOG_MAX 2048
#define LOG_KEEP 1536

typedef struct { char kind[8]; char name[16]; int code; uint32_t sec; } ev_t;

static ev_t q[QN];
static volatile uint32_t qh, qt;           /* голова пишет producer, хвост читает life_tick */
static uint32_t dropped;
static spinlock_t life_lock = SPINLOCK_INIT;
static int ready;
static char log[LOG_MAX + 1];
static uint32_t loglen;

static void scopy(char* d, const char* s, unsigned cap) { unsigned i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void life_note(const char* kind, const char* name, int code) {
    uint64_t f = spin_lock_irqsave(&life_lock);
    if (!ready || qh - qt >= QN) { dropped++; spin_unlock_irqrestore(&life_lock, f); return; }
    ev_t* e = &q[qh % QN];
    scopy(e->kind, kind, sizeof e->kind); scopy(e->name, name ? name : "", sizeof e->name);
    e->code = code; e->sec = (uint32_t)(timer_get_ticks() / TIMER_HZ);
    qh++;
    spin_unlock_irqrestore(&life_lock, f);
}

uint32_t life_dropped(void) { return dropped; }

/* ---- числа и строки без libc ---- */
static unsigned put_u(char* o, uint32_t v) { char t[12]; unsigned n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); for (unsigned i = 0; i < n; i++) o[i] = t[n - 1 - i]; return n; }
static unsigned put_i(char* o, int v) { if (v < 0) { o[0] = '-'; return 1 + put_u(o + 1, (uint32_t)(-(int64_t)v)); } return put_u(o, (uint32_t)v); }
static unsigned put_s(char* o, const char* s) { unsigned n = 0; while (s[n]) { o[n] = s[n]; n++; } return n; }

static const char* find_key(const char* text, const char* key) {      /* "ключ=" в тексте профиля */
    for (const char* p = text; *p; p++) {
        const char* a = p; const char* k = key;
        while (*k && *a == *k) { a++; k++; }
        if (!*k && *a == '=') return a + 1;
    }
    return 0;
}
static int parse_i(const char* p, int* out) {
    int neg = 0; long v = 0; int digits = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9' && digits < 10) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (!digits) return -1;
    *out = (int)(neg ? -v : v); return 0;
}

static int safe_name(const char* n) {
    if (!n[0]) return 0;
    for (unsigned i = 0; n[i]; i++) {
        char c = n[i];
        if (i >= 15 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

typedef struct { uint32_t runs, faults, denied; int last; } prof_t;

static void prof_path(char* path, const char* name) { unsigned n = put_s(path, "/var/prog/"); n += put_s(path + n, name); path[n] = 0; }

static int prof_load(const char* name, prof_t* p) {
    char path[40], buf[96];
    prof_path(path, name);
    int64_t n = kfs_read(path, 0, buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    int v; const char* s;
    p->runs = p->faults = p->denied = 0; p->last = 0;
    if ((s = find_key(buf, "runs")) && parse_i(s, &v) == 0 && v >= 0) p->runs = (uint32_t)v;
    if ((s = find_key(buf, "faults")) && parse_i(s, &v) == 0 && v >= 0) p->faults = (uint32_t)v;
    if ((s = find_key(buf, "denied")) && parse_i(s, &v) == 0 && v >= 0) p->denied = (uint32_t)v;
    if ((s = find_key(buf, "last")) && parse_i(s, &v) == 0) p->last = v;
    return 0;
}

static int prof_save(const char* name, const prof_t* p) {
    char path[40], buf[96]; unsigned n = 0;
    prof_path(path, name);
    n += put_s(buf + n, "runs="); n += put_u(buf + n, p->runs);
    n += put_s(buf + n, " faults="); n += put_u(buf + n, p->faults);
    n += put_s(buf + n, " denied="); n += put_u(buf + n, p->denied);
    n += put_s(buf + n, " last="); n += put_i(buf + n, p->last);
    buf[n++] = '\n';
    return kfs_write(path, buf, n);
}

int life_profile(const char* name, uint32_t* runs, uint32_t* faults, uint32_t* denied, int* last) {
    if (!ready || !safe_name(name)) return -1;
    prof_t p;
    if (prof_load(name, &p)) return -1;
    if (runs) *runs = p.runs;
    if (faults) *faults = p.faults;
    if (denied) *denied = p.denied;
    if (last) *last = p.last;
    return 0;
}

static void log_add(const ev_t* e) {
    char line[64]; unsigned n = 0;
    n += put_u(line + n, e->sec); n += put_s(line + n, "s ");
    n += put_s(line + n, e->kind);
    if (e->name[0]) { line[n++] = ' '; n += put_s(line + n, e->name); }
    if (e->kind[0] == 'e' || e->kind[0] == 'k' || e->kind[0] == 'd') { line[n++] = ' '; n += put_i(line + n, e->code); }
    line[n++] = '\n';
    if (loglen + n > LOG_MAX) {                                        /* забываем самое старое, обрезая по границе строки */
        uint32_t cut = loglen > LOG_KEEP ? loglen - LOG_KEEP : 0;
        while (cut < loglen && log[cut] != '\n') cut++;
        if (cut < loglen) cut++;
        for (uint32_t i = cut; i < loglen; i++) log[i - cut] = log[i];
        loglen -= cut;
    }
    for (unsigned i = 0; i < n; i++) log[loglen + i] = line[i];
    loglen += n;
}

static void apply(const ev_t* e) {
    log_add(e);
    if (!safe_name(e->name)) return;
    int isstart = seq(e->kind, "start"), isexit = seq(e->kind, "exit"), iskilled = seq(e->kind, "killed"), isdenied = seq(e->kind, "denied");
    if (!(isstart || isexit || iskilled || isdenied)) return;
    prof_t p;
    if (prof_load(e->name, &p)) { p.runs = p.faults = p.denied = 0; p.last = 0; }
    if (isstart) p.runs++;
    if (isexit) p.last = e->code;
    if (iskilled) { p.faults++; p.last = e->code; }
    if (isdenied) p.denied++;
    (void)prof_save(e->name, &p);
}

void life_tick(void) {
    if (!ready || qt == qh) return;
    while (qt != qh) {
        ev_t e = q[qt % QN];
        qt++;                                  /* слот освобождается после копирования */
        apply(&e);
    }
    (void)kfs_write("/var/life.log", log, loglen);
}

void life_init(void) {
    ready = 0; qh = qt = 0; loglen = 0;
    if (!kfs_mounted()) return;
    kfs_stat_t st;
    if (kfs_stat("/var", &st) != 0 && kfs_mkdir("/var") != 0) return;
    if (kfs_stat("/var/prog", &st) != 0 && kfs_mkdir("/var/prog") != 0) return;
    if (kfs_stat("/var/life.log", &st) == 0 && st.type == KFS_FILE) {
        uint64_t n = st.size < LOG_MAX ? st.size : LOG_MAX;
        int64_t r = kfs_read("/var/life.log", st.size - n, log, (size_t)n);
        if (r > 0) loglen = (uint32_t)r;
        uint32_t i = 0;                                                    /* начало с целой строки */
        if (st.size > n) { while (i < loglen && log[i] != '\n') i++; if (i < loglen) i++; }
        if (i) { for (uint32_t k = i; k < loglen; k++) log[k - i] = log[k]; loglen -= i; }
    }
    ready = 1;
    life_note("boot", "", 0);
    life_tick();
}

void life_print(void) {
    if (!ready) { uart_print("журнал недоступен: нет файловой системы\n"); return; }
    life_tick();
    if (!loglen) { uart_print("(журнал пуст)\n"); return; }
    for (uint32_t i = 0; i < loglen; i++) uart_putchar(log[i]);
}
