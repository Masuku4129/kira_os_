#include "mind.h"
#include <stdarg.h>

/* ---------------- мелочи без libc ---------------- */

#define ML(s, L) mind_lever_val((s), (L))
static void mzero(void* p, unsigned long n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }
static void mcopy(void* d, const void* src, unsigned long n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)src; while (n--) *a++ = *b++; }
static void scopy(char* d, const char* s, unsigned cap) { unsigned i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

static uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0, b = 1ULL << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

static unsigned ilog2u(uint64_t v) { unsigned n = 0; while (v > 1) { v >>= 1; n++; } return n; }

/* Мини-форматирование: %u (uint32), %d (int32), %s, %%. Всегда завершает строку нулём. */
static void vfmt(char* out, unsigned cap, const char* f, va_list ap) {
    unsigned n = 0;
    while (*f && n + 1 < cap) {
        if (*f != '%') { out[n++] = *f++; continue; }
        f++;
        if (*f == 's') {
            const char* s = va_arg(ap, const char*);
            while (s && *s && n + 1 < cap) out[n++] = *s++;
        } else if (*f == 'u' || *f == 'd') {
            int64_t v = *f == 'u' ? (int64_t)va_arg(ap, uint32_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) { if (n + 1 < cap) out[n++] = '-'; v = -v; }
            char t[21]; int k = 0;
            do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (k && n + 1 < cap) out[n++] = t[--k];
        } else if (*f == '%') {
            out[n++] = '%';
        }
        if (*f) f++;
    }
    out[n] = 0;
}

/* ---------------- модель и сигналы ---------------- */

static const char* const sig_name[MIND_SIG_N] = { "нагрузка", "очередь", "память", "опоздания кадров", "диск", "обмен", "сбои", "вызовы" };
static const char* const sig_unit[MIND_SIG_N] = { "%", "потоков/ядро", "%", "/с", "мкс", "сообщ./с", "/с", "/с" };
/* Нижняя граница разброса (x100): меньше этого колебания - не необычность, а шум. */
static const int64_t sd_floor[MIND_SIG_N] = { 800, 50, 300, 200, 50000, 10000, 100, 100000 };

const char* mind_signal_name(int sig) { return sig >= 0 && sig < MIND_SIG_N ? sig_name[sig] : "?"; }
const char* mind_signal_unit(int sig) { return sig >= 0 && sig < MIND_SIG_N ? sig_unit[sig] : ""; }

static const uint8_t Qv[MIND_NQ] = { 2, 3, 4, 6, 8, 12 };   /* кванты-кандидаты, x10 мс */

static unsigned bucket(unsigned cores) {
    return cores <= 1 ? 0 : cores <= 2 ? 1 : cores <= 4 ? 2 : cores <= 8 ? 3 : cores <= 16 ? 4 : cores <= 32 ? 5 : 6;
}

static uint32_t crc32_m(const void* data, unsigned long len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

void mind_model_seal(mind_model_t* m) {
    m->magic = MIND_MAGIC; m->version = MIND_VERSION; m->crc = 0;
    m->crc = crc32_m(m, sizeof *m);
}

int mind_model_valid(const mind_model_t* m) {
    if (m->magic != MIND_MAGIC || m->version != MIND_VERSION) return 0;
    mind_model_t t = *m;
    uint32_t c = t.crc; t.crc = 0;
    return crc32_m(&t, sizeof t) == c;
}

int mind_model_load(mind_model_t* out, const void* buf, unsigned len) {
    if (len == sizeof(mind_model_t)) {
        mcopy(out, buf, sizeof *out);
        return mind_model_valid(out);
    }
    if (len == sizeof(mind_model_v4_t)) {
        mind_model_v4_t o;
        mcopy(&o, buf, sizeof o);
        if (o.magic != MIND_MAGIC || o.version != 4) return 0;
        uint32_t csum = o.crc; o.crc = 0;
        if (crc32_m(&o, sizeof o) != csum) return 0;
        mzero(out, sizeof *out);
        mcopy(out, &o, __builtin_offsetof(mind_model_v4_t, net));
        out->net = o.net;
        mnet_init(&out->memory_net);
        out->forecasts = o.forecasts;
        mcopy(out->lv, o.lv, sizeof out->lv);
        out->mflags = o.mflags;
        mind_model_seal(out);
        return 1;
    }
    if (len == sizeof(mind_model_v3_t)) {
        mind_model_v3_t o;
        mcopy(&o, buf, sizeof o);
        if (o.magic != MIND_MAGIC || o.version != 3) return 0;
        uint32_t c = o.crc; o.crc = 0;
        if (crc32_m(&o, sizeof o) != c) return 0;
        mzero(out, sizeof *out);
        mcopy(out, &o, __builtin_offsetof(mind_model_v3_t, net));
        for (int i = 0; i < 12; i++) out->net.w[i] = o.net.w[i];
        out->net.b = o.net.b; out->net.n = o.net.n; out->net.pos = o.net.pos;
        out->net.tp = o.net.tp; out->net.fp = o.net.fp; out->net.fn = o.net.fn;
        out->net.tn = o.net.tn; out->net.loss_x1e4 = o.net.loss_x1e4;
        mnet_init(&out->memory_net);
        out->forecasts = o.forecasts;
        mcopy(out->lv, o.lv, sizeof out->lv);
        out->mflags = o.mflags;
        mind_model_seal(out);
        return 1;
    }
    if (len == sizeof(mind_model_v2_t)) {                     /* версия 2: те же поля без рычагов */
        mind_model_v2_t o;
        mcopy(&o, buf, sizeof o);
        if (o.magic != MIND_MAGIC || o.version != 2) return 0;
        uint32_t c = o.crc; o.crc = 0;
        if (crc32_m(&o, sizeof o) != c) return 0;
        mzero(out, sizeof *out);
        mcopy(out, &o, __builtin_offsetof(mind_model_v2_t, net));
        for (int i = 0; i < 12; i++) out->net.w[i] = o.net.w[i];
        out->net.b = o.net.b; out->net.n = o.net.n; out->net.pos = o.net.pos;
        out->net.tp = o.net.tp; out->net.fp = o.net.fp; out->net.fn = o.net.fn;
        out->net.tn = o.net.tn; out->net.loss_x1e4 = o.net.loss_x1e4;
        mnet_init(&out->memory_net);
        out->forecasts = o.forecasts;
        mind_model_seal(out);
        return 1;
    }
    if (len == sizeof(mind_model_v1_t)) {                     /* опыт прежней версии: нормы и настройки сохраняются */
        mind_model_v1_t o;
        mcopy(&o, buf, sizeof o);
        if (o.magic != MIND_MAGIC || o.version != 1) return 0;
        uint32_t c = o.crc; o.crc = 0;
        if (crc32_m(&o, sizeof o) != c) return 0;
        mzero(out, sizeof *out);
        mcopy(out, &o, __builtin_offsetof(mind_model_v1_t, crc));
        mnet_init(&out->net);
        mnet_init(&out->memory_net);
        mind_model_seal(out);
        return 1;
    }
    return 0;
}

/* Семя: только веса предвидения (с урезанным числом наблюдений) и подсказки по рычагам. */
int mind_model_seed(mind_model_t* out, const void* buf, unsigned len) {
    mind_model_t src;
    if (!mind_model_load(&src, buf, len)) return 0;
    if (src.net.n < 60) return 0;                              /* семя из пары минут жизни - не знание */
    mzero(out, sizeof *out);
    out->mode = MIND_ACT; out->fx_user = MIND_FX_AUTO; out->tz_min = 180;
    mnet_init(&out->net);
    mnet_init(&out->memory_net); /* память зависит от конкретной машины */
    for (int i = 0; i < MNET_F; i++) out->net.w[i] = src.net.w[i];
    out->net.b = src.net.b;
    out->net.n = src.net.n < MIND_SEED_N ? src.net.n : MIND_SEED_N;
    out->net.pos = (uint32_t)((uint64_t)src.net.pos * out->net.n / src.net.n);
    for (int i = 0; i < ML_N; i++) {
        if (src.lv[i].wins == 0 || src.lv[i].best >= 8) continue;      /* подсказка только от выигранной пробы */
        out->lv[i].best = src.lv[i].best;
        out->lv[i].trials = src.lv[i].trials < 2 ? src.lv[i].trials : 2;
        out->lv[i].wins = 1;
    }
    out->mflags = MF_SEED;
    mind_model_seal(out);
    return 1;
}

/* ---------------- журнал ---------------- */

static void jlog(mind_t* s, uint32_t now, int kind, int applied, uint32_t pid, const char* f, ...) {
    mind_event_t* e = &s->j[s->jn % MIND_JOURNAL];
    e->t = now; e->kind = (uint8_t)kind; e->applied = (uint8_t)applied; e->pid = pid;
    va_list ap; va_start(ap, f);
    vfmt(e->text, MIND_TEXT, f, ap);
    va_end(ap);
    s->jn++;
    s->activity++;
    if (applied) s->m.decisions++;
}

const mind_event_t* mind_journal(const mind_t* s, unsigned i) {
    if (i >= s->jn || i >= MIND_JOURNAL) return 0;
    return &s->j[(s->jn - 1 - i) % MIND_JOURNAL];
}

/* ---------------- создание ---------------- */

static void ml_init(mind_t* s);

void mind_init(mind_t* s, const mind_model_t* saved, unsigned cores) {
    mzero(s, sizeof *s);
    if (saved && mind_model_valid(saved)) s->m = *saved;
    else {
        s->m.mode = MIND_ACT;
        s->m.fx_user = MIND_FX_AUTO;
        s->m.tz_min = 180;
        mnet_init(&s->m.net);
        mnet_init(&s->m.memory_net);
    }
    if (s->m.mode > MIND_ACT) s->m.mode = MIND_ACT;
    if (s->m.fx_user > MIND_FX_AUTO) s->m.fx_user = MIND_FX_AUTO;
    s->q_idx = 2;                                          /* 40 мс */
    uint8_t best = s->m.q_best[bucket(cores)];
    for (int i = 0; i < MIND_NQ; i++) if (Qv[i] == best) s->q_idx = i;
    s->q_dir = 1;
    s->t_cool = 20;                                        /* первые секунды: только наблюдение */
    s->fx = s->m.fx_user == MIND_FX_AUTO ? MIND_FX_HIGH : s->m.fx_user;
    s->health = 100; s->c_resp = s->c_room = s->c_bal = s->c_stab = 100;
    s->mood = "просыпается";
    s->q_base_idx = -1;
    ml_init(s);
    int learned = 0;
    for (int i = 0; i < MIND_SIG_N; i++) if (s->m.norm[i].n >= 60) learned++;
    if (saved && mind_model_valid(saved) && (saved->mflags & MF_SEED))
        jlog(s, 0, MJ_LEARN, 0, 0, "нет своего опыта: взято семя (из QEMU, на железе лишь подсказка), веса на %u с",
             s->m.net.n);
    else if (saved && mind_model_valid(saved))
        jlog(s, 0, MJ_LEARN, 0, 0, "опыт загружен: %u из %u норм выучены, квант %u мс, решений за жизнь %u, модель предвидения обучена на %u с",
             (uint32_t)learned, (uint32_t)MIND_SIG_N, (uint32_t)Qv[s->q_idx] * 10, s->m.decisions, s->m.net.n);
    else
        jlog(s, 0, MJ_LEARN, 0, 0, "первая жизнь: учусь, что для этой машины нормально");
}

unsigned mind_quantum_ms(const mind_t* s) { return (unsigned)Qv[s->q_idx] * 10; }
int mind_fx_level(const mind_t* s) { return s->fx; }

void mind_set_mode(mind_t* s, int mode, const mind_act_t* act) {
    mode = mode ? MIND_ACT : MIND_OBSERVE;
    if (s->m.mode == mode) return;
    s->m.mode = (uint8_t)mode;
    if (mode == MIND_OBSERVE) {                            /* всё, что разум изменил, возвращается к обычному */
        for (int i = 0; i < MIND_PROC_MAX; i++) {
            mind_track_t* t = &s->tr[i];
            if (!t->alive) continue;
            if (t->weight != 4 && act->set_weight) act->set_weight(t->pid, 4);
            if (t->limit != 100 && act->set_limit) act->set_limit(t->pid, 100);
            t->weight = 4; t->limit = 100; t->flagged = 0; t->throttled_bg = 0;
        }
        if (s->t_state == 2 && s->q_base_idx >= 0) { s->q_idx = s->q_base_idx; if (act->set_quantum) act->set_quantum(Qv[s->q_idx]); }
        s->t_state = 0;
        if (s->mem_level >= 2 && act->heap_restrict) act->heap_restrict(0);
        s->mem_level = 0;
        s->focus_pid = 0;
        if (s->bg_fps_level && act->set_bg_fps) act->set_bg_fps(0);
        s->bg_fps_level = 0;
        if (s->core_res_pid && act->reserve_core) act->reserve_core(0, 0);
        s->core_res_pid = 0;
        for (int i = 0; i < MIND_PROC_MAX; i++) s->tr[i].tact_limit = 0;
        s->tact_on = 0;
        mind_levers_reset(s, act);
    }
    jlog(s, 0, MJ_INFO, 0, 0, mode ? "режим: управление (решения применяются)" : "режим: наблюдение (решения только записываются)");
}

void mind_set_fx_user(mind_t* s, int fx, const mind_act_t* act) {
    if (fx < 0 || fx > MIND_FX_AUTO) return;
    s->m.fx_user = (uint8_t)fx;
    s->fx = fx == MIND_FX_AUTO ? MIND_FX_HIGH : fx;
    s->fx_bad_s = s->fx_calm_s = 0;
    if (act->set_fx) act->set_fx(s->fx);
}

/* ---------------- нормы ---------------- */

static uint8_t to_pct(int sig, const mind_sample_t* in, int64_t x100) {
    uint64_t v;
    switch (sig) {
    case MS_LOAD: case MS_MEM: v = (uint64_t)(x100 / 100); break;
    case MS_QUEUE: v = (uint64_t)(x100 / 4); break;                                       /* 4 потока на ядро = 100 */
    case MS_LATE: v = in->loops ? (uint64_t)in->late * 100 / in->loops : 0; break;        /* доля опоздавших */
    case MS_DISK: v = (uint64_t)(x100 / 10000); break;                                    /* 10 мс = 100 */
    case MS_FAULT: v = (uint64_t)(x100 / 2); break;
    default: v = (uint64_t)ilog2u((uint64_t)(x100 / 100) + 1) * 6; break;                 /* частоты - в логарифме */
    }
    return (uint8_t)(v > 100 ? 100 : v);
}

static void learn(mind_t* s, const mind_sample_t* in, uint32_t now) {
    int64_t x[MIND_SIG_N];
    uint64_t sysc = 0;
    for (unsigned i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive) sysc += s->tr[i].sysc_rate;
    unsigned cores = in->cores ? in->cores : 1;
    x[MS_LOAD]  = (int64_t)in->load * 100;
    x[MS_QUEUE] = (int64_t)in->runnable * 100 / cores;
    x[MS_MEM]   = (int64_t)in->mem_used * 100;
    x[MS_LATE]  = (int64_t)in->late * 100;
    x[MS_DISK]  = (int64_t)in->disk_lat_us * 100;
    x[MS_IPC]   = (int64_t)in->ipc * 100;
    x[MS_FAULT] = (int64_t)in->faults * 100;
    x[MS_SYSC]  = (int64_t)sysc * 100;
    for (int k = 0; k < MIND_SIG_N; k++) {
        mind_norm_t* nm = &s->m.norm[k];
        if (k == MS_DISK && in->disk_ops == 0) {              /* диск не трогали: замера нет */
            s->z_x10[k] = 0; s->hot[k] = 0;
            s->hist[k][s->hpos % MIND_HIST] = s->hist[k][(s->hpos + MIND_HIST - 1) % MIND_HIST];
            continue;
        }
        s->cur_x100[k] = (int32_t)(x[k] > 0x7FFFFFFF ? 0x7FFFFFFF : x[k]);
        s->hist[k][s->hpos % MIND_HIST] = to_pct(k, in, x[k]);
        if (nm->n == 0) { nm->mean_x100 = x[k]; nm->var_x100 = (uint64_t)(sd_floor[k] * sd_floor[k]); }
        int64_t d = x[k] - nm->mean_x100;
        int64_t sd = (int64_t)isqrt64(nm->var_x100);
        if (sd < sd_floor[k]) sd = sd_floor[k];
        int64_t z = d * 10 / sd;
        s->z_x10[k] = (int32_t)(z > 999 ? 999 : z < -999 ? -999 : z);
        /* Необычное выучивается медленно: один выброс не должен сдвинуть норму. */
        int64_t rate = z >= 40 ? 1024 : 128;
        nm->mean_x100 += d / rate;
        uint64_t dd = (uint64_t)(d < 0 ? -d : d);
        dd = dd > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : dd;
        uint64_t sq = dd * dd;
        if (sq >= nm->var_x100) nm->var_x100 += (sq - nm->var_x100) / (uint64_t)rate;
        else nm->var_x100 -= (nm->var_x100 - sq) / (uint64_t)rate;
        if (nm->n < 0xFFFFFFFFu) nm->n++;

        if (nm->n >= 60 && z >= 40) {
            if (s->hot[k] < 255) s->hot[k]++;
        } else s->hot[k] = 0;
        if (s->hot[k] == 3 && (now - s->last_anom_t[k] >= 30 || s->last_anom_t[k] == 0)) {
            s->last_anom_t[k] = now;
            s->m.anomalies++;
            s->stab_debt += 5;
            int32_t cur = (int32_t)(x[k] / 100), mean = (int32_t)(nm->mean_x100 / 100);
            jlog(s, now, MJ_ANOMALY, 0, 0, "необычно: %s %d %s при норме %d (отклонение в %u раз больше обычного разброса)",
                 sig_name[k], cur, sig_unit[k], mean, (uint32_t)(z / 10));
        }
    }
}

/* ---------------- здоровье ---------------- */

static uint8_t clampu(int64_t v) { return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }

static void assess(mind_t* s, const mind_sample_t* in, uint32_t now) {
    unsigned cores = in->cores ? in->cores : 1;
    uint32_t late_pct = in->loops ? in->late * 100 / in->loops : 0;
    int64_t resp = 100 - (int64_t)late_pct * 3;
    if (in->max_wait_ms >= 2000) resp -= 30;
    else if (in->max_wait_ms >= 500) resp -= 10;
    int64_t room = in->mem_used <= 70 ? 100 : 100 - (int64_t)(in->mem_used - 70) * 100 / 27;
    int64_t bal = 100;
    if (in->runnable > cores) { int64_t o = (int64_t)(in->runnable - cores) * 100 / (cores * 3); bal = 100 - (o > 70 ? 70 : o); }
    /* Перекос ядер. Порог перегрузки у каждого ядра свой: выше его же обычной занятости на 3 разброса, но не ниже 85%. Перекос - когда какое-то ядро выше
       своего порога, другие простаивают (до 20%), а потоков в очереди больше, чем занятых ядер: работа есть, и её можно перенести. Три секунды подряд - событие. */
    {
        unsigned nc = cores > 256 ? 256 : cores;
        uint32_t hot = 0, cold = 0, worst = 0, worst_busy = 0;
        for (unsigned c = 0; c < nc; c++) {
            int32_t b = in->busy[c] > 100 ? 100 : in->busy[c];
            int32_t mean = s->core_mean_x100[c], var = s->core_var_x100[c];
            int32_t sd = 5; while (sd * sd * 100 < var && sd < 50) sd++;          /* корень из дисперсии, не меньше 5 */
            int32_t thr = s->core_n[c] >= 60 ? mean / 100 + 3 * sd : 85;
            thr = thr < 85 ? 85 : thr > 98 ? 98 : thr;
            s->core_thr[c] = (uint8_t)thr;
            if (b >= thr) { hot++; if ((uint32_t)b >= worst_busy) { worst_busy = (uint32_t)b; worst = c; } }
            else if (b <= 20) cold++;
            int32_t d = b * 100 - mean;                                             /* скользящая норма, шаг 1/16 */
            if (s->core_n[c] < 65535) s->core_n[c]++;
            s->core_mean_x100[c] = s->core_n[c] == 1 ? b * 100 : mean + d / 16;
            s->core_var_x100[c] = s->core_n[c] == 1 ? 0 : var + (d / 100 * (d / 100) * 100 - var) / 16;
        }
        s->skew_hot = hot; s->skew_cold = cold;
        if (hot && cold && in->runnable > nc - cold) s->skew_s++;
        else s->skew_s = s->skew_s > 1 ? s->skew_s / 2 : 0;
        if (s->skew_s >= 3) {
            s->lv.seen_skew = now; s->skew_core = worst;
            uint32_t pen = 10 * hot + 5 * cold; if (pen > 40) pen = 40;
            bal -= pen;
            if (now - s->skew_logged_t >= 60 || s->skew_logged_t == 0) {
                s->skew_logged_t = now;
                jlog(s, now, MJ_INFO, 0, 0, "перекос ядер: ядро %u занято на %u%% (его порог %u%%), простаивают %u ядер, потоков в очереди %u", worst, worst_busy, s->core_thr[worst], cold, in->runnable);
            }
        }
    }
    s->stab_debt += in->faults * 20;
    int64_t stab = 100 - (int64_t)s->stab_debt;
    s->stab_debt = s->stab_debt > 16 ? s->stab_debt * 15 / 16 : (s->stab_debt ? s->stab_debt - 1 : 0);
    s->c_resp = clampu(resp); s->c_room = clampu(room); s->c_bal = clampu(bal); s->c_stab = clampu(stab);
    int64_t h = ((int64_t)s->c_resp * 35 + s->c_room * 25 + s->c_bal * 15 + s->c_stab * 25) / 100;
    uint8_t mn = s->c_resp;
    if (s->c_room < mn) mn = s->c_room;
    if (s->c_bal < mn) mn = s->c_bal;
    if (s->c_stab < mn) mn = s->c_stab;
    if (h > mn + 40) h = mn + 40;
    uint8_t prev = s->health;
    s->health = clampu(h);
    s->health_hist[s->hpos % MIND_HIST] = s->health;
    if (s->health < 60) s->low_health_t = 30;
    else if (s->low_health_t) s->low_health_t--;
    if (s->health < 50) s->mood = "тревога";
    else if (s->health < 70) s->mood = "напряжена";
    else if (s->low_health_t && s->health > prev) s->mood = "восстанавливается";
    else if (in->load >= 60) s->mood = "сосредоточена";
    else if (now < 20) s->mood = "просыпается";
    else s->mood = "спокойна";
}

/* ---------------- процессы ---------------- */

static mind_track_t* track_of(mind_t* s, uint32_t pid) {
    for (int i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive && s->tr[i].pid == pid) return &s->tr[i];
    return 0;
}

static void apply_weight(mind_t* s, mind_track_t* t, uint32_t w, const mind_act_t* act) {
    (void)s;
    if (t->weight == w) return;
    t->weight = w;
    if (act->set_weight) act->set_weight(t->pid, w);
}

static void apply_limit(mind_t* s, mind_track_t* t, const mind_act_t* act) {
    uint32_t want = t->throttled_bg ? 50 : 100;
    if (t->flagged == 1 && (uint32_t)ML(s, ML_ODD_LIMIT) < want) want = (uint32_t)ML(s, ML_ODD_LIMIT);
    if (t->tact_limit && t->tact_limit < want) want = t->tact_limit;
    if (t->limit == want) return;
    t->limit = want;
    if (act->set_limit) act->set_limit(t->pid, want);
}

static uint32_t ewma_u(uint32_t old, uint32_t v, uint32_t runs) {
    if (runs <= 1) return v;
    return (uint32_t)(((uint64_t)old * 3 + v) / 4);
}

static void track_gone(mind_t* s, mind_track_t* t, const mind_act_t* act) {
    if (t->seen >= 3 && act->profile_put) {
        mind_profile_t p = t->prof;
        p.runs = t->has_profile ? p.runs + 1 : 1;
        p.cpu_x10 = ewma_u(p.cpu_x10, t->cpu_sum_x10 / t->seen, p.runs);
        p.sysc = ewma_u(p.sysc, t->sysc_sum / t->seen, p.runs);
        p.heap_kb = ewma_u(p.heap_kb, t->heap_peak, p.runs);
        act->profile_put(t->name, &p);
    }
    if (s->focus_pid == t->pid) s->focus_pid = 0;
    t->alive = 0;
}

static void processes(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    int acting = s->m.mode == MIND_ACT;
    for (int i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive) {
        int found = 0;
        for (unsigned k = 0; k < in->nproc && k < MIND_PROC_MAX; k++) if (in->proc[k].pid == s->tr[i].pid) found = 1;
        if (!found) track_gone(s, &s->tr[i], act);
    }
    uint32_t focus = 0;
    for (unsigned k = 0; k < in->nproc && k < MIND_PROC_MAX; k++) {
        const mind_proc_t* p = &in->proc[k];
        if (p->focus) focus = p->pid;
        mind_track_t* t = track_of(s, p->pid);
        if (!t) {
            for (int i = 0; i < MIND_PROC_MAX && !t; i++) if (!s->tr[i].alive) t = &s->tr[i];
            if (!t) continue;
            mzero(t, sizeof *t);
            t->alive = 1; t->pid = p->pid; scopy(t->name, p->name, sizeof t->name);
            t->last_sysc = p->syscalls; t->last_cyc = p->cpu_cycles;
            t->weight = 4; t->limit = 100; t->threads = p->threads;
            if (act->profile_get && act->profile_get(t->name, &t->prof) == 0) t->has_profile = 1;
            if (t->has_profile && t->prof.runs >= 4 && t->prof.crashes * 2 > t->prof.runs)
                jlog(s, now, MJ_BEHAVIOR, 0, t->pid, "%s часто падает: %u аварий из %u запусков", t->name, t->prof.crashes, t->prof.runs);
            continue;
        }
        t->threads = p->threads;
        uint64_t dc = p->cpu_cycles >= t->last_cyc ? p->cpu_cycles - t->last_cyc : 0;
        uint64_t ds = p->syscalls >= t->last_sysc ? p->syscalls - t->last_sysc : 0;
        t->last_cyc = p->cpu_cycles; t->last_sysc = p->syscalls;
        t->cpu_x10 = in->tsc_khz ? (uint32_t)(dc / in->tsc_khz) : 0;
        t->sysc_rate = (uint32_t)(ds > 0xFFFFFFFFu ? 0xFFFFFFFFu : ds);
        t->seen++;
        t->cpu_sum_x10 += t->cpu_x10;
        t->sysc_sum += t->sysc_rate;
        if (p->heap_kb > t->heap_peak) t->heap_peak = p->heap_kb;

        /* Поведение против собственного профиля программы */
        if (t->has_profile && t->prof.runs >= 3) {
            uint32_t lim = t->prof.sysc * 10 > 20000 ? t->prof.sysc * 10 : 20000;
            if (t->sysc_rate > lim) { t->odd_s++; t->calm_s = 0; }
            else { t->odd_s = 0; t->calm_s++; }
            if (t->odd_s == 5 && !t->flagged) {
                t->flagged = 1;
                jlog(s, now, MJ_BEHAVIOR, acting, t->pid, "%s%s ведёт себя необычно: %u вызовов/с при обычных %u - ограничиваю до 50%% ядра",
                     acting ? "" : "(наблюдение) ", t->name, t->sysc_rate, t->prof.sysc);
                if (acting) apply_limit(s, t, act); else t->flagged = 2;
            }
            if (t->flagged && t->calm_s >= 10) {
                int was = t->flagged == 1;
                t->flagged = 0;
                if (was) { apply_limit(s, t, act); jlog(s, now, MJ_RESTORE, 1, t->pid, "%s снова ведёт себя как обычно: ограничение снято", t->name); }
            }
            if (t->prof.heap_kb && p->heap_kb > t->prof.heap_kb * 4 && p->heap_kb > 32768 && !(t->prof.flags & 1)) {
                t->prof.flags |= 1;
                jlog(s, now, MJ_BEHAVIOR, 0, t->pid, "%s заняла %u МБ кучи при обычных %u МБ", t->name, p->heap_kb / 1024, t->prof.heap_kb / 1024);
            }
        }
    }

    /* Фокус: программа, с которой работает человек, получает больший квант. */
    if (focus != s->focus_pid) {
        mind_track_t* old = track_of(s, s->focus_pid);
        if (old && acting) apply_weight(s, old, 4, act);
        s->focus_pid = focus;
        mind_track_t* nw = track_of(s, focus);
        if (nw) {
            if (acting) apply_weight(s, nw, (uint32_t)ML(s, ML_FOCUS_W), act);
            jlog(s, now, MJ_FOCUS, acting, focus, "%sфокус: %s - вес %u/4 (квант больше обычного), пока окно впереди", acting ? "" : "(наблюдение) ", nw->name, (uint32_t)ML(s, ML_FOCUS_W));
        }
    }

    /* Экран опаздывает: тяжёлые фоновые программы ограничиваются, пока экран не успокоится. */
    uint32_t late_pct = in->loops ? in->late * 100 / in->loops : 0;
    if (late_pct >= 5) { s->late_s++; s->calm_s = 0; } else { s->late_s = 0; s->calm_s++; }
    if (s->late_s >= 2 && acting) {
        for (int i = 0; i < MIND_PROC_MAX; i++) {
            mind_track_t* t = &s->tr[i];
            if (!t->alive || t->pid == s->focus_pid || t->throttled_bg || t->cpu_x10 < 500) continue;
            t->throttled_bg = 1;
            apply_limit(s, t, act);
            jlog(s, now, MJ_LIMIT, 1, t->pid, "экран опаздывает (%u%% кадров): ограничиваю фоновую %s (%u%% ядра) до 50%%",
                 late_pct, t->name, t->cpu_x10 / 10);
        }
    }
    if (s->calm_s >= 10) {
        for (int i = 0; i < MIND_PROC_MAX; i++) {
            mind_track_t* t = &s->tr[i];
            if (!t->alive || !t->throttled_bg) continue;
            t->throttled_bg = 0;
            apply_limit(s, t, act);
            jlog(s, now, MJ_RESTORE, 1, t->pid, "экран успевает 10 с: %s снова без ограничения", t->name);
        }
    }

    /* Голодание: поток долго готов, но не получает процессор. */
    uint32_t starve_ms = (uint32_t)ML(s, ML_STARVE_MS);
    if (in->max_wait_ms >= starve_ms && now - s->starve_t >= 10) {
        uint32_t worst = 0; mind_track_t* wt = 0;
        for (unsigned k = 0; k < in->nproc && k < MIND_PROC_MAX; k++)
            if (in->proc[k].wait_ms > worst) { worst = in->proc[k].wait_ms; wt = track_of(s, in->proc[k].pid); }
        if (wt && worst >= starve_ms) {
            s->starve_t = now;
            if (acting) apply_weight(s, wt, 8, act);
            jlog(s, now, MJ_STARVE, acting, wt->pid, "%s%s ждал процессор %u мс: вес повышен до 2x", acting ? "" : "(наблюдение) ", wt->name, worst);
        } else if (in->kwait_ms >= starve_ms) {
            s->starve_t = now;
            jlog(s, now, MJ_STARVE, 0, 0, "поток ядра %s ждал процессор %u мс", in->kwait_name, in->kwait_ms);
        }
    } else if (now - s->starve_t == 10) {                   /* через 10 с повышенный вес возвращается */
        for (int i = 0; i < MIND_PROC_MAX; i++) {
            mind_track_t* t = &s->tr[i];
            if (t->alive && t->weight == 8 && acting) apply_weight(s, t, t->pid == s->focus_pid ? (uint32_t)ML(s, ML_FOCUS_W) : 4, act);
        }
    }
}

/* ---------------- самонастройка кванта ---------------- */

static uint64_t tune_score(const mind_sample_t* in) {
    unsigned cores = in->cores ? in->cores : 1;
    /* Короткий квант - больше переключений (накладные расходы), длинный - дольше ждут готовые потоки. */
    return (uint64_t)in->switches * 2 / cores + (uint64_t)in->max_wait_ms * 20 + (uint64_t)in->late * 200;
}

static void tuner(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    if (s->m.mode != MIND_ACT) return;
    unsigned cores = in->cores ? in->cores : 1;
    int contention = in->runnable > cores;
    if (s->t_state == 0) {
        if (s->t_cool) { s->t_cool--; return; }
        if (!contention) return;
        s->t_state = 1; s->t_n = 0; s->t_score = 0; s->t_run_sum = 0;
    }
    if (!contention) {
        /* Нагрузка ушла посреди пробы: честно сравнить не с чем - возврат к прежнему кванту. */
        if (s->t_state == 2 && ++s->t_idle >= 30) {
            jlog(s, now, MJ_TUNE_BACK, 1, 0, "нагрузка ушла во время пробы кванта %u мс - возвращаю %u мс",
                 (uint32_t)Qv[s->q_idx] * 10, (uint32_t)Qv[s->q_base_idx] * 10);
            s->q_idx = s->q_base_idx;
            if (act->set_quantum) act->set_quantum(Qv[s->q_idx]);
            s->q_base_idx = -1; s->t_state = 0; s->t_cool = 60; s->t_idle = 0;
        }
        return;
    }
    s->t_idle = 0;
    s->t_score += tune_score(in);
    s->t_run_sum += in->runnable;
    if (++s->t_n < 8) return;
    uint64_t avg = s->t_score / s->t_n;
    uint32_t run = s->t_run_sum / s->t_n;
    if (s->t_state == 1) {
        int cand = s->q_idx + s->q_dir;
        if (cand < 0 || cand >= MIND_NQ) { s->q_dir = -s->q_dir; cand = s->q_idx + s->q_dir; }
        s->t_base_score = avg; s->t_base_run = run; s->q_base_idx = s->q_idx;
        s->q_idx = cand;
        if (act->set_quantum) act->set_quantum(Qv[cand]);
        s->m.tune_trials++;
        jlog(s, now, MJ_TUNE_TRY, 1, 0, "пробую квант %u мс вместо %u мс: сравню переключения и ожидание потоков",
             (uint32_t)Qv[cand] * 10, (uint32_t)Qv[s->q_base_idx] * 10);
        s->t_state = 2; s->t_n = 0; s->t_score = 0; s->t_run_sum = 0;
        return;
    }
    /* t_state == 2: проба окончена */
    int fair = run * 10 >= s->t_base_run * 7 && run * 10 <= s->t_base_run * 14;
    unsigned b = bucket(cores);
    if (fair && avg * 100 < s->t_base_score * 95) {
        uint32_t gain = s->t_base_score ? (uint32_t)((s->t_base_score - avg) * 100 / s->t_base_score) : 0;
        s->m.q_best[b] = Qv[s->q_idx];
        s->m.tune_wins++;
        s->t_fails = 0;
        jlog(s, now, MJ_TUNE_KEEP, 1, 0, "квант %u мс лучше прежнего %u мс на %u%% - оставляю и запоминаю для %u ядер",
             (uint32_t)Qv[s->q_idx] * 10, (uint32_t)Qv[s->q_base_idx] * 10, gain, (uint32_t)cores);
        s->t_cool = 30;
    } else {
        int back = s->q_base_idx;
        jlog(s, now, MJ_TUNE_BACK, 1, 0, fair ? "квант %u мс не лучше (%u против %u) - возвращаю %u мс"
                                             : "нагрузка за время пробы изменилась (кванта %u мс, оценки %u и %u) - сравнение нечестное, возвращаю %u мс",
             (uint32_t)Qv[s->q_idx] * 10, (uint32_t)avg, (uint32_t)s->t_base_score, (uint32_t)Qv[back] * 10);
        s->q_idx = back;
        if (act->set_quantum) act->set_quantum(Qv[back]);
        s->q_dir = -s->q_dir;
        s->t_fails++;
        s->t_cool = s->t_fails >= 2 ? 300 : 60;
        if (s->t_fails >= 2) s->t_fails = 0;
        if (!s->m.q_best[b]) s->m.q_best[b] = Qv[back];
    }
    s->q_base_idx = -1;
    s->t_state = 0;
}

/* ---------------- анимация и память ---------------- */

static void visual(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    if (s->m.fx_user != MIND_FX_AUTO || s->m.mode != MIND_ACT) return;
    uint32_t late_pct = in->loops ? in->late * 100 / in->loops : 0;
    if (late_pct >= 10) s->fx_bad_s++; else s->fx_bad_s = 0;
    if (in->late == 0 && in->load < 70) s->fx_calm_s++; else s->fx_calm_s = 0;
    if (s->fx_bad_s >= (uint32_t)ML(s, ML_FX_BAD) && s->fx > MIND_FX_OFF) {
        s->fx--; s->fx_bad_s = 0;
        if (act->set_fx) act->set_fx(s->fx);
        jlog(s, now, MJ_FX, 1, 0, "экран не успевает (%u%% кадров опоздали): анимация фона - уровень %u", late_pct, (uint32_t)s->fx);
    } else if (s->fx_calm_s >= (uint32_t)ML(s, ML_FX_CALM) && s->fx < MIND_FX_HIGH) {
        s->fx++; s->fx_calm_s = 0;
        if (act->set_fx) act->set_fx(s->fx);
        jlog(s, now, MJ_FX, 1, 0, "у экрана есть запас времени: анимация фона - уровень %u", (uint32_t)s->fx);
    }
}

static void memory(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    int acting = s->m.mode == MIND_ACT;
    uint32_t u = in->mem_used;
    if (u >= 88 && s->mem_level < 1) {
        s->mem_level = 1;
        unsigned pages = acting && act->mem_trim ? act->mem_trim() : 0;
        jlog(s, now, MJ_MEMORY, acting, 0, "%sпамять занята на %u%%: вернула запасы ядра (%u страниц)", acting ? "" : "(наблюдение) ", u, (uint32_t)pages);
    }
    if (u >= 93 && s->mem_level < 2) {
        s->mem_level = 2;
        if (acting && act->heap_restrict) act->heap_restrict(2);
        jlog(s, now, MJ_MEMORY, acting, 0, "%sпамять занята на %u%%: программам - только осторожный рост куч", acting ? "" : "(наблюдение) ", u);
    }
    if (u >= 97 && acting && now - s->last_kill_t >= 10) {
        mind_track_t* big = 0; uint32_t heap = 0;
        for (unsigned k = 0; k < in->nproc && k < MIND_PROC_MAX; k++)
            if (in->proc[k].heap_kb > heap) { heap = in->proc[k].heap_kb; big = track_of(s, in->proc[k].pid); }
        if (big && heap >= 16384 && (uint64_t)heap * 100 >= (uint64_t)in->mem_total_mb * 1024 * 20) {   /* не меньше 20% ОЗУ */
            s->last_kill_t = now;
            if (act->kill && act->kill(big->pid) == 0) {
                s->m.kills++;
                jlog(s, now, MJ_KILL, 1, big->pid, "память исчерпана (%u%%): остановлена %s, занимавшая %u МБ кучи - система жива",
                     u, big->name, heap / 1024);
            }
        }
    }
    if (u < 80 && s->mem_level) {
        if (s->mem_level >= 2 && acting && act->heap_restrict) act->heap_restrict(0);
        s->mem_level = 0;
        jlog(s, now, MJ_RESTORE, acting, 0, "давление памяти ушло (%u%%): обычный режим", u);
    }
}

/* ---------------- шаг ---------------- */

/* ---------------- устройства и иммунитет ввода ---------------- */

static int same_name(const char* a, const char* b) { int i = 0; while (i < 16 && a[i] == b[i] && a[i]) i++; return i == 16 || a[i] == b[i]; }

unsigned mind_core_cap(const mind_t* s) { return s->m.core_cap ? s->m.core_cap : 50; }

/* Ступени темпа фоновых окон: экран опаздывает - каждые 200 мс ступень ниже; спокойно 1 с - каждые 0,5 с выше. */
static const uint8_t bg_fps_steps[] = { 0, 30, 15, 8, 4 };
#define BG_STEPS (sizeof bg_fps_steps / sizeof bg_fps_steps[0])
unsigned mind_bg_fps(const mind_t* s) { return bg_fps_steps[s->bg_fps_level]; }

static void bg_fps_step(mind_t* s, const mind_tact_in_t* in, const mind_act_t* act, int pressure, int acting) {
    uint32_t lvl = s->bg_fps_level;
    s->bg_calm_ms = pressure ? 0 : s->bg_calm_ms + in->window_ms;
    if (pressure && in->late && lvl + 1 < BG_STEPS && in->t_ms - s->bg_fps_ms >= (uint32_t)ML(s, ML_BG_STEP)) lvl++;
    else if (!pressure && lvl && s->bg_calm_ms >= (uint32_t)ML(s, ML_BG_CALM) && in->t_ms - s->bg_fps_ms >= 500) lvl--;
    if (lvl == s->bg_fps_level) return;
    int down = lvl > s->bg_fps_level;
    s->bg_fps_level = lvl; s->bg_fps_ms = in->t_ms;
    if (acting && act->set_bg_fps) act->set_bg_fps(bg_fps_steps[lvl]);
    if (lvl == 0) jlog(s, in->t_ms / 1000, MJ_RESTORE, acting, 0, "тактика: экран успевает - фоновые окна снова без предела кадров");
    else if (down) jlog(s, in->t_ms / 1000, MJ_LIMIT, acting, 0, "%sтактика: экран опаздывает - фоновые окна не чаще %u кадров/с (их копия и сборка - работа экрана)",
                        acting ? "" : "(наблюдение) ", (uint32_t)bg_fps_steps[lvl]);
}

/* Рычаг «ядро фокуса»: экран не успевает при 3+ ядрах и фоновых программах - одно ядро отдаётся окну в фокусе,
   фон работает на остальных (его потоки не встают в очередь к фокусу и не вытесняются рефлексом туда-обратно).
   Возврат - после 3 с спокойствия, при смене режима на наблюдение, без фокуса или фона. Пока ядро отдано, вызов
   повторяется каждый такт: новые потоки фоновых программ тоже уходят с него. */
static void core_res_step(mind_t* s, const mind_tact_in_t* in, const mind_act_t* act, int pressure, int acting,
                          uint32_t cores, uint32_t nbg) {
    s->core_calm_ms = pressure ? 0 : s->core_calm_ms + in->window_ms;
    uint32_t pid = s->core_res_pid;
    if (acting && pressure && in->late >= (uint32_t)ML(s, ML_CORE_RES) && cores >= 3 && s->focus_pid && nbg) pid = s->focus_pid;
    else if (!acting || !s->focus_pid || !nbg || s->core_calm_ms >= 3000) pid = 0;
    if (!act->reserve_core) { s->core_res_pid = 0; return; }
    if (pid == s->core_res_pid) { if (pid) act->reserve_core(1, pid); return; }
    act->reserve_core(pid != 0, pid);
    if (pid) jlog(s, in->t_ms / 1000, MJ_LIMIT, 1, pid, "тактика: экран опаздывает - ядро отдано окну в фокусе (pid %u), фоновые программы - на остальных %u",
                  pid, cores - 1);
    else jlog(s, in->t_ms / 1000, MJ_RESTORE, 1, 0, "тактика: экран успевает - ядро фокуса снова общее");
    s->core_res_pid = pid;
}

uint32_t mind_core_reserved_pid(const mind_t* s) { return s->core_res_pid; }

void mind_tact_step(mind_t* s, const mind_tact_in_t* in, const mind_act_t* act) {
    s->tact_steps++;
    int acting = s->m.mode == MIND_ACT;
    uint32_t cores = in->cores ? in->cores : 1;
    for (unsigned k = 0; k < in->nproc && k < MIND_TACT_MAXP; k++) {
        mind_track_t* t = track_of(s, in->p[k].pid);
        if (t) { t->tact_cpu_x10 = in->p[k].cpu_x10; if (in->p[k].threads) t->threads = in->p[k].threads; }
    }
    uint32_t bg = 0, nbg = 0;
    for (int i = 0; i < MIND_PROC_MAX; i++) {
        mind_track_t* t = &s->tr[i];
        if (!t->alive || t->pid == s->focus_pid) continue;
        bg += t->tact_cpu_x10; nbg++;
    }
    uint32_t fg = in->busy_x10 > bg ? in->busy_x10 - bg : 0;
    int foresee = in->t_ms < s->pre_until_ms;                     /* стратегия предвидит опоздание */
    int pressure = (in->late > 0) || in->fg_wait_ms > 20 || foresee;
    /* Бюджет фона: то, что не нужно переднему плану (экран, ядро, окно в фокусе), за вычетом запаса в 10% ядер
       (не меньше пятой части ядра) - и не больше предела core_cap от всех ядер. Раньше предел считался от «всего
       минус передний план»: на 4 ядрах при занятом экране фону оставалось 1-2% ядер, а три ядра стояли пустыми. */
    uint32_t total = cores * 1000, cap_x10 = mind_core_cap(s) * 10 * cores;
    uint32_t reserve = total / 10 > 200 ? total / 10 : 200;
    uint32_t spare = total > fg + reserve ? total - fg - reserve : 0;
    uint32_t budget = cap_x10 < spare ? cap_x10 : spare;
    if (budget < 50 * nbg) budget = 50 * nbg;                     /* фон не останавливается совсем: от 5% каждому */
    /* Ядра простаивают (свободно больше целого ядра), а экран всё равно опаздывает - дело не в процессоре: ужимать
       фон бесполезно, работает темп фоновых окон (их кадры - работа экрана). */
    int cpu_short = in->busy_x10 + 1000 > total;
    if (pressure && cpu_short && nbg && bg > budget) {
        /* Давление вернулось вскоре после шага снятия предела: предел был снят зря. Выдержка перед следующим снятием
           удлиняется (не больше чем в 4 раза), иначе предел качается вверх-вниз каждые 1-2 с, и каждый подъём даёт опоздавшие кадры. */
        if (s->tact_on && s->tact_rel_ms && in->t_ms - s->tact_rel_ms <= 3000) s->stab_relapses++;
        if (s->tact_on && s->tact_rel_ms && in->t_ms - s->tact_rel_ms <= 3000 && s->tact_backoff < MIND_REL_BACKOFF_MAX) {
            s->tact_backoff++;
            s->tact_back_ms = in->t_ms;
        }
        s->tact_rel_ms = 0;
        s->tact_calm_ms = 0;
        s->tact_budget_x10 = budget;
        for (int i = 0; i < MIND_PROC_MAX; i++) {
            mind_track_t* t = &s->tr[i];
            if (!t->alive || t->pid == s->focus_pid || t->tact_cpu_x10 < 20) continue;
            uint32_t share = (uint32_t)((uint64_t)t->tact_cpu_x10 * budget / bg);       /* % x10 процесса */
            uint32_t thr = t->threads ? t->threads : 1;
            uint32_t lim = share / 10 / thr;
            if (lim < 5) lim = 5;
            if (lim >= 100) continue;
            if (t->tact_limit && t->tact_limit <= lim) continue;           /* уже не мягче нужного */
            t->tact_limit = lim;
            if (acting) apply_limit(s, t, act);
            s->tact_actions++;
        }
        if (!s->tact_on || in->t_ms - s->tact_logged_ms >= 2000) {
            jlog(s, in->t_ms / 1000, MJ_LIMIT, acting, 0, "%sтактика: %s - фон ужат до %u%% ядер (предел %u%%)",
                 acting ? "" : "(наблюдение) ", in->late ? "экран опаздывает" : in->fg_wait_ms > 20 ? "система ждёт процессор" : "упреждение",
                 budget / 10 / cores, mind_core_cap(s));
            s->tact_logged_ms = in->t_ms;
        }
        if (!s->tact_on) { s->stab_episodes++; s->stab_cur_ms = 0; }
        s->tact_on = 1;
    } else if (s->tact_on) {
        s->tact_calm_ms += in->window_ms;
        if (!pressure && s->tact_calm_ms >= (uint32_t)ML(s, ML_RELEASE) << s->tact_backoff) {      /* спокойно: отпускать по четверти */
            int any = 0;
            if (s->tact_backoff && in->t_ms - s->tact_back_ms >= 10000) { s->tact_backoff--; s->tact_back_ms = in->t_ms; }   /* 10 с без возврата давления: выдержка короче */
            s->tact_rel_ms = in->t_ms;
            for (int i = 0; i < MIND_PROC_MAX; i++) {
                mind_track_t* t = &s->tr[i];
                if (!t->alive || !t->tact_limit) continue;
                uint32_t nl = t->tact_limit * 5 / 4 + 1;
                t->tact_limit = nl >= 100 ? 0 : nl;
                if (acting) apply_limit(s, t, act);
                any |= t->tact_limit != 0;
            }
            s->tact_calm_ms = 200;                                         /* следующий шаг - через ~0,2 с */
            if (!any) { s->tact_on = 0; s->tact_backoff = 0; s->tact_rel_ms = 0; jlog(s, in->t_ms / 1000, MJ_RESTORE, acting, 0, "тактика: давления нет - фон снова без пределов"); }
        }
    }
    if (s->tact_on) {                                              /* учёт времени с пределами */
        uint32_t dt = s->tact_last_ms && in->t_ms > s->tact_last_ms ? in->t_ms - s->tact_last_ms : in->window_ms;
        s->stab_limited_ms += dt; s->stab_cur_ms += dt;
        if (s->stab_cur_ms > s->stab_longest_ms) s->stab_longest_ms = s->stab_cur_ms;
    }
    bg_fps_step(s, in, act, pressure, acting);
    core_res_step(s, in, act, pressure, acting, cores, nbg);
    s->tact_last_ms = in->t_ms;
}

void mind_launch_limits(const mind_t* s, unsigned cores, mind_launch_t* out) {
    if (cores < 1) cores = 1;
    uint32_t same = cores * 2;
    if (s->health && s->health < 60) same = cores;
    if (s->health && s->health < 40) same = cores / 2;
    uint32_t pct = (uint32_t)ML(s, ML_ADMIT);            /* рычаг: допуск запусков, % от обычного */
    same = same * pct / 100;
    if (same < 2) same = 2;
    if (same > 64) same = 64;
    out->max_same = same;
    out->rate_same = cores < 3 ? 3 : cores;             /* burnall запускает по копии на ядро разом */
    out->rate_total = 2 * out->rate_same + 4;
    if (pct > 100) { out->rate_same = out->rate_same * pct / 100; out->rate_total = out->rate_total * pct / 100; }
    if (s->health && s->health < 40) { out->rate_same = (out->rate_same + 1) / 2; out->rate_total = (out->rate_total + 1) / 2; }
}

static void devices(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    if (in->dev_errors) {
        s->stab_debt += in->dev_errors > 10 ? 30 : in->dev_errors * 3;     /* сбой железа - удар по стабильности */
        s->dev_err_s++;
        int fresh = !same_name(s->dev_last, in->dev_name) || now - s->dev_last_t >= 30;
        if (fresh) {
            jlog(s, now, MJ_ANOMALY, 0, 0, "устройство %s: ошибок обмена %u за секунду", in->dev_name, in->dev_errors);
            s->dev_last_t = now;
            for (int i = 0; i < 16; i++) s->dev_last[i] = in->dev_name[i];
        }
        if (s->dev_err_s >= 10 && !s->dev_warned) {                       /* не случайность: устройство сдаёт */
            jlog(s, now, MJ_ANOMALY, 0, 0, "%s ошибается 10 с подряд: сохраните данные, проверьте кабель", in->dev_name);
            s->dev_warned = 1;
        }
    } else if (s->dev_err_s) {
        s->dev_err_s = 0;
        s->dev_warned = 0;
    }
    if (in->denied) {
        jlog(s, now, MJ_LIMIT, 1, 0, "не допущено запусков: %u (последний: %s) - больше, чем выдержит система",
             in->denied, in->denied_name);
        s->denied_launches = in->denied;
    }
    if (in->hid_suspect) {
        int block = s->m.mode == MIND_ACT;
        if (act->hid_verdict) act->hid_verdict(block);
        if (block) {
            s->hid_blocked++;
            jlog(s, now, MJ_LIMIT, 1, 0, "ввод %s: %u нажатий/с, быстрее человека - отключён (BadUSB?). Сканер штрихкодов? usb allow",
                 in->hid_name, in->hid_rate);
        } else
            jlog(s, now, MJ_ANOMALY, 0, 0, "ввод %s: %u нажатий/с, быстрее человека (наблюдение: ввод пропущен)", in->hid_name, in->hid_rate);
        s->stab_debt += 10;
    }
}

/* ---------------- предвидение (стратегия, раз в секунду) ----------------
   Модель (mindnet) по состоянию системы сейчас оценивает вероятность того, что в следующую секунду экран будет
   опаздывать. Через секунду исход известен - модель учится на нём. Если модели можно доверять (её предупреждения
   сбывались) и риск высок, тактика ужимает фон заранее, до первого опоздавшего кадра, а не после.
   Секунды, когда упреждение действовало, не учат модель: опоздания не было, но, возможно, только благодаря ему.
   Поэтому каждая десятая опасная секунда проходит без упреждения - модель видит честный исход и не застывает. */
static const char* const feat_name[MNET_F] = {
    "загрузка ядер", "очередь потоков", "память", "фоновые программы", "программа в фокусе", "обмен сообщениями",
    "задержка диска", "переключения потоков", "число программ", "опоздания сейчас", "ожидание процессора", "анимация", "валидность замера диска"
};
const char* mind_feature_name(int f) { return f >= 0 && f < MNET_F ? feat_name[f] : "?"; }

static int32_t q16(uint64_t num, uint64_t den) {
    if (!den) return 0;
    uint64_t v = num * MNET_ONE / den;
    return (int32_t)(v > MNET_ONE ? MNET_ONE : v);
}

void mind_make_features(const mind_t* s, const mind_sample_t* in, int32_t* x) {
    unsigned cores = in->cores ? in->cores : 1;
    uint64_t bg = 0, fg = 0;
    for (int i = 0; i < MIND_PROC_MAX; i++) {
        const mind_track_t* t = &s->tr[i];
        if (!t->alive) continue;
        if (t->pid == s->focus_pid) fg += t->cpu_x10; else bg += t->cpu_x10;
    }
    x[0] = q16(in->load, 100);
    x[1] = q16(in->runnable, (uint64_t)cores * 4);
    x[2] = q16(in->mem_used, 100);
    x[3] = q16(bg, (uint64_t)cores * 1000);
    x[4] = q16(fg, 1000);
    x[5] = q16(ilog2u((uint64_t)in->ipc + 1), 16);
    x[6] = q16(ilog2u((uint64_t)in->disk_lat_us + 1), 20);
    x[7] = q16(ilog2u((uint64_t)in->switches / cores + 1), 16);
    x[8] = q16(in->nproc, (uint64_t)cores * 4);
    x[9] = q16(in->late, in->loops ? in->loops : 1);
    x[10] = q16(ilog2u((uint64_t)in->max_wait_ms + 1), 12);
    x[11] = q16((uint64_t)(s->fx < 0 ? 0 : s->fx), 3);
    x[12] = in->disk_ops ? MNET_ONE : 0;       /* нулевая задержка без операций не означает валидный замер */
}

static void memory_foresight(mind_t* s, const mind_sample_t* in, const int32_t* x, uint32_t now) {
    if (!in->mem_total_mb || in->mem_used > 100) {
        for (unsigned i = 0; i < 5; i++) s->mem_pending[i].valid = 0;
        s->mem_prev_valid = 0;
        s->mem_prev_t = 0;
        s->mem_risk_q16 = 0;
        return;
    }
    /* Горизонт равен четырём реальным секундам: пропущенный/повторный timestamp
       обрывает непрерывность и цензурирует все незавершённые метки. */
    if (s->mem_prev_valid && (now <= s->mem_prev_t || now - s->mem_prev_t != 1)) {
        for (unsigned i = 0; i < 5; i++) s->mem_pending[i].valid = 0;
        s->mem_prev_valid = 0;
    }
    int32_t mx[MNET_F];
    for (int i = 0; i < MNET_F; i++) mx[i] = x[i];
    uint32_t growth = s->mem_prev_valid && in->mem_used > s->mem_prev_used ? in->mem_used - s->mem_prev_used : 0;
    mx[12] = q16(growth, 10);
    s->mem_prev_used = in->mem_used;
    s->mem_prev_t = now;
    s->mem_prev_valid = 1;
    for (unsigned i = 0; i < 5; i++) {
        mind_mem_pending_t* p = &s->mem_pending[i];
        if (!p->valid || now <= p->created_t) continue;
        uint32_t elapsed = now - p->created_t;
        if (elapsed > 4) { p->valid = 0; continue; }
        if (in->mem_used > p->max_used) p->max_used = in->mem_used;
        if (elapsed == 4) {
            if (!p->acted) mnet_train(&s->m.memory_net, p->x, p->max_used >= 90);
            p->valid = 0;
        }
    }
    mind_mem_pending_t* slot = &s->mem_pending[s->mem_pending_pos++ % 5];
    for (int i = 0; i < MNET_F; i++) slot->x[i] = mx[i];
    slot->max_used = in->mem_used;
    slot->created_t = now;
    slot->acted = 0;
    slot->valid = 1;
    s->mem_risk_q16 = mnet_predict(&s->m.memory_net, mx);
    if (mnet_trusted(&s->m.memory_net) && s->mem_risk_q16 >= (MNET_ONE * 60u) / 100u) {
        s->mem_forecasts++;
        if (now - s->mem_logged_t >= 30 || !s->mem_logged_t) {
            s->mem_logged_t = now;
            jlog(s, now, MJ_MEMORY, 0, 0, "предвижу давление памяти: риск %u%% достижения 90%% ОЗУ за 4 с (только наблюдение)",
                 s->mem_risk_q16 * 100 / MNET_ONE);
        }
    }
}

uint32_t mind_memory_risk(const mind_t* s) {
    return mnet_trusted(&s->m.memory_net) ? s->mem_risk_q16 : 0;
}

static void foresight(mind_t* s, const mind_sample_t* in, uint32_t now) {
    uint32_t late_pct = in->loops ? in->late * 100 / in->loops : 0;
    if (s->nx_have && !s->nx_acted && in->loops) mnet_train(&s->m.net, s->nx_prev, late_pct >= 5);
    int32_t x[MNET_F];
    mind_make_features(s, in, x);
    memory_foresight(s, in, x, now);
    s->risk_q16 = mnet_predict(&s->m.net, x);
    for (int i = 0; i < MNET_F; i++) s->nx_prev[i] = x[i];
    s->nx_have = in->loops != 0;                               /* без кадров дисплея исхода не будет */
    s->nx_acted = 0;
    int acting = s->m.mode == MIND_ACT;
    if (s->risk_q16 >= MNET_ONE / 100 * (uint32_t)ML(s, ML_RISK_THR) && mnet_trusted(&s->m.net) && s->nx_have) {
        if (++s->pre_skip >= 10) { s->pre_skip = 0; return; }  /* проверка: эта секунда - без упреждения */
        s->risk_why = mnet_top_feature(&s->m.net, x);
        if (acting) { s->pre_until_ms = (now + 1) * 1000 + (uint32_t)ML(s, ML_PRE_HOLD); s->nx_acted = 1; s->m.forecasts++; }
        if (in->late == 0 && (now - s->pre_logged_t >= 30 || !s->pre_logged_t)) {   /* опоздания уже идут - это видно и так */
            s->pre_logged_t = now;
            jlog(s, now, MJ_LIMIT, acting, 0, "%sпредвижу опоздание экрана (риск %u%%, главная причина - %s): фон ужимается заранее",
                 acting ? "" : "(наблюдение) ", s->risk_q16 * 100 / MNET_ONE, mind_feature_name(s->risk_why));
        }
    }
}

/* ---------------- рычаги 13-30: проверка по замеру, откат, арбитраж ----------------
   Каждый рычаг - величина с лесенкой допустимых значений (границы), обычным значением и оценкой (чем больше, тем
   лучше), построенной из реальных замеров секунды (см. ml_score). Проверка (A/B) - по одному рычагу за раз:
     база  - оценка за последние ML_W секунд при текущем значении;
     проба - соседнее значение ML_W секунд (первую секунду после смены не считаем);
     выигрыш - оценка выше базы на запас (ml_margin) и нагрузка за окна сравнима, иначе откат;
     подтверждение - ещё ML_W секунд: оценка не ниже базы, иначе откат (дрейф нагрузки не выдаёт себя за успех).
   Срочный откат: пока рычаг изменён, опоздания экрана / ожидание потоков / недоборы звука / ошибки диска заметно
   выросли над базой - значение возвращается сразу, рычаг остывает 2 мин; три срочных за 10 мин - пробы стоят 5 мин.
   Общий откат: в течение минуты после последнего оставленного изменения опоздания экрана выросли на 30 пунктов -
   все рычаги к обычным значениям, пробы стоят 3 мин. Бюджет: не больше 5 смен значения в минуту.
   «Не помогает»: три круга (по две неудачные пробы в обе стороны) без выигрыша - рычаг не трогается, пока не
   сменится класс нагрузки (сигнатура: загрузка, очередь, звук, диск, опоздания, отказы запусков). */
enum { SC_SCREEN, SC_WAIT, SC_TACT, SC_AUDIO, SC_AUDCPU, SC_DISK };
typedef struct { const char* name; const char* unit; uint8_t n, def; int32_t v[5]; uint8_t cls; } ml_def_t;
static const ml_def_t LD[ML_N] = {
    { "вес окна в фокусе", "/4", 5, 2, { 4, 5, 6, 7, 8 }, SC_SCREEN },
    { "порог голодания потока", "мс", 5, 2, { 1000, 1500, 2000, 3000, 4000 }, SC_WAIT },
    { "порог риска для упреждения", "%", 5, 2, { 40, 50, 60, 70, 80 }, SC_SCREEN },
    { "срок удержания упреждения", "мс", 4, 1, { 150, 300, 600, 1200 }, SC_SCREEN },
    { "спокойствие до снятия пределов фона", "мс", 4, 1, { 200, 400, 800, 1600 }, SC_SCREEN },
    { "период тактики", "мс", 4, 0, { 25, 40, 60, 100 }, SC_TACT },
    { "допуск копий программ", "%", 4, 1, { 75, 100, 150, 200 }, SC_SCREEN },
    { "предел необычной программы", "%", 4, 2, { 30, 40, 50, 70 }, SC_SCREEN },
    { "опоздавших кадров для ядра фокуса", "шт.", 3, 0, { 1, 2, 3 }, SC_SCREEN },
    { "спокойствие до роста темпа фона", "мс", 3, 1, { 500, 1000, 2000 }, SC_SCREEN },
    { "интервал сужения темпа фона", "мс", 3, 1, { 100, 200, 400 }, SC_SCREEN },
    { "спокойствие до роста анимации", "с", 3, 1, { 15, 30, 60 }, SC_SCREEN },
    { "терпение до снижения анимации", "с", 3, 1, { 2, 3, 5 }, SC_SCREEN },
    { "опережение звука перед картой", "мс", 5, 2, { 40, 60, 100, 150, 250 }, SC_AUDIO },
    { "период микшера звука", "мс", 3, 1, { 5, 10, 20 }, SC_AUDCPU },
    { "предел одного запроса к диску", "блоков", 3, 2, { 16, 64, 256 }, SC_DISK },
    { "порог взятия чужого потока ядром", "квантов", 4, 1, { 1, 2, 4, 8 }, SC_SCREEN },
    { "окно предела доли процессора", "тиков", 3, 1, { 25, 50, 100 }, SC_SCREEN },
};

const char* mind_lever_name(int L) { return L >= 0 && L < ML_N ? LD[L].name : "?"; }
const char* mind_lever_unit(int L) { return L >= 0 && L < ML_N ? LD[L].unit : ""; }
int32_t mind_lever_default(int L) { return L >= 0 && L < ML_N ? LD[L].v[LD[L].def] : 0; }
int32_t mind_lever_val(const mind_t* s, int L) {
    if (L < 0 || L >= ML_N) return 0;
    unsigned i = s->lv.one[L].idx;
    return LD[L].v[i < LD[L].n ? i : LD[L].def];
}
int mind_lever_state(const mind_t* s, int L) {
    if (L < 0 || L >= ML_N) return 0;
    const ml_one_t* o = &s->lv.one[L];
    if (s->lv.state && s->lv.lever == L) return 2;
    if (o->useless) return 3;
    if (o->idx != LD[L].def) return 1;
    return 0;
}
void mind_lever_stats(const mind_t* s, int L, unsigned* trials, unsigned* wins, unsigned* backs) {
    if (L < 0 || L >= ML_N) { *trials = 0; *wins = 0; *backs = 0; return; }
    const ml_one_t* o = &s->lv.one[L];
    *trials = o->trials; *wins = o->wins; *backs = o->backs;
}

static void ml_init(mind_t* s) {
    for (int L = 0; L < ML_N; L++) {
        ml_one_t* o = &s->lv.one[L];
        o->idx = LD[L].def;
        const mind_lv_saved_t* sv = &s->m.lv[L];
        o->hint = sv->wins > 0 && sv->best < LD[L].n && sv->best != LD[L].def;
        o->dir = o->hint && sv->best < LD[L].def;     /* подсказка вниз - сразу вниз */
    }
}

/* Применить значение: политика Разума читает его сама, рычаги ядра - через исполнителя. */
static void ml_set(mind_t* s, const mind_act_t* act, int L, unsigned idx, uint32_t now) {
    ml_t* l = &s->lv;
    l->one[L].idx = (uint8_t)idx;
    int32_t v = LD[L].v[idx];
    if (L >= ML_AUDIO_LEAD && act->set_lever) act->set_lever(L, v);
    if (L == ML_FOCUS_W && s->focus_pid) {
        mind_track_t* t = track_of(s, s->focus_pid);
        if (t && t->weight != 8) apply_weight(s, t, (uint32_t)v, act);
    }
    if (L == ML_ODD_LIMIT)
        for (int i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive && s->tr[i].flagged == 1) apply_limit(s, &s->tr[i], act);
    l->chg_t[l->chg_n++ % 6] = now ? now : 1;
    l->total_changes++;
}

static unsigned ml_changes_last_min(const ml_t* l, uint32_t now) {
    unsigned n = 0;
    for (int i = 0; i < 6; i++) if (l->chg_t[i] && now - l->chg_t[i] < 60) n++;
    return n;
}

static void ml_rec_fill(mind_t* s, const mind_sample_t* in) {
    ml_t* l = &s->lv;
    ml_rec_t* r = &l->rec[l->rn % ML_RING];
    uint32_t cores = in->cores ? in->cores : 1, bg = 0;
    for (int i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive && s->tr[i].pid != s->focus_pid) bg += s->tr[i].cpu_x10;
    uint64_t busy = (uint64_t)in->disk_ops * in->disk_lat_us;
    r->late = in->late; r->loops = in->loops; r->wait = in->max_wait_ms; r->bg_pm = bg / cores;
    r->load = in->load; r->run = in->runnable; r->under = in->audio_under;
    r->ops = in->disk_ops; r->kb = in->disk_kb; r->busy_us = busy > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)busy;
    r->tact = in->tact_cpu_x100; r->aud = in->audio_cpu_x100; r->audio_on = in->audio_mixed > 0;
    r->lead = (uint32_t)ML(s, ML_AUDIO_LEAD); r->errs = in->dev_errors; r->denied = in->denied;
    l->rn++;
}

typedef struct { uint32_t n, loops, late, wait, bg, load, run, under, ops, kb, busy, tact, aud, audio_s, errs, denied, lead; } ml_agg_t;
static void ml_agg(const ml_t* l, uint32_t from, uint32_t to, ml_agg_t* a) {
    mzero(a, sizeof *a);
    if (to > l->rn) to = l->rn;
    if (l->rn - from > ML_RING) from = l->rn - ML_RING;
    for (uint32_t k = from; k < to; k++) {
        const ml_rec_t* r = &l->rec[k % ML_RING];
        a->n++; a->loops += r->loops; a->late += r->late; a->wait += r->wait; a->bg += r->bg_pm; a->load += r->load;
        a->run += r->run; a->under += r->under; a->ops += r->ops; a->kb += r->kb; a->busy += r->busy_us;
        a->tact += r->tact; a->aud += r->aud; a->audio_s += r->audio_on; a->errs += r->errs; a->denied += r->denied; a->lead += r->lead;
    }
}
static int32_t late_pm_of(const ml_agg_t* a) { return a->loops ? (int32_t)((uint64_t)a->late * 1000 / a->loops) : 0; }

/* Оценка: чем больше, тем лучше. Экран важнее фона: 1% опоздавших кадров стоит 4% процессора, отданного фону;
   ожидание потока 100 мс - как 25 тысячных процессора. */
static int32_t ml_score(int cls, const ml_agg_t* a) {
    int32_t n = a->n ? (int32_t)a->n : 1;
    int32_t late_pm = late_pm_of(a), bg = (int32_t)a->bg / n, wait = (int32_t)(a->wait / (uint32_t)n);
    int32_t scr = bg - 4 * late_pm - (wait > 1000 ? 1000 : wait) / 4;
    switch (cls) {
    case SC_SCREEN: return scr;
    case SC_WAIT:   return -(wait > 5000 ? 5000 : wait) - 10 * late_pm;
    case SC_TACT:   return scr - (int32_t)(a->tact / (uint32_t)n) / 4;
    case SC_AUDIO:  return -(int32_t)(a->under * 1000 / (uint32_t)n) - (int32_t)(a->lead / (uint32_t)n);
    case SC_AUDCPU: return -(int32_t)(a->under * 1000 / (uint32_t)n) - (int32_t)(a->aud / (uint32_t)n) / 2;
    default:        return a->busy ? (int32_t)((uint64_t)a->kb * 100000 / a->busy) : 0;     /* КБ на мс занятости диска, x100 */
    }
}
static int32_t ml_margin(int cls, int32_t base) {
    int32_t a = base < 0 ? -base : base, m;
    switch (cls) {
    case SC_SCREEN: case SC_TACT: m = a / 20; return m < 20 ? 20 : m;
    case SC_WAIT: m = a / 10; return m < 40 ? 40 : m;
    case SC_AUDIO: return 30;
    case SC_AUDCPU: m = a / 10; return m < 8 ? 8 : m;
    default: m = a / 12; return m < 1 ? 1 : m;
    }
}
/* Окна честно сравнимы: нагрузка близка; для звука - играл в обоих; для диска - объём обмена одного порядка. */
static int ml_comparable(int cls, const ml_t* l, const ml_agg_t* a) {
    uint32_t n = a->n ? a->n : 1, ld = a->load / n;
    if ((ld > l->base_load ? ld - l->base_load : l->base_load - ld) > 20) return 0;
    if (cls == SC_AUDIO || cls == SC_AUDCPU) return a->audio_s * 10 >= n * 7 && l->base_audio * 10 >= ML_W * 7;
    if (cls == SC_DISK) return a->ops * 2 >= l->base_ops && a->ops <= l->base_ops * 2;
    return 1;
}

/* Класс нагрузки: биты с гистерезисом по последним секундам. */
static void ml_signature(mind_t* s, const mind_sample_t* in) {
    ml_t* l = &s->lv;
    uint32_t cores = in->cores ? in->cores : 1;
    int on[6] = { in->load >= 70, in->runnable > cores, in->audio_mixed > 0, in->disk_ops >= 30,
                  in->loops && in->late * 20 >= in->loops, in->denied > 0 };
    uint32_t sig = l->sig;
    for (int b = 0; b < 6; b++) {
        int c = l->sigc[b] + (on[b] ? 1 : -1);
        l->sigc[b] = (int8_t)(c < 0 ? 0 : c > 10 ? 10 : c);
        if (l->sigc[b] >= 6) sig |= 1u << b;
        else if (l->sigc[b] <= 3) sig &= ~(1u << b);
    }
    l->sig = sig;
}

static void ml_seen(mind_t* s, const mind_sample_t* in, uint32_t now) {
    ml_t* l = &s->lv;
    if (s->m.forecasts != l->prev_fc) { l->prev_fc = s->m.forecasts; l->seen_fc = now; }
    if (s->tact_on) l->seen_tact = now;
    if (s->core_res_pid) l->seen_core = now;
    if (s->bg_fps_level) l->seen_bg = now;
    for (int i = 0; i < MIND_PROC_MAX; i++) if (s->tr[i].alive && s->tr[i].flagged == 1) { l->seen_odd = now; break; }
    if (in->loops && in->late * 10 >= in->loops) l->seen_fxbad = now;
    if (s->fx < MIND_FX_HIGH) l->seen_fxlow = now;
}

static int ml_recent(uint32_t seen, uint32_t now) { return seen && now - seen <= 30; }

/* Есть ли рычагу что измерять сейчас (0 - нечего: рычаг не трогается). why - причина для журнала. */
static unsigned ml_need(const mind_t* s, int L, uint32_t now, const char** why) {
    const ml_t* l = &s->lv;
    ml_agg_t a;
    ml_agg(l, l->rn > 16 ? l->rn - 16 : 0, l->rn, &a);
    uint32_t n = a.n ? a.n : 1;
    int pressure = late_pm_of(&a) >= 50 || a.wait / n >= 100;
    *why = "экран опаздывает или потоки ждут процессор";
    switch (L) {
    case ML_FOCUS_W: return pressure && s->focus_pid ? 70 : 0;
    case ML_STARVE_MS: *why = "потоки долго ждут процессор"; return a.wait / n >= 300 ? 60 : 0;
    case ML_RISK_THR: case ML_PRE_HOLD: *why = "упреждение срабатывало"; return ml_recent(l->seen_fc, now) ? 60 : 0;
    case ML_RELEASE: case ML_LIM_WIN: *why = "тактика ужимала фон"; return ml_recent(l->seen_tact, now) ? 55 : 0;
    case ML_TACT_PERIOD: *why = "тактика заметно загружает процессор"; return !pressure && a.tact / n >= 40 ? 25 : 0;
    case ML_ADMIT: *why = "запуски отклонялись"; return a.denied > 0 ? 60 : 0;
    case ML_ODD_LIMIT: *why = "необычная программа ограничена"; return ml_recent(l->seen_odd, now) ? 50 : 0;
    case ML_CORE_RES: *why = "ядро фокуса отдавалось"; return ml_recent(l->seen_core, now) ? 55 : 0;
    case ML_BG_CALM: case ML_BG_STEP: *why = "темп фона снижался"; return ml_recent(l->seen_bg, now) ? 50 : 0;
    case ML_FX_CALM: *why = "анимация снижена"; return ml_recent(l->seen_fxlow, now) && !pressure ? 40 : 0;
    case ML_FX_BAD: *why = "кадры опаздывали"; return ml_recent(l->seen_fxbad, now) ? 45 : 0;
    case ML_STEAL: *why = ml_recent(l->seen_skew, now) ? "перекос ядер: одни перегружены, другие простаивают" : "очередь потоков при занятых ядрах";
        return ml_recent(l->seen_skew, now) ? 65 : pressure && a.run / n > 1 ? 45 : 0;
    case ML_AUDIO_LEAD: *why = a.under ? "были недоборы звука" : "звук играет - задержку можно уменьшить";
        return a.audio_s * 2 >= n ? (a.under ? 90 : 40) : 0;
    case ML_AUDIO_PERIOD: *why = "звук играет - микшер можно разгрузить"; return a.audio_s * 2 >= n ? 30 : 0;
    case ML_KFS_RUN: *why = "диск занят"; return a.ops / n >= 30 && a.kb / n >= 256 ? 40 : 0;
    default: return 0;
    }
}

static void ml_forget_best(mind_t* s, int L) {
    s->m.lv[L].best = LD[L].def; s->m.lv[L].wins = 0;
}

static void ml_back(mind_t* s, const mind_act_t* act, uint32_t now) {      /* вернуть значение до пробы */
    ml_t* l = &s->lv;
    ml_set(s, act, l->lever, (unsigned)l->from, now);
    l->state = 0;
    l->next_t = now + 10;
    l->guard_until = 0;
}

static void ml_trial_done(mind_t* s, int L, int won, uint32_t now) {
    ml_t* l = &s->lv;
    ml_one_t* o = &l->one[L];
    mind_lv_saved_t* sv = &s->m.lv[L];
    if (o->trials < 0xFFFF) o->trials++;
    if (sv->trials < 0xFFFF) sv->trials++;
    if (won) {
        if (o->wins < 0xFFFF) o->wins++;
        if (sv->wins < 0xFFFF) sv->wins++;
        sv->best = o->idx; o->fails = 0; o->rounds = 0;
        o->cool_until = now + 30;
        return;
    }
    if (o->backs < 0xFFFF) o->backs++;
    if (sv->wins && sv->best == (uint8_t)l->to) ml_forget_best(s, L);          /* подсказка не подтвердилась */
    o->dir = !o->dir;
    if (++o->fails >= 2) {
        o->fails = 0;
        o->cool_until = now + 90;
        if (++o->rounds >= 3) {
            o->useless = 1; o->sig = l->sig;
            jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: не помогает при такой нагрузке - не трогаю, пока она не изменится",
                 (uint32_t)(13 + L), LD[L].name);
        }
    }
}

