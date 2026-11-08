// Стенд эффективности Разума: замкнутый детерминированный сценарий «передний план + фоновая нагрузка».
// Мир (plant) задан формулами и не содержит случайности: доля опаздывающих кадров зависит от суммарной загрузки ядер,
// а загрузка - от пределов доли процессора, которые Разум выставляет через mind_act_t.set_limit.
// Один и тот же сценарий прогоняется дважды: MIND_OBSERVE (Разум ничего не меняет - это «без Разума») и MIND_ACT.
// Метрики (строки "EFF ..." на стандартный вывод, их читает человек и ci):
//   1. late_pm        - опоздавших кадров из всех, промилле, за весь прогон и внутри всплесков нагрузки;
//   2. react_ms       - от момента, когда нагрузка без пределов стала бы давить экран (аналитически по миру), до первого предела, мс;
//   3. late_secs      - секунд внутри тяжёлых всплесков, где опоздало >= 5% кадров (экран страдал заметно);
//   4. release_ms     - от конца всплеска до снятия предела, мс;
//   5. false_act      - ограничений вне всплесков и вне 30 с после них (включая безвредный всплеск);
//   6. bg_kept_pct    - сколько работы фона сохранено за тяжёлые всплески, % от запрошенного (цена вмешательства);
//   7. churn          - смен предела фона в минуту внутри тяжёлых всплесков (дрожание: каждая смена - вызов планировщика)
//                       и число перекладываний вверх-вниз;
//   8. brier_x1e4     - Brier предвидения опоздания на следующую секунду и его база (доля опозданий в прогоне).
// Граница честности: мир описывает ТОЛЬКО процессорную причину опозданий. Числа показывают поведение контроллера
// на этой модели, а не на железе; рычаги звука, диска и темп фоновых окон мир не моделирует.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "mind.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define CORES 4
#define WIN_MS 25
#define WPS (1000 / WIN_MS)
#define RUN_S 900
#define FG_PID 10
#define BG_PID 20

typedef struct { uint32_t on, off; uint32_t thr_pct; } burst_t;      /* фон просит thr_pct% ядра на каждый из 4 потоков */
static const burst_t B[] = { { 100, 160, 100 }, { 300, 360, 100 }, { 500, 580, 100 }, { 220, 225, 30 } };
#define NB 4
#define PRESS_THR 70                                                 /* спрос на поток, при котором 40 + 4*спрос превышает 320 */
#define N_HEAVY 3                                                    /* первые три - тяжёлые; четвёртый безвреден (30% x 4 = 120% < запаса) */

static uint32_t bg_limit, fg_weight_calls, core_calls, fps_calls, limit_calls;
static uint32_t lim_t_ms[4096], lim_v[4096], nlim;
static uint32_t now_ms;
static void a_w(uint32_t pid, uint32_t w) { (void)pid; (void)w; fg_weight_calls++; }
static void a_l(uint32_t pid, uint32_t p) {
    if (pid != BG_PID) return;
    bg_limit = p; limit_calls++;
    if (nlim < 4096) { lim_t_ms[nlim] = now_ms; lim_v[nlim] = p; nlim++; }
}
static void a_bg(uint32_t f) { (void)f; fps_calls++; }
static void a_core(int on, uint32_t pid) { (void)on; (void)pid; core_calls++; }
static const mind_act_t act = { 0, a_w, a_l, 0, 0, 0, 0, 0, 0, 0, a_bg, a_core, 0 };

/* спрос фона на поток, % ядра, в секунду t (с линейным разгоном 3 с к началу всплеска) */
static uint32_t bg_demand_thr(uint32_t t_ms, int* in_heavy, int* in_any) {
    uint32_t d = 10;
    *in_heavy = 0; *in_any = 0;
    for (int i = 0; i < NB; i++) {
        uint32_t on = B[i].on * 1000, off = B[i].off * 1000;
        if (t_ms >= on && t_ms < off) {
            uint32_t ramp = t_ms - on < 3000 ? (t_ms - on) * (B[i].thr_pct - 10) / 3000 + 10 : B[i].thr_pct;
            if (ramp > d) d = ramp;
            *in_any = 1;
            if (i < N_HEAVY) *in_heavy = 1;
        }
    }
    return d;
}

