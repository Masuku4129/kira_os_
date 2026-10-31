// Хост-тест Разума: нормы и необычности, здоровье, самонастройка кванта на модели нагрузки, фокус и фон,
// поведение программ против профиля, давление памяти, голодание, режим наблюдения, анимация, сохранение модели.
#include <stdio.h>
#include <string.h>
#include "mind.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- исполнители-записыватели ---- */
static uint32_t q_now = 4, weight[100], limit[100], killed, trims, restrict_lvl, fx_now = 99, q_sets;
static mind_profile_t profs[8]; static char prof_names[8][16]; static int nprof;
static void a_q(uint32_t t) { q_now = t; q_sets++; }
static void a_w(uint32_t pid, uint32_t w) { weight[pid] = w; }
static void a_l(uint32_t pid, uint32_t p) { limit[pid] = p; }
static void a_fx(int l) { fx_now = (uint32_t)l; }
static unsigned a_trim(void) { trims++; return 7; }
static void a_r(int l) { restrict_lvl = (uint32_t)l; }
static int a_kill(uint32_t pid) { killed = pid; return 0; }
static int a_pget(const char* n, mind_profile_t* o) { for (int i = 0; i < nprof; i++) if (!strcmp(prof_names[i], n)) { *o = profs[i]; return 0; } return -1; }
static void a_pput(const char* n, const mind_profile_t* p) {
    for (int i = 0; i < nprof; i++) if (!strcmp(prof_names[i], n)) { profs[i] = *p; return; }
    strcpy(prof_names[nprof], n); profs[nprof++] = *p;
}
static int hid_v = -1;
static void a_hid(int b) { hid_v = b; }
static int bgfps = -1;
static void a_bgfps(uint32_t f) { bgfps = (int)f; }
static const mind_act_t act = { a_q, a_w, a_l, a_fx, a_trim, a_r, a_kill, a_pget, a_pput, a_hid, a_bgfps };
static const mind_act_t none = { 0 };

static mind_t M;
static mind_sample_t S;

static void base(uint32_t t) {
    memset(&S, 0, sizeof S);
    S.t = t; S.tsc_khz = 1000000; S.cores = 4; S.load = 10; S.runnable = 2; S.mem_used = 20; S.mem_total_mb = 512;
    S.loops = 60; S.late = 0; S.switches = 400; S.disk_ops = 0;
}
static void add_proc(uint32_t pid, const char* name, uint64_t cyc, uint64_t sysc, uint32_t heap, int focus) {
    mind_proc_t* p = &S.proc[S.nproc++];
    p->pid = pid; strcpy(p->name, name); p->cpu_cycles = cyc; p->syscalls = sysc; p->heap_kb = heap; p->focus = (uint8_t)focus; p->threads = 1;
}
static int journal_has(const char* w) {
    for (unsigned i = 0; ; i++) { const mind_event_t* e = mind_journal(&M, i); if (!e) return 0; if (strstr(e->text, w)) return 1; }
}