static void ml_freeze(mind_t* s, uint32_t now, uint32_t secs, const char* why) {
    s->lv.frozen_until = now + secs;
    jlog(s, now, MJ_LEVER, 1, 0, "пробы рычагов остановлены на %u с: %s", secs, why);
}

/* Срочный откат во время пробы или подтверждения. */
static void ml_emergency(mind_t* s, const mind_act_t* act, uint32_t now, const char* why) {
    ml_t* l = &s->lv;
    int L = l->lever;
    jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: срочный откат на %d %s - %s", (uint32_t)(13 + L), LD[L].name, LD[L].v[l->from], LD[L].unit, why);
    ml_back(s, act, now);
    l->one[L].cool_until = now + 120;
    l->one[L].dir = !l->one[L].dir;
    l->one[L].backs++;
    if (s->m.lv[L].wins && s->m.lv[L].best == (uint8_t)l->to) ml_forget_best(s, L);
    uint32_t recent = 0;
    for (int i = 0; i < 3; i++) if (l->emerg_t[i] && now - l->emerg_t[i] < 600) recent++;
    l->emerg_t[l->emerg_n++ % 3] = now;
    if (recent >= 2) ml_freeze(s, now, 300, "три срочных отката за 10 минут");
}

static unsigned ml_nondefault(const ml_t* l) {
    unsigned n = 0;
    for (int L = 0; L < ML_N; L++) if (l->one[L].idx != LD[L].def) n++;
    return n;
}

