// Хост-тест рычагов 13-30 Разума: проба и оставление по замеру, откат при ухудшении, срочный откат, «не помогает»
// и возврат после смены нагрузки, бюджет смен, общий откат, режим наблюдения, подсказки из сохранённого опыта и семя.
// Мир задан функциями: замеры секунды зависят от значений рычагов, как они зависели бы в системе.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mind.h"

#define ML(s, L) mind_lever_val((s), (L))
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int32_t hook[ML_N];
static unsigned hook_calls[ML_N];
static void a_lv(int L, int32_t v) { hook[L] = v; hook_calls[L]++; }
static uint32_t weight[100], limit[100];
static void a_w(uint32_t pid, uint32_t w) { weight[pid] = w; }
static void a_l(uint32_t pid, uint32_t p) { limit[pid] = p; }
static const mind_act_t act = { 0, a_w, a_l, 0, 0, 0, 0, 0, 0, 0, 0, 0, a_lv };

static mind_t M;
static mind_sample_t S;
static uint32_t T;
static uint64_t bgcyc;

/* что видно в журнале за всё время (журнал хранит 24 записи, поэтому читаем по ходу) */
static struct { unsigned try_, keep, back, emerg, useless, general, freeze, fair; } ev;
static uint32_t seen_jn;
static void scan(void) {
    while (seen_jn < M.jn) {
        const mind_event_t* e = mind_journal(&M, M.jn - 1 - seen_jn);
        seen_jn++;
        if (!e || e->kind != MJ_LEVER) continue;
        if (strstr(e->text, "пробую")) ev.try_++;
        if (strstr(e->text, "оставлено")) ev.keep++;
        if (strstr(e->text, "срочный откат")) ev.emerg++;
        else if (strstr(e->text, "откат на")) ev.back++;
        if (strstr(e->text, "сравнение нечестное")) ev.fair++;
        if (strstr(e->text, "не помогает")) ev.useless++;
        if (strstr(e->text, "общий откат")) ev.general++;
        if (strstr(e->text, "остановлены")) ev.freeze++;
    }
}

#define SHOW(l) do { if (getenv("V")) printf("%-10s try=%u keep=%u back=%u emerg=%u useless=%u general=%u freeze=%u fair=%u focus_w=%d lead=%d run=%d\n", l, ev.try_, ev.keep, ev.back, ev.emerg, ev.useless, ev.general, ev.freeze, ev.fair, (int)ML(&M, ML_FOCUS_W), (int)ML(&M, ML_AUDIO_LEAD), (int)ML(&M, ML_KFS_RUN)); } while (0)
typedef void (*world_t)(void);
static void sample(world_t w) {
    memset(&S, 0, sizeof S);
    S.t = ++T; S.tsc_khz = 1000000; S.cores = 4; S.load = 50; S.runnable = 2; S.mem_used = 20; S.mem_total_mb = 512;
    S.loops = 60; S.switches = 400;
    mind_proc_t* a = &S.proc[S.nproc++];
    strcpy(a->name, "editor"); a->pid = 10; a->focus = 1; a->threads = 1; a->cpu_cycles = (uint64_t)T * 100000000ull;
    mind_proc_t* b = &S.proc[S.nproc++];
    bgcyc += 300000000ull;                         /* фон: 30% одного ядра */
    strcpy(b->name, "render"); b->pid = 20; b->threads = 1; b->cpu_cycles = bgcyc;
    w();
}
static void run(unsigned secs, world_t w) {
    for (unsigned i = 0; i < secs; i++) { sample(w); mind_step(&M, &S, &act); scan(); }
}