typedef struct {
    uint32_t frames, late, hframes, hlate;
    uint64_t bg_req, bg_got;                 /* x10 процента ядра * окна, только внутри тяжёлых всплесков */
    int32_t react_ms[N_HEAVY], release_ms[N_HEAVY];
    uint32_t late_secs, churn_calls, heavy_secs;
    uint32_t false_act;
    double brier, brier_base; uint32_t bn, b_late;
    uint32_t forecasts, trusted, net_prec, net_rec, net_n, net_pos;
    uint32_t limit_calls, weight_calls, core_calls, fps_calls;
} res_t;

static uint32_t risk_hist[RUN_S + 2], late_pm_hist[RUN_S + 2];

static void run_scenario(int mode, res_t* r) {
    static mind_t M;
    static mind_sample_t S;
    memset(r, 0, sizeof *r);
    bg_limit = 100; fg_weight_calls = core_calls = fps_calls = limit_calls = nlim = 0; now_ms = 0;
    memset(risk_hist, 0, sizeof risk_hist); memset(late_pm_hist, 0, sizeof late_pm_hist);
    mind_init(&M, 0, CORES);
    mind_set_mode(&M, mode, &act);
    for (int i = 0; i < N_HEAVY; i++) r->react_ms[i] = r->release_ms[i] = -1;
    uint64_t cyc_fg = 0, cyc_bg = 0;
    uint32_t loop_acc = 0, late_acc = 0;     /* дробные накопители: детерминированно, без rand */
    for (uint32_t t = 1; t <= RUN_S; t++) {
        uint32_t sec_loops = 0, sec_late = 0, sec_busy = 0, sec_wait = 0;
        static mind_tact_in_t TI;
        for (int w = 0; w < WPS; w++) {
            now_ms = (t - 1) * 1000 + (uint32_t)(w + 1) * WIN_MS;
            int heavy, any;
            uint32_t dem = bg_demand_thr(now_ms, &heavy, &any);
            uint32_t lim = bg_limit;
            uint32_t eff = dem < lim ? dem : lim;                         /* на поток, % */
            uint32_t fg = 40, bg = eff * 4;                               /* % ядра: передний план 40, фон - 4 потока */
            uint32_t load = fg + bg;                                      /* из 400 */
            if (load > 400) load = 400;
            /* опоздавшая доля кадров: 0 до загрузки 320 из 400, 60% при полной */
            uint32_t late_pm_w = load > 320 ? (load - 320) * 600 / 80 : 0;
            loop_acc += 60 * WIN_MS;                                       /* кадров дисплея: 60/с */
            uint32_t loops = loop_acc / 1000; loop_acc %= 1000;
            late_acc += loops * late_pm_w;
            uint32_t late = late_acc / 1000; late_acc %= 1000;
            if (late > loops) late = loops;
            sec_loops += loops; sec_late += late; sec_busy += load;
            uint32_t waitms = load >= 380 ? 30 : 0;
            if (waitms > sec_wait) sec_wait = waitms;
            cyc_fg += (uint64_t)fg * 10 * 25000; cyc_bg += (uint64_t)bg * 10 * 25000;
            memset(&TI, 0, sizeof TI);
            TI.t_ms = now_ms; TI.window_ms = WIN_MS; TI.cores = CORES; TI.busy_x10 = load * 10;
            TI.loops = loops; TI.late = late; TI.fg_wait_ms = waitms; TI.nproc = 2;
            TI.p[0].pid = FG_PID; TI.p[0].threads = 1; TI.p[0].cpu_x10 = fg * 10;
            TI.p[1].pid = BG_PID; TI.p[1].threads = 4; TI.p[1].cpu_x10 = bg * 10;
            mind_tact_step(&M, &TI, &act);
            r->frames += loops; r->late += late;
            if (heavy) {
                r->hframes += loops; r->hlate += late;
                r->bg_req += (uint64_t)dem * 4; r->bg_got += bg;
            }
        }
        memset(&S, 0, sizeof S);
        S.t = t; S.tsc_khz = 1000000; S.cores = CORES; S.load = sec_busy / WPS / CORES;
        S.runnable = 2 + (sec_busy / WPS > 320 ? 4 : 0); S.mem_used = 30; S.mem_total_mb = 1024;
        S.loops = sec_loops; S.late = sec_late; S.switches = 400; S.max_wait_ms = sec_wait;
        S.nproc = 2;
        strcpy(S.proc[0].name, "editor"); S.proc[0].pid = FG_PID; S.proc[0].focus = 1; S.proc[0].threads = 1; S.proc[0].cpu_cycles = cyc_fg;
        strcpy(S.proc[1].name, "render"); S.proc[1].pid = BG_PID; S.proc[1].threads = 4; S.proc[1].cpu_cycles = cyc_bg;
        /* оценка риска, выданная ПРЕДЫДУЩИМ шагом, сверяется с исходом этой секунды */
        if (t > 1) {
            int y = sec_loops && sec_late * 100 / sec_loops >= 5;
            double e = risk_hist[t - 1] / (double)MNET_ONE - y;
            r->brier += e * e; r->bn++; r->b_late += (uint32_t)y;
        }
        mind_step(&M, &S, &act);
        risk_hist[t] = M.risk_q16;
        late_pm_hist[t] = sec_loops ? sec_late * 1000 / sec_loops : 0;
    }
    /* вторая проходка по журналу действий: задержки реакции и снятия, ложные действия, дрожание */
    for (int i = 0; i < N_HEAVY; i++) {
        uint32_t on = B[i].on * 1000, off = B[i].off * 1000;
        uint32_t press = on + 3000 * (PRESS_THR - 10) / (B[i].thr_pct - 10);        /* момент, когда 40 + 4*спрос > 320 */
        for (uint32_t k = 0; k < nlim; k++) {
            if (r->react_ms[i] < 0 && lim_t_ms[k] >= on && lim_t_ms[k] < off && lim_v[k] < 100)
                r->react_ms[i] = lim_t_ms[k] > press ? (int32_t)(lim_t_ms[k] - press) : 0;
            if (r->release_ms[i] < 0 && lim_t_ms[k] >= off && lim_v[k] == 100) r->release_ms[i] = (int32_t)(lim_t_ms[k] - off);
            if (lim_t_ms[k] >= on && lim_t_ms[k] < off) r->churn_calls++;
        }
        for (uint32_t t = B[i].on + 1; t <= B[i].off; t++) {
            r->heavy_secs++;
            if (late_pm_hist[t] >= 50) r->late_secs++;
        }
    }
    for (uint32_t k = 0; k < nlim; k++) {
        int ok = 0;
        for (int i = 0; i < N_HEAVY; i++)
            if (lim_t_ms[k] >= B[i].on * 1000 && lim_t_ms[k] < (B[i].off + 30) * 1000) ok = 1;
        if (!ok) r->false_act++;
    }
    r->limit_calls = limit_calls; r->weight_calls = fg_weight_calls; r->core_calls = core_calls; r->fps_calls = fps_calls;
    if (getenv("V") && mode == MIND_ACT) for (uint32_t k = 0; k < nlim && k < 60; k++) printf("  lim t=%u v=%u\n", lim_t_ms[k], lim_v[k]);
    r->forecasts = M.m.forecasts; r->trusted = (uint32_t)mnet_trusted(&M.m.net);
    r->net_prec = mnet_precision(&M.m.net); r->net_rec = mnet_recall(&M.m.net); r->net_n = M.m.net.n; r->net_pos = M.m.net.pos;
    double prev = r->bn ? r->b_late / (double)r->bn : 0.0;
    r->brier_base = 0;
    for (uint32_t t = 2; t <= RUN_S; t++) { double y = late_pm_hist[t] >= 50; double e = prev - y; r->brier_base += e * e; }
    r->brier_base /= (double)(RUN_S - 1);
    if (r->bn) r->brier /= (double)r->bn;
}