void mind_levers_reset(mind_t* s, const mind_act_t* act) {
    ml_t* l = &s->lv;
    for (int L = 0; L < ML_N; L++) if (l->one[L].idx != LD[L].def) ml_set(s, act, L, LD[L].def, 0);
    l->state = 0; l->guard_until = 0;
}

static void ml_guard(mind_t* s, const mind_act_t* act, uint32_t now) {
    ml_t* l = &s->lv;
    if (l->state) {
        if (l->rn < l->rn0 + 3) return;                      /* меньше двух секунд после смены - судить рано */
        ml_agg_t a;
        ml_agg(l, l->rn - 3 < l->rn0 + 1 ? l->rn0 + 1 : l->rn - 3, l->rn, &a);
        uint32_t n = a.n ? a.n : 1;
        int cls = LD[l->lever].cls;
        int32_t lp = late_pm_of(&a);
        const char* why = 0;
        if (lp >= (int32_t)l->base_late_pm + 250 && lp >= 150) why = "кадры экрана опаздывают заметно чаще";
        else if (a.wait / n >= l->base_wait + 1500) why = "потоки стали ждать процессор намного дольше";
        else if ((cls == SC_AUDIO || cls == SC_AUDCPU) && a.under >= l->base_under * n / ML_W + 3) why = "появились недоборы звука";
        else if (cls == SC_DISK && a.errs) why = "ошибки обмена с диском";
        if (why) ml_emergency(s, act, now, why);
        return;
    }
    if (l->guard_until > now && ml_nondefault(l)) {
        ml_agg_t a;
        ml_agg(l, l->rn > 5 ? l->rn - 5 : 0, l->rn, &a);
        int32_t lp = late_pm_of(&a);
        if (a.n >= 5 && lp >= (int32_t)l->guard_late_pm + 300 && lp >= 200) {
            unsigned k = ml_nondefault(l);
            jlog(s, now, MJ_LEVER, 1, 0, "общий откат: %u рычагов возвращены к обычным значениям - после изменений опоздало %u%% кадров против %u%% до них",
                 k, (uint32_t)(lp / 10), l->guard_late_pm / 10);
            for (int L = 0; L < ML_N; L++) if (l->one[L].idx != LD[L].def) { ml_set(s, act, L, LD[L].def, now); ml_forget_best(s, L); l->one[L].cool_until = now + 180; }
            l->guard_until = 0; l->general_rollbacks++;
            ml_freeze(s, now, 180, "общий откат");
        }
    }
}

