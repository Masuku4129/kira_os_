#include "mind_sys.h"
#include "sched.h"
#include "proc.h"
#include "pmm.h"
#include "heap.h"
#include "display.h"
#include "blk.h"
#include "kobj.h"
#include "kfs.h"
#include "life.h"
#include "timer.h"
#include "clock.h"
#include "gfxsrv.h"
#include "uart.h"
#include "spinlock.h"
#include "percpu.h"
#include "cpu.h"
#include "devhealth.h"
#include "usb.h"
#include "kmutex.h"
#include "audio.h"
#include "kfs.h"

#define MODEL_PATH   "/var/mind.dat"
#define SEED_PATH    "/etc/mind_seed.bin"
#define PROF_DIR     "/var/mind"
#define SAVE_PERIOD  60
#define PROF_MAGIC   0x464F5250U   /* "PROF" */

static mind_t M;
static mind_sample_t S;
static mind_view_t V;
static spinlock_t view_lock = SPINLOCK_INIT;
static volatile uint32_t version;
static int ready;
static uint32_t printed;
static uint64_t last_save;
static int telemetry;

static thread_info_t* tinfo;
static unsigned tcap;
static uint64_t prev_idle[256], prev_total[256], prev_loops, prev_late, prev_blk_n, prev_blk_t, prev_msgs, prev_sw;
static uint32_t prev_faults;
static uint64_t prev_a_mixed, prev_a_under, prev_bytes, prev_tact_cyc, prev_aud_cyc;
static volatile int req_mode = -1, req_fx = -1, req_rtc = -1, req_cap = -1;
static kmutex_t mmx = KMUTEX_INIT;
static thread_info_t* tt_info; static unsigned tt_cap;
static void tact_main(void* arg);          /* секундный шаг и тактика меняют одни и те же записи Разума */
static volatile int32_t req_tz = 0x7FFFFFFF;

/* ---------------- исполнители ---------------- */

static void a_quantum(uint32_t q) { sched_set_quantum(q * TIMER_PER_10MS); }
static void a_weight(uint32_t pid, uint32_t w) { sched_set_proc_weight(pid, w); }
static void a_limit(uint32_t pid, uint32_t pct) { sched_set_proc_limit(pid, pct); }
static void a_fx(int level) { display_set_fx(level); }
static unsigned a_trim(void) { return heap_trim(); }
static void a_restrict(int level) { proc_set_heap_restrict(level); }
static int a_kill(uint32_t pid) { return proc_kill(pid); }
static void a_hid(int block) { usb_hid_verdict(block); }
static void a_bgfps(uint32_t fps) { gfx_set_bg_period(fps ? (TIMER_HZ + fps - 1) / fps : 0); }
static void a_core(int on, uint32_t pid) {                         /* отдаётся последнее ядро: загрузочное занято ядром */
    unsigned n = sched_cpu_count();
    sched_reserve_core(on && n >= 3 ? (int)n - 1 : -1, pid);
}

typedef struct { uint32_t magic; mind_profile_t p; uint32_t check; } prof_file_t;

static uint32_t prof_check(const prof_file_t* f) {
    const uint32_t* w = (const uint32_t*)&f->p;
    uint32_t c = 0x9E3779B9u;
    for (unsigned i = 0; i < sizeof f->p / 4; i++) c = (c ^ w[i]) * 0x01000193u;
    return c;
}