static void report(const char* tag, const res_t* r) {
    printf("EFF %s late_pm_all=%u late_pm_heavy=%u frames=%u\n", tag, r->late * 1000 / (r->frames ? r->frames : 1), r->hlate * 1000 / (r->hframes ? r->hframes : 1), r->frames);
    printf("EFF %s react_ms=%d,%d,%d late_secs=%u/%u release_ms=%d,%d,%d\n", tag, r->react_ms[0], r->react_ms[1], r->react_ms[2],
           r->late_secs, r->heavy_secs, r->release_ms[0], r->release_ms[1], r->release_ms[2]);
    printf("EFF %s false_act=%u limit_calls=%u churn_per_min=%u bg_kept_pct=%u\n", tag, r->false_act, r->limit_calls,
           r->heavy_secs ? r->churn_calls * 60 / r->heavy_secs : 0, (unsigned)(r->bg_req ? r->bg_got * 100 / r->bg_req : 0));
    printf("EFF %s other_actions weight=%u reserve_core=%u bg_fps=%u\n", tag, r->weight_calls, r->core_calls, r->fps_calls);
    printf("EFF %s brier_x1e4=%u base_x1e4=%u net_n=%u net_pos=%u prec=%u rec=%u trusted=%u forecasts=%u\n", tag, (unsigned)(r->brier * 1e4), (unsigned)(r->brier_base * 1e4),
           r->net_n, r->net_pos, r->net_prec, r->net_rec, r->trusted, r->forecasts);
}