static void ml_start(mind_t* s, const mind_act_t* act, uint32_t now) {
    ml_t* l = &s->lv;
    if (l->rn < ML_W + 2 || ml_changes_last_min(l, now) >= 5) return;
    int best = -1; unsigned bestk = 0;
    for (int L = 0; L < ML_N; L++) {
        ml_one_t* o = &l->one[L];
        if (o->useless && o->sig == l->sig) continue;
        if (o->useless) { o->useless = 0; o->rounds = 0; o->fails = 0; }          /* нагрузка сменилась - рычагу снова можно пробовать */
        if (o->cool_until > now) continue;
        const char* why;
        unsigned need = ml_need(s, L, now, &why);
        if (!need) continue;
        unsigned age = now - o->last_try; if (age > 999) age = 999;
        unsigned k = need * 1000 + age;
        if (k > bestk) { bestk = k; best = L; }
    }
    if (best < 0) return;
    int L = best;
    ml_one_t* o = &l->one[L];
    const char* why; ml_need(s, L, now, &why);
    ml_agg_t b;
    ml_agg(l, l->rn - ML_W, l->rn, &b);
    int cls = LD[L].cls;
    if ((cls == SC_AUDIO || cls == SC_AUDCPU) && b.audio_s * 10 < ML_W * 7) { o->cool_until = now + 20; return; }
    if (cls == SC_DISK && b.ops < ML_W * 20) { o->cool_until = now + 20; return; }
    int tgt;
    const mind_lv_saved_t* sv = &s->m.lv[L];
    if (o->hint && sv->wins && sv->best < LD[L].n && sv->best != o->idx) tgt = sv->best;
    else {
        tgt = o->idx + (o->dir ? -1 : 1);
        if (tgt < 0 || tgt >= LD[L].n) { o->dir = !o->dir; tgt = o->idx + (o->dir ? -1 : 1); }
    }
    o->hint = 0;
    if (tgt < 0 || tgt >= LD[L].n) { o->cool_until = now + 120; return; }
    l->lever = L; l->from = o->idx; l->to = tgt;
    l->base_s = ml_score(cls, &b);
    l->base_load = b.load / (b.n ? b.n : 1); l->base_ops = b.ops; l->base_late_pm = (uint32_t)late_pm_of(&b);
    l->base_wait = b.wait / (b.n ? b.n : 1); l->base_under = b.under; l->base_audio = b.audio_s;
    o->last_try = now; l->t0 = now;
    jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: пробую %d %s вместо %d (причина: %s; оценка сейчас %d)", (uint32_t)(13 + L), LD[L].name,
         LD[L].v[tgt], LD[L].unit, LD[L].v[l->from], why, l->base_s);
    ml_set(s, act, L, (unsigned)tgt, now);
    l->state = 1; l->rn0 = l->rn;
}