/* ---- миры ---- */
static void w_calm(void) { S.late = 0; }
/* вес фокуса 7 и выше убирает опоздания; ниже - 12 из 60 кадров опаздывают (давление есть, есть что улучшать) */
static void w_focus_helps(void) { S.late = (uint32_t)ML(&M, ML_FOCUS_W) >= 7 ? 2 : 12; }
/* на 6 терпимо (давление есть), любой другой вес - катастрофа */
static void w_focus_harms(void) { S.late = (uint32_t)ML(&M, ML_FOCUS_W) == 6 ? 12 : 55; }
/* рычагу всё равно: давление то же при любом значении */
static void w_flat(void) { S.late = 12; }
static void w_flat_heavy(void) { S.late = 12; S.load = 90; S.runnable = 9; }
/* звук играет; опережение 40 мс - недоборы */
static void w_audio(void) {
    S.audio_mixed = 48000; S.audio_cpu_x100 = 50; S.late = 0;
    S.audio_under = hook[ML_AUDIO_LEAD] && hook[ML_AUDIO_LEAD] <= 40 ? 2 : 0;
}
/* диск занят; запрос 64 блока даёт лучшую пропускную способность, чем 256 и 16 */
static void w_disk(void) {
    S.disk_ops = 100; S.disk_lat_us = 500;
    int32_t run = hook[ML_KFS_RUN] ? hook[ML_KFS_RUN] : 256;
    S.disk_kb = run == 64 ? 20000 : run == 256 ? 12000 : 8000;
}