int main(void) {
    static res_t obs, act_r;
    run_scenario(MIND_OBSERVE, &obs);
    run_scenario(MIND_ACT, &act_r);
    report("observe", &obs);
    report("act", &act_r);

    /* Инварианты стенда (не пороги качества): наблюдение ничего не меняет; сценарий реально создаёт давление. */
    CHECK(obs.limit_calls == 0);
    CHECK(obs.hframes > 0 && obs.hlate * 1000 / obs.hframes > 200);          /* без Разума всплески действительно давят экран */
    CHECK(act_r.frames == obs.frames);
    CHECK(act_r.limit_calls > 0);

    /* Защита от регрессий (границы взяты с запасом от измеренного после улучшения; до улучшения churn был 236/мин, late_pm_heavy 21,
       late_secs 2, release_ms до 1225 - см. docs/BUILDER_PLAN.md, этап D). */
    CHECK(act_r.hlate * 1000 / act_r.hframes <= 30);                 /* экран внутри всплесков: не больше 3% опоздавших кадров */
    CHECK(act_r.hlate * 5 < obs.hlate);                              /* и как минимум в 5 раз лучше, чем без Разума */
    CHECK(act_r.late_secs <= 2);
    for (int i = 0; i < N_HEAVY; i++) {
        CHECK(act_r.react_ms[i] >= 0 && act_r.react_ms[i] <= 400);   /* предел фону - в пределах 0,4 с после начала давления */
        CHECK(act_r.release_ms[i] >= 0 && act_r.release_ms[i] <= 8000);
    }
    CHECK(act_r.false_act == 0);                                     /* безвредный всплеск и спокойные участки: ни одного ограничения */
    CHECK(act_r.churn_calls * 60 / act_r.heavy_secs <= 100);         /* предел не качается: не больше 100 смен в минуту */
    CHECK(act_r.bg_req && act_r.bg_got * 100 / act_r.bg_req >= 40);  /* фон не задушен: сохранено не меньше 40% запрошенной работы */

    printf(fails ? "MIND EFFICIENCY TESTS FAILED\n" : "mind efficiency stand: scenario ran\n");
    return fails != 0;
}