static int safe_name(const char* n) {
    if (!n[0]) return 0;
    for (unsigned i = 0; n[i]; i++) {
        char c = n[i];
        if (i >= 15 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

static void prof_path(char* out, const char* name) {
    const char* d = PROF_DIR "/";
    unsigned n = 0;
    while (*d) out[n++] = *d++;
    while (*name && n < 60) out[n++] = *name++;
    out[n] = 0;
}

static int a_pget(const char* name, mind_profile_t* out) {
    if (!safe_name(name) || !kfs_mounted()) return -1;
    char path[64]; prof_path(path, name);
    prof_file_t f;
    int have = kfs_read(path, 0, &f, sizeof f) == (int64_t)sizeof f && f.magic == PROF_MAGIC && f.check == prof_check(&f);
    if (!have) { for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t*)out)[i] = 0; }
    else *out = f.p;
    /* Запуски и аварии считает журнал жизни (/var/prog): берём оттуда, они точнее. */
    uint32_t runs, faults, denied; int last;
    if (life_profile(name, &runs, &faults, &denied, &last) == 0) { out->runs = runs; out->crashes = faults; have = 1; }
    return have ? 0 : -1;
}

static void a_pput(const char* name, const mind_profile_t* p) {
    if (!safe_name(name) || !kfs_mounted()) return;
    kfs_stat_t st;
    if (kfs_stat(PROF_DIR, &st) != 0 && kfs_mkdir(PROF_DIR) != 0) return;
    char path[64]; prof_path(path, name);
    prof_file_t f = { PROF_MAGIC, *p, 0 };
    f.check = prof_check(&f);
    kfs_write(path, &f, sizeof f);
}

/* Рычаги ядра (13-30, те, чьё значение живёт вне Разума). Границы значений уже заданы лесенкой Разума и проверены ещё раз в самих модулях. */
static void a_lever(int lever, int32_t v) {
    if (v <= 0) return;
    switch (lever) {
    case ML_AUDIO_LEAD:   audio_set_lead_ms((uint32_t)v); break;
    case ML_AUDIO_PERIOD: audio_set_period_ms((uint32_t)v); break;
    case ML_KFS_RUN:      kfs_set_run_max((uint32_t)v); break;
    case ML_STEAL:        sched_set_steal_bias((uint32_t)v); break;
    case ML_LIM_WIN:      sched_set_limit_window((uint32_t)v); break;
    default: break;
    }
}

static const mind_act_t act = { a_quantum, a_weight, a_limit, a_fx, a_trim, a_restrict, a_kill, a_pget, a_pput, a_hid, a_bgfps, a_core, a_lever };

/* ---------------- замеры ---------------- */

/* ---- допуск запусков ---- */
static spinlock_t adm_lock = SPINLOCK_INIT;
static struct { char name[16]; uint64_t win_ms; uint32_t n; } adm[64];
static uint64_t adm_win_ms; static uint32_t adm_total, adm_denied;
static char adm_denied_name[16];

static void put(char* out, unsigned cap, unsigned* k, const char* s) { while (*s && *k + 1 < cap) out[(*k)++] = *s++; out[*k] = 0; }
static void put_n(char* out, unsigned cap, unsigned* k, uint32_t v) { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n && *k + 1 < cap) out[(*k)++] = t[--n]; out[*k] = 0; }
static int same_name16(const char* a, const char* b) { for (int i = 0; i < 16; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 1; }

int mind_sys_admit(const char* name, unsigned same, char* why, unsigned cap) {
    mind_launch_t L;
    mind_launch_limits(&M, percpu_count(), &L);
    uint64_t now = clock_ms();
    unsigned k = 0; if (cap) why[0] = 0;
    uint64_t f = spin_lock_irqsave(&adm_lock);
    if (now - adm_win_ms >= 1000) { adm_win_ms = now; adm_total = 0; }
    int slot = -1, oldest = 0;
    for (int i = 0; i < 64; i++) {
        if (adm[i].name[0] && same_name16(adm[i].name, name)) { slot = i; break; }
        if (adm[i].win_ms < adm[oldest].win_ms) oldest = i;
    }
    if (slot < 0) { slot = oldest; int i = 0; for (; name[i] && i < 15; i++) adm[slot].name[i] = name[i]; adm[slot].name[i] = 0; adm[slot].win_ms = now; adm[slot].n = 0; }
    if (now - adm[slot].win_ms >= 1000) { adm[slot].win_ms = now; adm[slot].n = 0; }
    int ok = 1;
    if (same >= L.max_same) { ok = 0; put(why, cap, &k, "уже "); put_n(why, cap, &k, same); put(why, cap, &k, " копий - предел сейчас "); put_n(why, cap, &k, L.max_same); }
    else if (adm[slot].n >= L.rate_same) { ok = 0; put(why, cap, &k, "больше "); put_n(why, cap, &k, L.rate_same); put(why, cap, &k, " запусков в секунду"); }
    else if (adm_total >= L.rate_total) { ok = 0; put(why, cap, &k, "система запускает больше "); put_n(why, cap, &k, L.rate_total); put(why, cap, &k, " программ в секунду"); }
    if (ok) { adm[slot].n++; adm_total++; }
    else { adm_denied++; int i = 0; for (; name[i] && i < 15; i++) adm_denied_name[i] = name[i]; adm_denied_name[i] = 0; }
    spin_unlock_irqrestore(&adm_lock, f);
    return ok;
}

static int name_is(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void gather(uint64_t seconds) {
    mind_sample_t* s = &S;
    __builtin_memset(s, 0, sizeof *s);
    s->t = (uint32_t)seconds;
    s->tsc_khz = clock_tsc_khz();
    unsigned n = percpu_count(), cores = 0;
    uint64_t busy_sum = 0;
    for (unsigned i = 0; i < n && i < 256; i++) {
        uint64_t idle, total, sw;
        if (sched_cpu_stats(i, &idle, &total, &sw) != 0) continue;
        uint64_t di = idle - prev_idle[i], dt = total - prev_total[i];
        prev_idle[i] = idle; prev_total[i] = total;
        uint32_t b = dt ? (uint32_t)(100 - (di >= dt ? 100 : di * 100 / dt)) : 0;
        s->busy[cores < 256 ? cores : 255] = (uint8_t)b;
        busy_sum += b; cores++;
    }
    s->cores = cores ? cores : 1;
    s->load = cores ? (uint32_t)(busy_sum / cores) : 0;
    s->runnable = sched_runnable();
    uint32_t total_kb = pmm_get_total_memory_kb(), free_kb = pmm_get_free_memory_kb();
    s->mem_total_mb = total_kb / 1024;
    s->mem_used = total_kb ? 100 - (uint32_t)((uint64_t)(free_kb > total_kb ? total_kb : free_kb) * 100 / total_kb) : 0;

    uint64_t loops, late, gap;
    display_get_stats(&loops, &late, &gap);
    s->loops = (uint32_t)(loops - prev_loops); s->late = (uint32_t)(late - prev_late);
    prev_loops = loops; prev_late = late;
    s->worst_gap_ms = (uint32_t)(gap / 1000000);

    uint64_t bn, bt;
    blk_stats(&bn, &bt);
    s->disk_ops = (uint32_t)(bn - prev_blk_n);
    uint64_t dt = bt - prev_blk_t;
    s->disk_lat_us = s->disk_ops && s->tsc_khz ? (uint32_t)(dt * 1000 / s->tsc_khz / s->disk_ops) : 0;
    prev_blk_n = bn; prev_blk_t = bt;

    uint64_t msgs = kobj_messages_total();
    s->ipc = (uint32_t)(msgs - prev_msgs); prev_msgs = msgs;
    uint32_t f = proc_faults_total();
    s->faults = f - prev_faults; prev_faults = f;
    uint64_t sw = sched_total_switches();
    s->switches = (uint32_t)(sw - prev_sw); prev_sw = sw;

    devhealth_take(&s->dev_errors, s->dev_name);
    devhealth_hid_take(&s->hid_suspect, &s->hid_rate, s->hid_name);
    {
        uint64_t f = spin_lock_irqsave(&adm_lock);
        s->denied = adm_denied; adm_denied = 0;
        for (int i = 0; i < 16; i++) s->denied_name[i] = adm_denied_name[i];
        spin_unlock_irqrestore(&adm_lock, f);
    }

    static proc_stat_t ps[MIND_PROC_MAX];                   /* 256 записей - не на стек потока */
    unsigned np = proc_stats(ps, MIND_PROC_MAX);
    int focus = gfx_focus_pid();
    for (unsigned i = 0; i < np; i++) {
        mind_proc_t* p = &s->proc[s->nproc++];
        p->pid = ps[i].pid;
        for (unsigned k = 0; k < sizeof p->name; k++) p->name[k] = ps[i].name[k];
        p->syscalls = ps[i].syscalls; p->heap_kb = ps[i].heap_kb; p->threads = ps[i].threads;
        p->focus = focus > 0 && (uint32_t)focus == ps[i].pid;
    }
    {   /* для рычагов: звук, объём обмена с дисками */
        uint64_t mixed, under, by = blk_bytes();
        audio_stats(&mixed, &under);
        s->audio_mixed = (uint32_t)(mixed - prev_a_mixed); s->audio_under = (uint32_t)(under - prev_a_under);
        s->disk_kb = (uint32_t)((by - prev_bytes) / 1024);
        prev_a_mixed = mixed; prev_a_under = under; prev_bytes = by;
    }
    unsigned nt = tinfo ? sched_snapshot(tinfo, tcap) : 0;
    {   /* доля ядра потоков mind-tact и audio за секунду (% x100): цена тактики и микшера */
        uint64_t tact = 0, aud = 0;
        for (unsigned i = 0; i < nt; i++) {
            if (name_is(tinfo[i].name, "mind-tact")) tact += tinfo[i].cpu_cycles;
            else if (name_is(tinfo[i].name, "audio")) aud += tinfo[i].cpu_cycles;
        }
        if (s->tsc_khz) {
            s->tact_cpu_x100 = tact >= prev_tact_cyc ? (uint32_t)((tact - prev_tact_cyc) * 10 / s->tsc_khz) : 0;
            s->audio_cpu_x100 = aud >= prev_aud_cyc ? (uint32_t)((aud - prev_aud_cyc) * 10 / s->tsc_khz) : 0;
        }
        prev_tact_cyc = tact; prev_aud_cyc = aud;
    }
    for (unsigned i = 0; i < nt; i++) {
        const thread_info_t* t = &tinfo[i];
        uint32_t wms = (uint32_t)(t->waiting * 1000 / TIMER_HZ);
        if (wms > s->max_wait_ms) s->max_wait_ms = wms;
        if (!t->pid) {
            if (wms > s->kwait_ms) { s->kwait_ms = wms; for (unsigned k = 0; k < 16; k++) s->kwait_name[k] = t->name[k]; }
            continue;
        }
        for (unsigned k = 0; k < s->nproc; k++) {
            if (s->proc[k].pid != t->pid) continue;
            s->proc[k].cpu_cycles += t->cpu_cycles;
            if (wms > s->proc[k].wait_ms) s->proc[k].wait_ms = wms;
        }
    }
}

/* ---------------- снимок для оболочки ---------------- */

static void scopy(char* d, const char* s, unsigned cap) { unsigned i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

static void publish(void) {
    static mind_view_t v;
    v.t = S.t; v.lifetime_s = M.m.lifetime_s;
    v.health = M.health; v.c_resp = M.c_resp; v.c_room = M.c_room; v.c_bal = M.c_bal; v.c_stab = M.c_stab;
    scopy(v.mood, M.mood ? M.mood : "", sizeof v.mood);
    v.mode = M.m.mode; v.fx = (uint8_t)M.fx; v.fx_user = M.m.fx_user;
    v.quantum_ms = mind_quantum_ms(&M);
    v.cores = S.cores; v.load = S.load; v.mem_used = S.mem_used; v.runnable = S.runnable;
    for (unsigned i = 0; i < 64; i++) v.busy[i] = S.busy[i];
    v.frames_per_s = S.loops; v.late_per_s = S.late;
    for (int k = 0; k < MIND_SIG_N; k++) {
        v.cur_x100[k] = M.cur_x100[k]; v.mean_x100[k] = (int32_t)M.m.norm[k].mean_x100; v.z_x10[k] = M.z_x10[k];
        v.learned[k] = M.m.norm[k].n;
        for (int i = 0; i < MIND_HIST; i++) v.hist[k][i] = M.hist[k][i];
    }
    for (int i = 0; i < MIND_HIST; i++) v.health_hist[i] = M.health_hist[i];
    v.hpos = M.hpos;
    v.decisions = M.m.decisions; v.anomalies = M.m.anomalies; v.kills = M.m.kills;
    v.tune_trials = M.m.tune_trials; v.tune_wins = M.m.tune_wins;
    v.activity = M.activity;
    v.nj = 0;
    for (unsigned i = 0; i < MIND_VIEW_J; i++) {
        const mind_event_t* e = mind_journal(&M, i);
        if (!e) break;
        v.j[i].t = e->t; v.j[i].kind = e->kind; v.j[i].applied = e->applied;
        scopy(v.j[i].text, e->text, MIND_TEXT);
        v.nj++;
    }
    v.tz_min = M.m.tz_min; v.rtc_local = M.m.rtc_local;
    v.risk = M.risk_q16 * 100 / MNET_ONE; v.risk_why = M.risk_why;
    v.net_n = M.m.net.n; v.net_pos = M.m.net.pos; v.net_prec = mnet_precision(&M.m.net); v.net_rec = mnet_recall(&M.m.net);
    v.net_trusted = (uint8_t)mnet_trusted(&M.m.net); v.forecasts = M.m.forecasts; v.net_loss_x1e4 = M.m.net.loss_x1e4;
    for (int i = 0; i < MNET_F; i++) v.net_w[i] = M.m.net.w[i];
    v.bg_fps = mind_bg_fps(&M); v.core_cap = mind_core_cap(&M);
    v.core_res_pid = mind_core_reserved_pid(&M); v.core_res = sched_reserved_core();
    display_compositor_stats(&v.comp_par, &v.comp_ns_kpx_par, &v.comp_ns_kpx_ser);
    for (int L = 0; L < ML_N; L++) {
        v.lv[L].val = mind_lever_val(&M, L); v.lv[L].def = mind_lever_default(L);
        unsigned tr, wi, ba; mind_lever_stats(&M, L, &tr, &wi, &ba);
        v.lv[L].trials = (uint16_t)tr; v.lv[L].wins = (uint16_t)wi; v.lv[L].backs = (uint16_t)ba;
        v.lv[L].state = (uint8_t)mind_lever_state(&M, L);
    }
    v.lv_changes = M.lv.total_changes; v.lv_general = M.lv.general_rollbacks; v.lv_seed = (M.m.mflags & MF_SEED) != 0;
    v.nproc = 0;
    for (int i = 0; i < MIND_PROC_MAX && v.nproc < 16; i++) {
        const mind_track_t* t = &M.tr[i];
        if (!t->alive) continue;
        __typeof__(v.p[0])* o = &v.p[v.nproc++];
        o->pid = t->pid; o->cpu_x10 = t->cpu_x10; o->sysc = t->sysc_rate; o->weight = t->weight; o->limit = t->limit;
        o->flagged = t->flagged == 1; o->focus = t->pid == M.focus_pid;
        o->threads = 0; o->heap_kb = 0;
        for (unsigned k = 0; k < S.nproc; k++) if (S.proc[k].pid == t->pid) { o->threads = S.proc[k].threads; o->heap_kb = S.proc[k].heap_kb; }
        scopy(o->name, t->name, sizeof o->name);
    }
    uint64_t f = spin_lock_irqsave(&view_lock);
    __builtin_memcpy(&V, &v, sizeof V);
    spin_unlock_irqrestore(&view_lock, f);
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
}

void mind_sys_view(mind_view_t* out) {
    uint64_t f = spin_lock_irqsave(&view_lock);
    __builtin_memcpy(out, &V, sizeof V);
    spin_unlock_irqrestore(&view_lock, f);
}

uint32_t mind_sys_version(void) { return __atomic_load_n(&version, __ATOMIC_ACQUIRE); }

/* ---------------- жизнь ---------------- */

void mind_sys_init(void) {
    mind_model_t saved;
    int have = 0;
    if (kfs_mounted()) {                                    /* опыт прежнего формата поднимается до текущего */
        static uint8_t raw[sizeof(mind_model_t) + 16];
        int64_t got = kfs_read(MODEL_PATH, 0, raw, sizeof raw);
        if (got > 0 && mind_model_load(&saved, raw, (unsigned)got)) have = 1;
        if (!have) {                                        /* опыта этой машины нет: семя (data/mind_seed.bin -> /etc/mind_seed.bin) */
            got = kfs_read(SEED_PATH, 0, raw, sizeof raw);
            if (got > 0 && mind_model_seed(&saved, raw, (unsigned)got)) have = 1;
        }
    }
    unsigned cores = sched_cpu_count();
    mind_init(&M, have ? &saved : 0, cores ? cores : 1);
    tcap = sched_thread_capacity();
    tinfo = (thread_info_t*)kmalloc(sizeof(thread_info_t) * tcap);
    tt_cap = tcap;
    tt_info = (thread_info_t*)kmalloc(sizeof(thread_info_t) * tt_cap);
    ready = 1;
    if (tt_info && !sched_create("mind-tact", tact_main, 0, PRIO_CRITICAL))
        uart_print("[KIRA MIND] cannot start the fast (tactical) loop.\n");
    publish();
    uart_print("[KIRA MIND] Mind online: learns this machine's norms (load, queue, memory, frame delays, disk, IPC, faults, syscalls), ");
    uart_print("tunes the scheduler quantum by measurement, limits background programs when the screen is late, ");
    uart_print("watches program behaviour against saved profiles, manages memory pressure. Mode: ");
    uart_print(M.m.mode == MIND_ACT ? "act" : "observe");
    uart_print(have ? ", experience restored from " MODEL_PATH ".\n" : ", first life (no saved model).\n");
}

void mind_sys_save(void) {
    if (!ready || !kfs_mounted()) return;
    kfs_stat_t st;
    if (kfs_stat("/var", &st) != 0 && kfs_mkdir("/var") != 0) return;
    mind_model_t m = M.m;
    mind_model_seal(&m);
    if (kfs_write(MODEL_PATH, &m, sizeof m) != 0) uart_print("[KIRA MIND] cannot save the model to disk.\n");
}

static void put_s(char* b, unsigned* n, const char* s) { while (*s && *n < 250) b[(*n)++] = *s++; b[*n] = 0; }
static void put_u(char* b, unsigned* n, uint64_t v) {
    char t[21]; unsigned k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && *n < 250) b[(*n)++] = t[--k];
    b[*n] = 0;
}

void mind_sys_tick(uint64_t seconds) {
    if (!ready) return;
    if (req_mode >= 0) { mind_set_mode(&M, req_mode, &act); req_mode = -1; mind_sys_save(); }
    if (req_fx >= 0) { mind_set_fx_user(&M, req_fx, &act); req_fx = -1; mind_sys_save(); }
    if (req_tz != 0x7FFFFFFF) { M.m.tz_min = req_tz; req_tz = 0x7FFFFFFF; mind_sys_save(); }
    if (req_rtc >= 0) { M.m.rtc_local = (uint8_t)req_rtc; req_rtc = -1; mind_sys_save(); }
    if (req_cap >= 0) { M.m.core_cap = (uint8_t)req_cap; req_cap = -1; mind_sys_save(); }
    gather(seconds);
    kmutex_lock(&mmx);
    mind_step(&M, &S, &act);
    kmutex_unlock(&mmx);
    /* Новые записи журнала - только в последовательный порт (стенд, отладка). На экране их показывает окно Разума
       и команда ai; в терминал они не копируются, иначе вывод Разума дублировался бы там. */
    uint32_t fresh = M.jn - printed;
    if (fresh > MIND_JOURNAL) fresh = MIND_JOURNAL;
    for (uint32_t i = fresh; i > 0; i--) {
        const mind_event_t* e = mind_journal(&M, i - 1);
        if (!e) continue;
        char jl[256]; unsigned jn = 0;
        put_s(jl, &jn, "[KIRA MIND] "); put_s(jl, &jn, e->text); put_s(jl, &jn, "\n");
        uart_print_serial(jl);
    }
    printed = M.jn;
    publish();
    char line[256]; unsigned n = 0;
    put_s(line, &n, "[KIRA TELEMETRY] t="); put_u(line, &n, seconds);
    put_s(line, &n, "s health="); put_u(line, &n, M.health);
    put_s(line, &n, " load="); put_u(line, &n, S.load);
    put_s(line, &n, "% mem="); put_u(line, &n, S.mem_used);
    put_s(line, &n, "% run="); put_u(line, &n, S.runnable);
    put_s(line, &n, " frames="); put_u(line, &n, S.loops);
    put_s(line, &n, " late="); put_u(line, &n, S.late);
    put_s(line, &n, " q="); put_u(line, &n, mind_quantum_ms(&M));
    put_s(line, &n, "ms fx="); put_u(line, &n, (uint64_t)M.fx);
    put_s(line, &n, "\n");
    if (telemetry) uart_print(line); else uart_print_serial(line);
    if (seconds - last_save >= SAVE_PERIOD) { last_save = seconds; mind_sys_save(); }
}

/* ---- тактика: ~40 раз в секунду ---- */
static struct { uint32_t pid; uint64_t cyc; } tt_prev[MIND_TACT_MAXP];
static uint64_t tt_idle[PERCPU_MAX_CPUS], tt_total[PERCPU_MAX_CPUS], tt_loops, tt_late;
static mind_tact_in_t TI;
static uint64_t tt_cyc[MIND_TACT_MAXP];

static void tact_main(void* arg) {
    (void)arg;
    uint64_t last = clock_ms();
    for (;;) {
        uint32_t period = (uint32_t)((uint64_t)TIMER_HZ * (uint32_t)mind_lever_val(&M, ML_TACT_PERIOD) / 1000);   /* рычаг 18: 25 мс по умолчанию */
        sched_sleep_ticks(period ? period : 1);
        uint64_t now = clock_ms(), khz = clock_tsc_khz();
        uint32_t win = (uint32_t)(now - last);
        if (!khz || win == 0) { last = now; continue; }
        last = now;
        mind_tact_in_t* in = &TI;
        in->t_ms = (uint32_t)now; in->window_ms = win; in->nproc = 0; in->busy_x10 = 0; in->fg_wait_ms = 0;
        unsigned n = percpu_count(), cores = 0;
        for (unsigned i = 0; i < n && i < PERCPU_MAX_CPUS; i++) {
            uint64_t idle, total, sw;
            if (sched_cpu_stats(i, &idle, &total, &sw) != 0) continue;
            uint64_t di = idle - tt_idle[i], dt = total - tt_total[i];
            tt_idle[i] = idle; tt_total[i] = total;
            if (dt) in->busy_x10 += (uint32_t)(1000 - (di >= dt ? 1000 : di * 1000 / dt));
            cores++;
        }
        in->cores = cores ? cores : 1;
        uint64_t loops, late, gap;
        display_get_stats(&loops, &late, &gap);
        in->loops = (uint32_t)(loops - tt_loops); in->late = (uint32_t)(late - tt_late);
        tt_loops = loops; tt_late = late;
        int focus = gfx_focus_pid();
        unsigned nt = sched_snapshot(tt_info, tt_cap);
        for (unsigned i = 0; i < nt; i++) {
            const thread_info_t* t = &tt_info[i];
            uint32_t wms = (uint32_t)(t->waiting * 1000 / TIMER_HZ);
            if ((t->pid == 0 && t->prio != PRIO_IDLE) || (focus > 0 && t->pid == (uint32_t)focus))
                if (wms > in->fg_wait_ms) in->fg_wait_ms = wms;
            if (!t->pid) continue;
            unsigned k = 0;
            while (k < in->nproc && in->p[k].pid != t->pid) k++;
            if (k == in->nproc) {
                if (k >= MIND_TACT_MAXP) continue;
                in->p[k].pid = t->pid; in->p[k].threads = 0; in->p[k].cpu_x10 = 0; in->nproc++;
                tt_cyc[k] = 0;
            }
            in->p[k].threads++;
            tt_cyc[k] += t->cpu_cycles;
        }
        for (unsigned k = 0; k < in->nproc; k++) {                       /* такты за окно -> доля ядра */
            uint64_t prev = 0;
            for (unsigned j = 0; j < MIND_TACT_MAXP; j++) if (tt_prev[j].pid == in->p[k].pid) { prev = tt_prev[j].cyc; break; }
            uint64_t dc = tt_cyc[k] >= prev ? tt_cyc[k] - prev : 0;
            in->p[k].cpu_x10 = (uint32_t)(dc * 1000 / (khz * win));
        }
        for (unsigned j = 0; j < MIND_TACT_MAXP; j++) {
            tt_prev[j].pid = j < in->nproc ? in->p[j].pid : 0;
            tt_prev[j].cyc = j < in->nproc ? tt_cyc[j] : 0;
        }
        kmutex_lock(&mmx);
        mind_tact_step(&M, in, &act);
        kmutex_unlock(&mmx);
    }
}

void mind_sys_request_cap(int pct) { if (pct >= 10 && pct <= 100) req_cap = pct; }
unsigned mind_sys_core_cap(void) { return mind_core_cap(&M); }

void mind_sys_request_mode(int mode) { req_mode = mode ? MIND_ACT : MIND_OBSERVE; }
void mind_sys_request_fx(int fx) { if (fx >= 0 && fx <= MIND_FX_AUTO) req_fx = fx; }
void mind_sys_request_tz(int32_t minutes) { if (minutes >= -720 && minutes <= 840) req_tz = minutes; }
void mind_sys_set_telemetry(int on) { telemetry = on != 0; }
int  mind_sys_telemetry(void) { return telemetry; }
int32_t mind_sys_tz(void) { return M.m.tz_min; }

void mind_sys_request_rtc_local(int on) { req_rtc = on ? 1 : 0; }
int  mind_sys_rtc_local(void) { return M.m.rtc_local != 0; }
int32_t mind_sys_local_offset(void) { return M.m.rtc_local ? 0 : M.m.tz_min; }

/* ---------------- консоль ---------------- */

static void pr_x100(int64_t v) {
    if (v < 0) { uart_print("-"); v = -v; }
    uart_print_dec((uint64_t)(v / 100));
    uint64_t f = (uint64_t)(v % 100);
    if (f) { uart_print(","); if (f < 10) uart_print("0"); uart_print_dec(f % 10 == 0 ? f / 10 : f); }
}

void mind_sys_print(const char* what) {
    mind_view_t* v = (mind_view_t*)kmalloc(sizeof(mind_view_t));
    if (!v) return;
    mind_sys_view(v);
    if (what[0] == 's') {                                   /* save: записать опыт сейчас (тренажёр mind-train забирает файл с диска) */
        mind_sys_save();
        uart_print("[MIND] опыт записан: /var/mind.dat\n");
    } else if (what[0] == 'w') {                                   /* why: журнал решений */
        uart_print("\n[MIND] решения и наблюдения (свежие сверху):\n");
        for (unsigned i = 0; i < v->nj; i++) {
            uart_print("  "); uart_print_dec(v->j[i].t); uart_print(" с  "); uart_print(v->j[i].text); uart_print("\n");
        }
        if (!v->nj) uart_print("  пока пусто\n");
        uart_print("\n");
    } else if (what[0] == 'n') {                            /* norms: что выучено */
        uart_print("\n[MIND] нормы этой машины (сейчас / норма / отклонение / секунд наблюдения):\n");
        for (int k = 0; k < MIND_SIG_N; k++) {
            uart_print("  "); uart_print(mind_signal_name(k)); uart_print(": ");
            pr_x100(v->cur_x100[k]); uart_print(" / "); pr_x100(v->mean_x100[k]); uart_print(" "); uart_print(mind_signal_unit(k));
            uart_print(" / z="); pr_x100((int64_t)v->z_x10[k] * 10);
            uart_print(" / "); uart_print_dec(v->learned[k]); uart_print("\n");
        }
        uart_print("\n");
    } else if (what[0] == 'l') {                            /* levers: чем Разум управляет и что сейчас выставлено */
        uart_print("\n[MIND] рычаги (что Разум меняет сам; режим ");
        uart_print(v->mode ? "управление" : "наблюдение - только записывает"); uart_print("):\n");
        uart_print("  1. квант планировщика: "); uart_print_dec(v->quantum_ms); uart_print(" мс (сам пробует соседние и оставляет лучший)\n");
        uart_print("  2. вес потоков: окно в фокусе x1,5, голодающий x2; проснувшийся более тяжёлый вытесняет лёгкий сразу (рефлекс в планировщике)\n");
        uart_print("  3. доля ядра фоновых программ: при нехватке процессора фон получает не больше "); uart_print_dec(v->core_cap);
        uart_print("% всех ядер и не больше свободного (ai cap N)\n");
        uart_print("  4. темп фоновых окон: "); if (v->bg_fps) { uart_print("не чаще "); uart_print_dec(v->bg_fps); uart_print(" кадров/с"); } else uart_print("без предела");
        uart_print(" (ступени 30/15/8/4 по опозданию экрана)\n");
        uart_print("  5. сборка кадра: "); uart_print(v->comp_par ? "на всех ядрах" : "на одном ядре"); uart_print(" (по замеренной цене)\n");
        uart_print("  6. анимация фона: уровень "); uart_print_dec(v->fx); uart_print(v->fx_user == MIND_FX_AUTO ? " (решает Разум)\n" : " (задана вручную)\n");
        uart_print("  7. допуск запусков: копий одной программы и запусков в секунду - по ядрам и здоровью\n");
        uart_print("  8. память: возврат запасов ядра, осторожный рост куч, в крайнем случае остановка самой большой программы\n");
        uart_print("  9. поведение программ: необычная по своему профилю программа ограничивается до 50% ядра\n");
        uart_print(" 10. устройства ввода: печатающее быстрее человека (BadUSB) - отключается\n");
        uart_print(" 11. предвидение: при доверии модели фон ужимается до опоздания экрана (сейчас риск "); uart_print_dec(v->risk); uart_print("%)\n");
        uart_print(" 12. ядро фокуса: при опоздании экрана (3+ ядра) одно ядро - окну в фокусе, фон - на остальных: ");
        if (v->core_res >= 0) { uart_print("ядро "); uart_print_dec((uint64_t)v->core_res); uart_print(" отдано pid "); uart_print_dec(v->core_res_pid); }
        else uart_print("сейчас общее");
        uart_print("\n\n");
        uart_print("  Рычаги 13-30 (каждый проверяется по замеру: проба соседнего значения, сравнение с базой, подтверждение вторым окном, "
                   "откат при ухудшении; не больше одной пробы за раз; 5 смен в минуту; общий откат, если экран стал опаздывать сильнее). "
                   "Колонки: номер, имя, значение (обычное), состояние, проб/удач/откатов.\n");
        for (int L = 0; L < ML_N; L++) {
            uart_print(" "); uart_print_dec((uint64_t)(13 + L)); uart_print(". "); uart_print(mind_lever_name(L)); uart_print(": ");
            uart_print_dec((uint64_t)v->lv[L].val); uart_print(" "); uart_print(mind_lever_unit(L));
            uart_print(" (обычно "); uart_print_dec((uint64_t)v->lv[L].def); uart_print(") - ");
            static const char* const st[] = { "обычное значение", "изменено по замеру", "идёт проба", "не помогает при такой нагрузке - не трогаю", "" };
            uart_print(st[v->lv[L].state < 4 ? v->lv[L].state : 4]);
            uart_print("; проб "); uart_print_dec(v->lv[L].trials); uart_print(", удач "); uart_print_dec(v->lv[L].wins);
            uart_print(", откатов "); uart_print_dec(v->lv[L].backs); uart_print("\n");
        }
        uart_print("  смен значений за запуск "); uart_print_dec(v->lv_changes); uart_print(", общих откатов "); uart_print_dec(v->lv_general);
        if (v->lv_seed) uart_print("; стартовые подсказки взяты из семени опыта (получено в QEMU)");
        uart_print("\n\n");
    } else if (what[0] == 'f') {                            /* forecast: качество и объяснение текущего прогноза */
        uart_print("\n[MIND] прогноз на следующую секунду\n  риск опоздания экрана: ");
        uart_print_dec(v->risk); uart_print("%\n  главная причина: ");
        uart_print(v->risk_why >= 0 ? mind_feature_name(v->risk_why) : "пока не определена");
        uart_print("\n  статус модели: "); uart_print(v->net_trusted ? "допущена к упреждению" : "недостаточно подтверждена - только наблюдение");
        uart_print("\n  обучающих секунд: "); uart_print_dec(v->net_n);
        uart_print("; положительных исходов: "); uart_print_dec(v->net_pos);
        uart_print("\n  точность предупреждений: "); uart_print_dec(v->net_prec);
        uart_print("%; полнота: "); uart_print_dec(v->net_rec);
        uart_print("%; прогнозов с упреждением: "); uart_print_dec(v->forecasts);
        uart_print("\n  средняя лог-потеря x1e4: "); uart_print_dec(v->net_loss_x1e4);
        uart_print("\n  примечание: это статистический прогноз одного события, не языковой ИИ.\n\n");
    } else if (what[0] == 'm') {                            /* model: веса модели предвидения */
        uart_print("\n[MIND] модель предвидения (логистическая регрессия, учится каждую секунду; вес > 0 - признак ведёт к опозданию):\n");
        for (int i = 0; i < MNET_F; i++) {
            uart_print("  "); uart_print(mind_feature_name(i)); uart_print(": ");
            int32_t w = v->net_w[i];
            if (w < 0) { uart_print("-"); w = -w; }
            uart_print_dec((uint64_t)w / MNET_ONE); uart_print(".");
            uint64_t fr = ((uint64_t)w % MNET_ONE) * 100 / MNET_ONE; if (fr < 10) uart_print("0"); uart_print_dec(fr);
            uart_print("\n");
        }
        uart_print("  обучено на "); uart_print_dec(v->net_n); uart_print(" с, средняя ошибка (лог-потеря) 0.");
        uint64_t l = v->net_loss_x1e4; if (l < 1000) uart_print("0"); if (l < 100) uart_print("0"); if (l < 10) uart_print("0"); uart_print_dec(l);
        uart_print("\n\n");
    } else if (what[0] == 'p') {                            /* procs: что разум знает о программах */
        uart_print("\n[MIND] программы: pid имя  процессор  вызовы/с  куча  вес  предел\n");
        for (unsigned i = 0; i < v->nproc; i++) {
            uart_print("  "); uart_print_dec(v->p[i].pid); uart_print(" "); uart_print(v->p[i].name);
            uart_print("  "); uart_print_dec(v->p[i].cpu_x10 / 10); uart_print("%  ");
            uart_print_dec(v->p[i].sysc); uart_print("  "); uart_print_dec(v->p[i].heap_kb); uart_print(" КБ  ");
            uart_print_dec(v->p[i].weight); uart_print("/4  "); uart_print_dec(v->p[i].limit); uart_print("%");
            if (v->p[i].focus) uart_print("  [фокус]");
            if (v->p[i].flagged) uart_print("  [необычно]");
            uart_print("\n");
        }
        if (!v->nproc) uart_print("  программ нет\n");
        uart_print("\n");
    } else {
        uart_print("\n[MIND] здоровье "); uart_print_dec(v->health); uart_print(" ("); uart_print(v->mood);
        uart_print("): отзывчивость "); uart_print_dec(v->c_resp);
        uart_print(", запас памяти "); uart_print_dec(v->c_room);
        uart_print(", баланс ядер "); uart_print_dec(v->c_bal);
        uart_print(", стабильность "); uart_print_dec(v->c_stab);
        uart_print("\n[MIND] режим: "); uart_print(v->mode ? "управление" : "наблюдение");
        uart_print(", квант "); uart_print_dec(v->quantum_ms); uart_print(" мс (проб "); uart_print_dec(v->tune_trials);
        uart_print(", удачных "); uart_print_dec(v->tune_wins); uart_print("), анимация ");
        uart_print_dec(v->fx); uart_print(v->fx_user == MIND_FX_AUTO ? " (авто)" : " (вручную)");
        uart_print("\n[MIND] за жизнь: "); uart_print_dec(v->lifetime_s); uart_print(" с, решений ");
        uart_print_dec(v->decisions); uart_print(", необычностей "); uart_print_dec(v->anomalies);
        uart_print(", остановлено программ "); uart_print_dec(v->kills);
        uart_print("\n[MIND] предвидение: риск опоздания экрана в следующую секунду "); uart_print_dec(v->risk);
        uart_print("%; модель обучена на "); uart_print_dec(v->net_n); uart_print(" с (опозданий "); uart_print_dec(v->net_pos);
        uart_print("), предупреждения сбываются "); uart_print_dec(v->net_prec); uart_print("%, замечено опозданий заранее ");
        uart_print_dec(v->net_rec); uart_print("%, "); uart_print(v->net_trusted ? "доверие есть" : "пока без права действовать");
        uart_print(", упреждений "); uart_print_dec(v->forecasts);
        uart_print("\n[MIND] тактика: фон - не больше "); uart_print_dec(v->core_cap); uart_print("% ядер при нехватке; фоновые окна - ");
        if (v->bg_fps) { uart_print("не чаще "); uart_print_dec(v->bg_fps); uart_print(" кадров/с"); } else uart_print("без предела кадров");
        uart_print("\n[MIND] композитор: ");
        uart_print(v->comp_par ? "параллельно на всех ядрах" : "на одном ядре");
        uart_print(" (цена 1000 точек: параллельно "); uart_print_dec(v->comp_ns_kpx_par); uart_print(" нс, на одном ядре ");
        uart_print_dec(v->comp_ns_kpx_ser); uart_print(" нс; 0 - ещё не мерили)");
        {   /* общий замок планировщика: доля времени ядер в очереди за ним (с запуска) */
            uint64_t w = 0, tot = 0, cont = 0, taken = 0;
            for (unsigned c = 0; c < sched_cpu_count(); c++) {
                uint64_t wc, cc, tk, total;
                if (sched_cpu_stats(c, 0, &total, 0) != 0) continue;
                sched_lock_stats(c, &wc, &cc, &tk);
                w += wc; tot += total; cont += cc; taken += tk;
            }
            uint64_t pm = tot ? w * 1000 / tot : 0;
            uart_print("\n[MIND] замок планировщика: ядра ждут его "); uart_print_dec(pm / 10); uart_print(",");
            uart_print_dec(pm % 10); uart_print("% времени; взятий с ожиданием "); uart_print_dec(cont);
            uart_print(" из "); uart_print_dec(taken);
        }
        uart_print("\n[MIND] подробнее: ai why (журнал), ai norms (нормы), ai procs (программы), ai model (веса), ai levers (рычаги), ai act | ai observe, ai fx auto|0..3\n\n");
    }
    kfree(v);
}