int main(void) {
    /* ---- 18 рычагов: таблица ---- */
    mind_init(&M, 0, 4);
    CHECK(ML_N == 18);
    for (int L = 0; L < ML_N; L++) {
        CHECK(mind_lever_name(L)[0] && mind_lever_unit(L) && mind_lever_val(&M, L) == mind_lever_default(L));
        CHECK(mind_lever_state(&M, L) == 0);
    }
    CHECK(mind_lever_val(&M, ML_FOCUS_W) == 6 && mind_lever_val(&M, ML_STARVE_MS) == 2000 && mind_lever_val(&M, ML_AUDIO_LEAD) == 100);

    /* ---- выигрыш: проба -> оценка выше -> подтверждение вторым окном -> оставлено ---- */
    run(30, w_calm);
    CHECK(ev.try_ == 0);                                          /* давления нет - рычаги не трогаются */
    run(120, w_focus_helps);
    CHECK(ev.keep >= 1 && mind_lever_val(&M, ML_FOCUS_W) >= 7);
    CHECK(M.m.lv[ML_FOCUS_W].wins >= 1 && M.m.lv[ML_FOCUS_W].best >= 3);   /* запомнено в модели */
    CHECK(weight[10] == (uint32_t)mind_lever_val(&M, ML_FOCUS_W));        /* и применено к окну в фокусе */

    SHOW("win");
    /* ---- ухудшение: срочный откат на прежнее значение ---- */
    mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
    run(30, w_calm);
    run(100, w_focus_harms);
    CHECK(ev.emerg >= 1);
    CHECK(mind_lever_val(&M, ML_FOCUS_W) == 6 || mind_lever_state(&M, ML_FOCUS_W) == 2);
    CHECK(ev.keep == 0 && M.m.lv[ML_FOCUS_W].wins == 0);
    {   /* пока рычаг в пробе, смена идёт по границам лесенки */
        int32_t v = mind_lever_val(&M, ML_FOCUS_W);
        CHECK(v >= 4 && v <= 8);
    }

    SHOW("harm");
    /* ---- «не помогает»: рычагам всё равно; перестаёт трогаться; после смены нагрузки пробуется снова ---- */
    mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
    run(30, w_calm);
    run(1600, w_flat);
    CHECK(ev.try_ >= 6 && ev.back >= 4 && ev.keep == 0);
    CHECK(ev.useless >= 1);
    int useless_now = 0;
    for (int L = 0; L < ML_N; L++) if (mind_lever_state(&M, L) == 3) useless_now++;
    CHECK(useless_now >= 1);
    unsigned tries_before = ev.try_;
    run(60, w_flat);
    CHECK(ev.try_ - tries_before <= 4);                           /* помеченные не трогаются: пробы почти прекратились */
    run(60, w_flat_heavy);                                        /* нагрузка другого класса (занята, очередь) */
    int useless_after = 0;
    for (int L = 0; L < ML_N; L++) if (mind_lever_state(&M, L) == 3) useless_after++;
    CHECK(useless_after < useless_now || ev.try_ > tries_before + 4);

    SHOW("flat");
    /* ---- звук: опережение уменьшается, пока нет недоборов; 40 мс даёт недоборы - срочный откат ---- */
    mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0; memset(hook, 0, sizeof hook);
    run(10, w_audio);
    run(200, w_audio);
    CHECK(ev.keep >= 1 && mind_lever_val(&M, ML_AUDIO_LEAD) == 60);
    CHECK(ev.emerg >= 1 && hook[ML_AUDIO_LEAD] == 60);            /* исполнитель получил именно оставленное значение */
    CHECK(hook_calls[ML_AUDIO_LEAD] >= 4);

    SHOW("audio");
    /* ---- диск: лучший размер запроса находится замером ---- */
    mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0; memset(hook, 0, sizeof hook);
    run(150, w_disk);
    CHECK(mind_lever_val(&M, ML_KFS_RUN) == 64 && hook[ML_KFS_RUN] == 64);

    SHOW("disk");
    /* ---- бюджет: не больше 5 смен значения в минуту при старте проб (+ откат); окно сравнения на 1 пробу ---- */
    {
        mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
        run(20, w_calm);
        uint32_t hist[600]; unsigned n = 0;
        for (unsigned i = 0; i < 500; i++) { sample(w_flat); mind_step(&M, &S, &act); scan(); hist[n++] = M.lv.total_changes; }
        unsigned worst = 0;
        for (unsigned i = 60; i < n; i++) if (hist[i] - hist[i - 60] > worst) worst = hist[i] - hist[i - 60];
        CHECK(worst <= 6);
    }

    /* ---- режим наблюдения: всё возвращено к обычному, проб нет ---- */
    mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
    run(30, w_calm); run(40, w_focus_helps);
    CHECK(mind_lever_val(&M, ML_FOCUS_W) != 6 || ev.try_ > 0);
    mind_set_mode(&M, MIND_OBSERVE, &act);
    for (int L = 0; L < ML_N; L++) CHECK(mind_lever_val(&M, L) == mind_lever_default(L));
    unsigned t0 = ev.try_;
    run(100, w_focus_helps);
    CHECK(ev.try_ == t0);

    /* ---- общий откат: после оставленного изменения экран резко опаздывает ---- */
    {
        mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
        run(30, w_calm);
        for (int i = 0; i < 200 && !ev.keep; i++) run(1, w_focus_helps);      /* до первого оставленного изменения */
        CHECK(ev.keep >= 1);
        /* сразу после подтверждения (guard 60 с): катастрофа, не связанная с пробой, - рычаги возвращаются все */
        int guarded = M.lv.guard_until > T;
        CHECK(guarded);
        if (guarded) {
            for (int i = 0; i < 8; i++) { sample(w_flat); S.late = 50; mind_step(&M, &S, &act); scan(); }
            CHECK(ev.general >= 1);
            for (int L = 0; L < ML_N; L++) CHECK(mind_lever_val(&M, L) == mind_lever_default(L));
        }
    }

    /* ---- подсказка из сохранённого опыта: следующая жизнь сразу пробует найденное значение ---- */
    {
        mind_init(&M, 0, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
        run(30, w_calm); run(120, w_focus_helps);
        CHECK(ev.keep >= 1);
        mind_model_t sv = M.m; mind_model_seal(&sv);
        mind_model_t back;
        CHECK(mind_model_load(&back, &sv, sizeof sv) && back.lv[ML_FOCUS_W].wins >= 1);
        mind_init(&M, &back, 4); memset(&ev, 0, sizeof ev); seen_jn = 0; T = 0; bgcyc = 0;
        CHECK(mind_lever_val(&M, ML_FOCUS_W) == 6);               /* значение при загрузке - всегда обычное: подсказка не применяется вслепую */
        run(30, w_calm); run(40, w_focus_helps);
        CHECK(ev.try_ >= 1);                                      /* ...а проверяется первой же пробой */

        /* семя: только веса и подсказки; нормы, квант и счётчики жизни не берутся; число наблюдений урезано */
        mind_model_t rich = M.m;
        rich.norm[MS_LOAD].n = 5000; rich.norm[MS_LOAD].mean_x100 = 4200; rich.q_best[2] = 12; rich.lifetime_s = 99999; rich.decisions = 500;
        rich.net.n = 4000; rich.net.pos = 800; rich.net.tp = 5000; rich.net.w[3] = 12345; rich.core_cap = 30; rich.tz_min = 60;
        rich.lv[ML_AUDIO_LEAD].best = 1; rich.lv[ML_AUDIO_LEAD].wins = 9; rich.lv[ML_AUDIO_LEAD].trials = 20;
        mind_model_seal(&rich);
        mind_model_t seed;
        CHECK(mind_model_seed(&seed, &rich, sizeof rich) && mind_model_valid(&seed));
        CHECK(seed.norm[MS_LOAD].n == 0 && seed.q_best[2] == 0 && seed.lifetime_s == 0 && seed.decisions == 0 && seed.core_cap == 0);
        CHECK(seed.net.n == MIND_SEED_N && seed.net.n < 120 && seed.net.w[3] == 12345 && seed.net.tp == 0);
        CHECK(!mnet_trusted(&seed.net));                          /* доверие семени не даёт: предупреждения надо заслужить здесь */
        CHECK(seed.lv[ML_AUDIO_LEAD].best == 1 && seed.lv[ML_AUDIO_LEAD].wins == 1 && seed.lv[ML_AUDIO_LEAD].trials <= 2);
        CHECK(seed.mflags & MF_SEED);
        mind_init(&M, &seed, 4);
        const mind_event_t* e0 = mind_journal(&M, 0);
        CHECK(e0 && strstr(e0->text, "семя") && strstr(e0->text, "QEMU"));
        CHECK(M.m.norm[MS_LOAD].n == 0 && M.m.net.w[3] == 12345);
        CHECK(!mind_model_seed(&seed, &rich, 10));                /* испорченный файл не принимается */
        rich.net.n = 30; mind_model_seal(&rich);
        CHECK(!mind_model_seed(&seed, &rich, sizeof rich));       /* семя из пары десятков секунд - не знание */
        /* версия 2 (без рычагов) читается и дополняется */
        mind_model_v2_t v2; memset(&v2, 0, sizeof v2);
        memcpy(&v2, &sv, __builtin_offsetof(mind_model_v2_t, crc));
        v2.version = 2; v2.norm[MS_LOAD].n = 321;
        {
            v2.crc = 0;
            uint32_t c = 0xFFFFFFFFu; const uint8_t* p = (const uint8_t*)&v2;
            for (unsigned i = 0; i < sizeof v2; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
            v2.crc = ~c;
        }
        CHECK(mind_model_load(&back, &v2, sizeof v2) && back.norm[MS_LOAD].n == 321 && back.lv[ML_FOCUS_W].wins == 0 && mind_model_valid(&back));
    }

    /* ---- допуск запусков и предел необычной программы читают рычаги ---- */
    {
        mind_init(&M, 0, 4);
        mind_launch_t a, b;
        mind_launch_limits(&M, 4, &a);
        M.lv.one[ML_ADMIT].idx = 3;                               /* 200% */
        mind_launch_limits(&M, 4, &b);
        CHECK(b.max_same >= a.max_same && b.rate_total > a.rate_total && mind_lever_val(&M, ML_ADMIT) == 200);
        M.lv.one[ML_ADMIT].idx = 0;                               /* 75%: копий меньше, частота запусков не ниже обычной (burnall) */
        mind_launch_limits(&M, 4, &b);
        CHECK(b.max_same < a.max_same && b.rate_same == a.rate_same);
    }

    printf("%s\n", fails ? "mind levers: FAILED" : "mind levers: 18 levers - win kept after confirmation, worse -> emergency rollback, flat -> marked useless and retried after load change, audio lead and disk run found by measurement, change budget, observe mode, general rollback, hints from saved experience, seed (weights only, trust capped), v2 upgrade passed");
    return fails != 0;
}