static void ml_progress(mind_t* s, const mind_act_t* act, uint32_t now) {
    ml_t* l = &s->lv;
    if (l->rn < l->rn0 + 1 + ML_W) return;
    int L = l->lever, cls = LD[L].cls;
    ml_agg_t a;
    ml_agg(l, l->rn0 + 1, l->rn, &a);
    int32_t sc = ml_score(cls, &a), margin = ml_margin(cls, l->base_s);
    int fair = ml_comparable(cls, l, &a);
    if (l->state == 1) {
        if (fair && sc >= l->base_s + margin) {
            jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: проба %d %s дала оценку %d против %d - проверяю ещё %u с", (uint32_t)(13 + L), LD[L].name,
                 LD[L].v[l->to], LD[L].unit, sc, l->base_s, (uint32_t)ML_W);
            l->state = 2; l->rn0 = l->rn - 1;
            return;
        }
        jlog(s, now, MJ_LEVER, 1, 0, fair ? "рычаг %u «%s»: откат на %d %s - оценка %d не лучше %d" : "рычаг %u «%s»: откат на %d %s - нагрузка изменилась, сравнение нечестное (%d и %d)",
             (uint32_t)(13 + L), LD[L].name, LD[L].v[l->from], LD[L].unit, sc, l->base_s);
        ml_back(s, act, now);
        if (fair) ml_trial_done(s, L, 0, now); else l->one[L].cool_until = now + 30;
        return;
    }
    /* подтверждение */
    if (fair && sc >= l->base_s) {
        jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: оставлено %d %s - оценка %d против %d до пробы (подтверждено вторым окном)",
             (uint32_t)(13 + L), LD[L].name, LD[L].v[l->to], LD[L].unit, sc, l->base_s);
        l->state = 0; l->next_t = now + 10;
        l->guard_until = now + 60; l->guard_late_pm = l->base_late_pm;
        ml_trial_done(s, L, 1, now);
        return;
    }
    jlog(s, now, MJ_LEVER, 1, 0, "рычаг %u «%s»: откат на %d %s - второе окно не подтвердило (оценка %d, было %d)",
         (uint32_t)(13 + L), LD[L].name, LD[L].v[l->from], LD[L].unit, sc, l->base_s);
    ml_back(s, act, now);
    ml_trial_done(s, L, 0, now);
}