int main(void) {
    /* ---- нормы и необычности ---- */
    mind_init(&M, 0, 4);
    CHECK(M.m.mode == MIND_ACT && M.m.fx_user == MIND_FX_AUTO && journal_has("первая жизнь"));
    uint32_t t = 1;
    for (; t <= 200; t++) { base(t); S.load = 10 + t % 5; mind_step(&M, &S, &act); }
    CHECK(q_now == 4 && fx_now == 3);                            /* при старте применены квант и анимация */
    CHECK(M.m.norm[MS_LOAD].n == 200 && M.m.norm[MS_LOAD].mean_x100 > 900 && M.m.norm[MS_LOAD].mean_x100 < 1500);
    CHECK(M.m.anomalies == 0 && M.health >= 90 && !strcmp(M.mood, "спокойна"));
    uint32_t a0 = M.m.anomalies;
    for (int k = 0; k < 2; k++, t++) { base(t); S.load = 95; mind_step(&M, &S, &act); }
    CHECK(M.m.anomalies == a0);                                   /* две секунды - ещё не необычность */
    base(t++); S.load = 95; mind_step(&M, &S, &act);
    CHECK(M.m.anomalies == a0 + 1 && journal_has("необычно: нагрузка 95"));
    int64_t mean_after = M.m.norm[MS_LOAD].mean_x100;
    CHECK(mean_after < 1700);                                     /* выброс почти не сдвинул норму */
    for (int k = 0; k < 10; k++, t++) { base(t); S.load = 95; mind_step(&M, &S, &act); }
    CHECK(M.m.anomalies == a0 + 1);                               /* повтор не чаще раза в 30 с */

    /* ---- здоровье ---- */
    base(t++); S.loops = 50; S.late = 20; S.mem_used = 96; S.runnable = 20; mind_step(&M, &S, &act);
    CHECK(M.c_resp < 10 && M.c_room < 30 && M.c_bal <= 30 && M.health < 50 && !strcmp(M.mood, "тревога"));
    for (int k = 0; k < 40; k++, t++) { base(t); mind_step(&M, &S, &act); }
    CHECK(M.health >= 90);

    /* ---- память ---- */
    trims = 0; restrict_lvl = 0; killed = 0;
    base(t++); S.mem_used = 89; add_proc(7, "hog", 0, 0, 200 * 1024, 0); mind_step(&M, &S, &act);
    CHECK(trims == 1 && M.mem_level == 1 && journal_has("вернула запасы ядра (7 страниц)"));
    base(t++); S.mem_used = 94; add_proc(7, "hog", 0, 0, 200 * 1024, 0); mind_step(&M, &S, &act);
    CHECK(restrict_lvl == 2 && killed == 0);
    base(t++); S.mem_used = 98; add_proc(7, "hog", 0, 0, 200 * 1024, 0); add_proc(8, "tiny", 0, 0, 1024, 0); mind_step(&M, &S, &act);
    CHECK(killed == 7 && M.m.kills == 1 && journal_has("остановлена hog"));
    killed = 0;
    base(t++); S.mem_used = 98; add_proc(8, "tiny", 0, 0, 1024, 0); mind_step(&M, &S, &act);
    CHECK(killed == 0);                                           /* маленькую программу не трогает */
    base(t++); S.mem_used = 50; mind_step(&M, &S, &act);
    CHECK(restrict_lvl == 0 && M.mem_level == 0 && journal_has("давление памяти ушло"));

    /* ---- фокус и фоновое ограничение ---- */
    memset(weight, 0, sizeof weight); memset(limit, 0, sizeof limit);
    uint64_t cyc1 = 0, cyc2 = 0;
    for (int k = 0; k < 3; k++, t++) {
        base(t); cyc1 += 900000000ULL; cyc2 += 300000000ULL;      /* 90% и 30% ядра */
        add_proc(11, "burn", cyc1, 0, 0, 0); add_proc(12, "cube3d", cyc2, 0, 0, 1);
        mind_step(&M, &S, &act);
    }
    CHECK(weight[12] == 6 && journal_has("фокус: cube3d"));
    for (int k = 0; k < 3; k++, t++) {
        base(t); S.late = 9; cyc1 += 900000000ULL; cyc2 += 300000000ULL;
        add_proc(11, "burn", cyc1, 0, 0, 0); add_proc(12, "cube3d", cyc2, 0, 0, 1);
        mind_step(&M, &S, &act);
    }
    CHECK(limit[11] == 50 && limit[12] == 0 && journal_has("ограничиваю фоновую burn"));
    for (int k = 0; k < 11; k++, t++) {
        base(t); cyc1 += 450000000ULL; cyc2 += 300000000ULL;
        add_proc(11, "burn", cyc1, 0, 0, 0); add_proc(12, "cube3d", cyc2, 0, 0, 1);
        mind_step(&M, &S, &act);
    }
    CHECK(limit[11] == 100 && journal_has("burn снова без ограничения"));
    base(t++); mind_step(&M, &S, &act);                           /* обе программы завершились: профили записаны */
    mind_profile_t pb; CHECK(a_pget("burn", &pb) == 0 && pb.runs == 1 && pb.cpu_x10 >= 500 && pb.cpu_x10 <= 900);

    /* ---- поведение против профиля ---- */
    strcpy(prof_names[nprof], "chat"); profs[nprof++] = (mind_profile_t){ .runs = 5, .sysc = 300, .heap_kb = 1024 };
    uint64_t sc = 0;
    for (int k = 0; k < 7; k++, t++) { base(t); sc += 50000; add_proc(20, "chat", 0, sc, 1024, 0); mind_step(&M, &S, &act); }
    CHECK(limit[20] == 50 && journal_has("chat ведёт себя необычно: 50000 вызовов/с при обычных 300"));
    for (int k = 0; k < 11; k++, t++) { base(t); sc += 200; add_proc(20, "chat", 0, sc, 1024, 0); mind_step(&M, &S, &act); }
    CHECK(limit[20] == 100 && journal_has("chat снова ведёт себя как обычно"));
    base(t++); add_proc(20, "chat", 0, sc, 64 * 1024, 0); mind_step(&M, &S, &act);
    CHECK(journal_has("chat заняла 64 МБ кучи при обычных 1 МБ"));
    strcpy(prof_names[nprof], "flaky"); profs[nprof++] = (mind_profile_t){ .runs = 6, .crashes = 4 };
    base(t++); add_proc(21, "flaky", 0, 0, 0, 0); mind_step(&M, &S, &act);
    CHECK(journal_has("flaky часто падает: 4 аварий из 6"));

    /* ---- голодание ---- */
    base(t++); S.max_wait_ms = 2500; add_proc(30, "slow", 0, 0, 0, 0); S.proc[0].wait_ms = 2500; mind_step(&M, &S, &act);
    CHECK(weight[30] == 8 && journal_has("slow ждал процессор 2500 мс"));

    /* ---- самонастройка кванта на модели: переключения ~ 1/квант, ожидание ~ квант ---- */
    mind_t* m = &M;
    mind_init(m, 0, 4);
    q_now = 0; q_sets = 0;
    for (t = 1; t <= 1500; t++) {
        base(t);
        S.runnable = 12;                                          /* потоков больше, чем ядер */
        uint32_t q = q_now ? q_now : 4;
        S.switches = 40000 / q;                                   /* оценка: 2*sw/4 + 20*wait */
        S.max_wait_ms = q * 3;                                    /* = 20000/q + 60q -> лучше всего около q = 18 -> 12 (120 мс) */
        mind_step(m, &S, &act);
    }
    CHECK(m->m.tune_trials >= 3 && m->m.tune_wins >= 2);
    CHECK(q_now == 12 || q_now == 8);
    CHECK(m->m.q_best[2] == q_now);                               /* лучшее запомнено для группы 3-4 ядер */
    CHECK(journal_has("оставляю и запоминаю для 4 ядер"));
    /* модель, где короткий квант лучше */
    mind_init(m, 0, 4); q_now = 0;
    for (t = 1; t <= 1500; t++) {
        base(t); S.runnable = 12;
        uint32_t q = q_now ? q_now : 4;
        S.switches = 400 / q; S.max_wait_ms = q * 30;
        mind_step(m, &S, &act);
    }
    CHECK(q_now == 2 && m->m.q_best[2] == 2);
    /* без конкуренции за ядра квант не трогается */
    mind_init(m, 0, 4); q_now = 0;
    for (t = 1; t <= 400; t++) { base(t); S.runnable = 3; mind_step(m, &S, &act); }
    CHECK(q_now == 4 && m->m.tune_trials == 0);

    /* ---- режим наблюдения: ничего не меняет, но записывает ---- */
    mind_init(m, 0, 4);
    mind_set_mode(m, MIND_OBSERVE, &act);
    q_sets = 0; killed = 0; trims = 0; memset(weight, 0, sizeof weight); memset(limit, 0, sizeof limit);
    for (t = 1; t <= 600; t++) {
        base(t); S.runnable = 12; S.switches = 10000; S.max_wait_ms = 50; S.late = 10; S.mem_used = t > 500 ? 99 : 30;
        add_proc(40, "big", (uint64_t)t * 900000000ULL, 0, 300 * 1024, 0); add_proc(41, "front", 0, 0, 0, 1);
        mind_step(m, &S, &act);
    }
    CHECK(q_sets == 0 && killed == 0 && trims == 0 && weight[41] == 0 && limit[40] == 0);
    CHECK(journal_has("(наблюдение) фокус: front") && journal_has("(наблюдение) память занята"));

    /* ---- анимация по запасу времени кадра ---- */
    mind_init(m, 0, 4); fx_now = 99;
    for (t = 1; t <= 4; t++) { base(t); S.late = 12; mind_step(m, &S, &act); }
    CHECK(fx_now == 2 && journal_has("анимация фона - уровень 2"));
    for (t = 5; t <= 40; t++) { base(t); mind_step(m, &S, &act); }
    CHECK(fx_now == 3);
    mind_set_fx_user(m, MIND_FX_LOW, &act);
    for (t = 41; t <= 50; t++) { base(t); S.late = 30; mind_step(m, &S, &act); }
    CHECK(fx_now == 1);                                           /* ручной уровень разум не меняет */

    /* ---- сохранение модели ---- */
    mind_model_t saved = M.m;
    mind_model_seal(&saved);
    CHECK(mind_model_valid(&saved));
    mind_init(m, &saved, 4);
    CHECK(m->m.norm[MS_LOAD].n == saved.norm[MS_LOAD].n && journal_has("опыт загружен"));
    saved.norm[0].n ^= 1;
    CHECK(!mind_model_valid(&saved));
    mind_model_t learned = { 0 }; learned.q_best[2] = 8; learned.mode = MIND_ACT; mind_model_seal(&learned);
    mind_init(m, &learned, 4);
    CHECK(mind_quantum_ms(m) == 80);                              /* выученный квант применяется с первой секунды */
    base(1); q_now = 0; mind_step(m, &S, &act);
    CHECK(q_now == 8);
    mind_init(m, 0, 4); base(1); mind_step(m, &S, &none);         /* без исполнителей - без падений */

    /* устройства: ошибки обмена - журнал с именем, стабильность падает; 10 с подряд - предупреждение */
    {
        mind_t* d = &M; mind_init(d, 0, 4);
        for (uint32_t t = 1; t <= 30; t++) { base(t); mind_step(d, &S, &act); }
        uint8_t h0 = d->health;
        for (uint32_t t = 31; t <= 42; t++) { base(t); S.dev_errors = 3; strcpy(S.dev_name, "sata0"); mind_step(d, &S, &act); }
        CHECK(journal_has("устройство sata0: ошибок обмена 3") && journal_has("sata0 ошибается 10 с подряд"));
        CHECK(d->health < h0);
        unsigned n0 = d->jn;
        base(43); S.dev_errors = 1; strcpy(S.dev_name, "sata0"); mind_step(d, &S, &act);
        CHECK(d->jn == n0);                                       /* то же устройство: не чаще раза в 30 с */
        /* иммунитет ввода: режим действий - отключить, наблюдение - отпустить */
        base(44); S.hid_suspect = 1; S.hid_rate = 400; strcpy(S.hid_name, "kbd 1234:abcd"); mind_step(d, &S, &act);
        CHECK(hid_v == 1 && d->hid_blocked == 1 && journal_has("kbd 1234:abcd: 400"));
        mind_set_mode(d, MIND_OBSERVE, &act); hid_v = -1;
        base(45); S.hid_suspect = 1; S.hid_rate = 300; strcpy(S.hid_name, "kbd 1:2"); mind_step(d, &S, &act);
        CHECK(hid_v == 0 && journal_has("наблюдение: ввод пропущен"));
        base(46); S.hid_suspect = 1; mind_step(d, &S, &none);    /* без исполнителя - без падения */
    }

    /* допуск запусков: при тревоге копий меньше и запуски реже; не допущенные - в журнал */
    {
        mind_t* d = &M; mind_init(d, 0, 8);
        mind_launch_t L;
        d->health = 95; mind_launch_limits(d, 8, &L);
        CHECK(L.max_same == 16 && L.rate_same == 8 && L.rate_total == 20);
        d->health = 50; mind_launch_limits(d, 8, &L); CHECK(L.max_same == 8);
        d->health = 30; mind_launch_limits(d, 8, &L); CHECK(L.max_same == 4 && L.rate_same == 4);
        d->health = 30; mind_launch_limits(d, 1, &L); CHECK(L.max_same == 2 && L.rate_same == 2);
        base(5); S.denied = 7; strcpy(S.denied_name, "plasma"); mind_step(d, &S, &act);
        CHECK(journal_has("не допущено запусков: 7 (последний: plasma)"));
    }

    /* тактика: экран опаздывает - фон ужимается до предела ядер за доли секунды; спокойно - пределы отпускаются */
    {
        mind_t* d = &M; mind_init(d, 0, 4);
        for (uint32_t t = 1; t <= 3; t++) {
            base(t);
            add_proc(10, "editor", (uint64_t)t * 300000000ull, t * 50, 100, 1);
            add_proc(20, "render", (uint64_t)t * 1000000000ull, t * 50, 100, 0);
            add_proc(30, "miner", (uint64_t)t * 1000000000ull, t * 50, 100, 0);
            mind_step(d, &S, &act);
        }
        limit[10] = limit[20] = limit[30] = 100;
        static mind_tact_in_t ti; memset(&ti, 0, sizeof ti);
        ti.cores = 4; ti.window_ms = 25; ti.busy_x10 = 2300; ti.loops = 2; ti.late = 1; ti.nproc = 3;
        ti.p[0].pid = 10; ti.p[0].threads = 1; ti.p[0].cpu_x10 = 300;
        ti.p[1].pid = 20; ti.p[1].threads = 1; ti.p[1].cpu_x10 = 1000;
        ti.p[2].pid = 30; ti.p[2].threads = 1; ti.p[2].cpu_x10 = 1000;
        ti.t_ms = 4000; mind_tact_step(d, &ti, &act);
        /* фон 200% ядра, на переднем плане 30%, предел 50% от 4 ядер = 200%: фону остаётся 170%, по 85% каждому */
        CHECK(limit[20] == 85 && limit[30] == 85 && limit[10] == 100 && journal_has("тактика: экран опаздывает"));
        ti.late = 0; ti.fg_wait_ms = 0;
        ti.p[1].cpu_x10 = ti.p[2].cpu_x10 = 850; ti.busy_x10 = 2000;
        for (int k = 0; k < 15; k++) { ti.t_ms += 25; mind_tact_step(d, &ti, &act); }
        CHECK(limit[20] == 85);                                  /* 0,4 с спокойствия ещё не прошли */
        for (int k = 0; k < 60; k++) { ti.t_ms += 25; mind_tact_step(d, &ti, &act); }
        CHECK(limit[20] == 100 && limit[30] == 100 && !d->tact_on && journal_has("фон снова без пределов"));
        /* без давления фон не трогается, даже если он занимает все ядра */
        ti.p[1].cpu_x10 = ti.p[2].cpu_x10 = 2000; ti.busy_x10 = 4000;
        ti.t_ms += 25; mind_tact_step(d, &ti, &act);
        CHECK(limit[20] == 100 && !d->tact_on);
        /* наблюдение: записывает, но не применяет */
        mind_set_mode(d, MIND_OBSERVE, &act); ti.late = 2; ti.t_ms += 25; mind_tact_step(d, &ti, &act);
        CHECK(limit[20] == 100 && journal_has("(наблюдение) тактика"));
        mind_tact_step(d, &ti, &none);
    }

    /* темп фоновых окон: экран опаздывает - ступени 30, 15, 8, 4 кадров/с через 200 мс; спокойно - обратно */
    {
        mind_t* d = &M; mind_init(d, 0, 4); bgfps = -1;
        static mind_tact_in_t ti; memset(&ti, 0, sizeof ti);
        ti.cores = 4; ti.window_ms = 25; ti.loops = 2; ti.late = 1; ti.busy_x10 = 1000;
        for (ti.t_ms = 1000; ti.t_ms <= 1000 + 25 * 40; ti.t_ms += 25) mind_tact_step(d, &ti, &act);
        CHECK(bgfps == 4 && mind_bg_fps(d) == 4 && journal_has("фоновые окна не чаще 4 кадров/с"));
        ti.late = 0;
        uint32_t t0 = ti.t_ms;
        for (; ti.t_ms <= t0 + 900; ti.t_ms += 25) mind_tact_step(d, &ti, &act);
        CHECK(bgfps == 4);                                     /* спокойно меньше секунды - не отпускает */
        for (; ti.t_ms <= t0 + 4000; ti.t_ms += 25) mind_tact_step(d, &ti, &act);
        CHECK(bgfps == 0 && journal_has("фоновые окна снова без предела"));
        ti.late = 1; ti.t_ms += 25; mind_tact_step(d, &ti, &act);   /* снова опоздание - сразу первая ступень */
        CHECK(bgfps == 30);
        mind_set_mode(d, MIND_OBSERVE, &act);
        CHECK(bgfps == 0);                                     /* наблюдение: всё возвращается */
    }

    /* предвидение: в этом мире тяжёлый фон в эту секунду означает опоздания экрана в следующую. Модель учится,
       получает доверие и затем ужимает фон заранее - до первого опоздавшего кадра (тактика видит «упреждение»). */
    {
        mind_t* d = &M; mind_init(d, 0, 4);
        uint64_t cyc = 0; int heavy_prev = 0; uint32_t seed = 12345;
        uint32_t t = 1;
        for (; t <= 900; t++) {
            seed = seed * 1103515245u + 12345u;
            int heavy = (seed >> 16) % 4 == 0;
            cyc += heavy ? 3000000000ull : 200000000ull;
            base(t);
            S.late = heavy_prev ? 12 : 0;
            S.load = heavy ? 85 : 20;
            add_proc(10, "editor", (uint64_t)t * 100000000ull, t * 50, 100, 1);
            add_proc(20, "render", cyc, t * 50, 100, 0);
            mind_step(d, &S, &act);
            heavy_prev = heavy;
        }
        printf("foresight: n=%u pos=%u precision=%u%% recall=%u%% trusted=%d w[bg]=%d w[late]=%d forecasts=%u\n",
               d->m.net.n, d->m.net.pos, mnet_precision(&d->m.net), mnet_recall(&d->m.net), mnet_trusted(&d->m.net),
               d->m.net.w[3] / 655, d->m.net.w[9] / 655, d->m.forecasts);
        CHECK(mnet_trusted(&d->m.net) && d->m.net.w[3] > 0);
        CHECK(d->m.forecasts > 0 && journal_has("предвижу опоздание экрана"));
        /* опасная секунда: без единого опоздавшего кадра тактика ужимает фон заранее */
        d->pre_skip = 0;
        cyc += 3000000000ull; base(t); S.load = 85;
        add_proc(10, "editor", (uint64_t)t * 100000000ull, t * 50, 100, 1);
        add_proc(20, "render", cyc, t * 50, 100, 0);
        mind_step(d, &S, &act);
        CHECK(d->risk_q16 > MNET_ONE / 2 && d->pre_until_ms > t * 1000);
        limit[20] = 100;
        static mind_tact_in_t ti; memset(&ti, 0, sizeof ti);
        ti.cores = 4; ti.window_ms = 25; ti.busy_x10 = 3100; ti.loops = 2; ti.late = 0; ti.nproc = 2;
        ti.p[0].pid = 10; ti.p[0].threads = 1; ti.p[0].cpu_x10 = 100;
        ti.p[1].pid = 20; ti.p[1].threads = 4; ti.p[1].cpu_x10 = 3000;
        ti.t_ms = t * 1000 + 100; mind_tact_step(d, &ti, &act);
        CHECK(limit[20] < 100 && journal_has("упреждение"));
        /* модель переживает перезагрузку; опыт прежнего формата (без модели) читается и дополняется */
        mind_model_t sv = d->m; mind_model_seal(&sv);
        mind_model_t back;
        CHECK(mind_model_load(&back, &sv, sizeof sv) && back.net.n == d->m.net.n && back.net.w[3] == d->m.net.w[3]);
        mind_model_v1_t old; memset(&old, 0, sizeof old);
        memcpy(&old, &sv, __builtin_offsetof(mind_model_v1_t, crc));
        old.version = 1; old.norm[MS_LOAD].n = 777;
        old.crc = 0;
        {   /* crc прежнего формата - тем же CRC-32 */
            uint32_t c = 0xFFFFFFFFu; const uint8_t* p = (const uint8_t*)&old;
            for (unsigned i = 0; i < sizeof old; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
            old.crc = ~c;
        }
        CHECK(mind_model_load(&back, &old, sizeof old) && back.norm[MS_LOAD].n == 777 && back.net.n == 0 && mind_model_valid(&back));
        old.norm[MS_LOAD].n = 778;                                /* испорченный файл не принимается */
        CHECK(!mind_model_load(&back, &old, sizeof old));
        CHECK(!mind_model_load(&back, &old, 100));
    }

    printf("%s\n", fails ? "mind: FAILED" : "mind: norms and anomalies, health, quantum self-tuning (both directions, no contention), focus and background limit, behaviour vs profile, memory pressure, starvation, observe mode, animation, model persistence, device errors, input immunity, launch admission, fast tactics (core cap, background frame rate), foresight model (learns, earns trust, acts ahead, persists, upgrades v1) passed");
    return fails != 0;
}