static void levers(mind_t* s, const mind_sample_t* in, const mind_act_t* act, uint32_t now) {
    ml_t* l = &s->lv;
    ml_rec_fill(s, in);
    ml_signature(s, in);
    ml_seen(s, in, now);
    if (s->m.mode != MIND_ACT) return;
    ml_guard(s, act, now);
    if (l->frozen_until > now) return;
    if (l->state) { ml_progress(s, act, now); return; }
    if (now < l->next_t) return;
    ml_start(s, act, now);
}

void mind_step(mind_t* s, const mind_sample_t* in, const mind_act_t* act) {
    uint32_t now = in->t;
    s->activity = 0;
    s->m.lifetime_s++;
    if (!s->started) {
        s->started = 1;
        if (s->m.mode == MIND_ACT && act->set_quantum) act->set_quantum(Qv[s->q_idx]);
        if (act->set_fx) act->set_fx(s->fx);
    }
    processes(s, in, act, now);
    foresight(s, in, now);
    learn(s, in, now);
    s->tact_cost_x100 = in->tact_cpu_x100;
    assess(s, in, now);
    tuner(s, in, act, now);
    visual(s, in, act, now);
    memory(s, in, act, now);
    if (s->m.mode == MIND_ACT && s->mem_level && (act->mem_trim || act->heap_restrict || act->kill))
        for (unsigned i = 0; i < 5; i++) if (s->mem_pending[i].valid) s->mem_pending[i].acted = 1;
    devices(s, in, act, now);
    levers(s, in, act, now);
    s->hpos++;
}
