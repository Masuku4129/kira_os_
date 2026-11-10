#ifndef MIND_H
#define MIND_H

#include <stdint.h>
#include "mindnet.h"

/* Разум Киры. Раз в секунду получает замеры системы (mind_sample_t), учит для каждого сигнала норму этой машины
   (среднее и разброс), оценивает здоровье и принимает решения через исполнителей (mind_act_t):
     - доля процессора: программа в фокусе получает больший квант, фоновая программа, из-за которой опаздывает экран
       или которая ведёт себя не так, как обычно, ограничивается по доле процессора;
     - самонастройка кванта планировщика: проба соседнего значения, сравнение по замерам, откат, если не лучше;
     - уровень анимации оболочки по запасу времени кадра;
     - давление памяти: возврат запасов ядра, осторожный рост куч, в крайнем случае остановка самой большой программы;
     - голодание: поток, который долго не получает процессор, получает больший вес;
     - поведение программ: профиль каждой программы (процессор, вызовы, память, сбои) хранится между запусками;
     - устройства: ошибки обмена дисков и USB снижают стабильность и попадают в журнал с именем устройства;
       постоянные ошибки - предупреждение «сохраните данные»;
     - рычаги 13-30 (ML_*): 18 величин с лесенкой значений, измеримой оценкой и откатом; проверяются по одной (A/B),
       при ухудшении откатываются; бесполезные помечаются «не помогает» до смены нагрузки (см. блок «Рычаги» ниже);
     - иммунитет ввода: устройство ввода, печатающее быстрее человека (атака BadUSB: «флешка», которая сама набирает
       команды), рефлекторно задерживается драйвером; Разум решает - заблокировать (режим действий) или отпустить.
   Каждое решение записывается в журнал с причиной. В режиме наблюдения решения только записываются.
   Модуль чистый (без железа): все входы - в замере, все действия - через исполнителей; проверяется на хосте. */

#define MIND_SIG_N    8
#define MIND_HIST     60
#define MIND_PROC_MAX 256       /* = наибольшее число программ (scale_procs): Разум видит каждую */
#define MIND_JOURNAL  24
#define MIND_TEXT     176
#define MIND_NQ       6
#define MIND_REL_BACKOFF_MAX 2   /* выдержка перед снятием предела фона растёт не более чем в 4 раза */

enum { MS_LOAD, MS_QUEUE, MS_MEM, MS_LATE, MS_DISK, MS_IPC, MS_FAULT, MS_SYSC };
enum { MIND_OBSERVE = 0, MIND_ACT = 1 };
enum { MIND_FX_OFF = 0, MIND_FX_LOW = 1, MIND_FX_MID = 2, MIND_FX_HIGH = 3, MIND_FX_AUTO = 4 };

typedef enum {
    MJ_LEARN = 1, MJ_ANOMALY, MJ_FOCUS, MJ_LIMIT, MJ_RESTORE, MJ_TUNE_TRY, MJ_TUNE_KEEP, MJ_TUNE_BACK,
    MJ_FX, MJ_MEMORY, MJ_KILL, MJ_STARVE, MJ_BEHAVIOR, MJ_INFO, MJ_LEVER
} mind_kind_t;

typedef struct {
    uint32_t pid;
    char     name[16];
    uint64_t cpu_cycles;       /* сумма тактов TSC всех потоков процесса (растёт) */
    uint64_t syscalls;         /* системных вызовов за жизнь процесса (растёт) */
    uint32_t heap_kb;
    uint32_t threads;
    uint32_t wait_ms;          /* дольше всех ждущий готовый поток процесса, мс */
    uint8_t  focus;            /* окно процесса сейчас в фокусе */
} mind_proc_t;

typedef struct {
    uint32_t t;                /* секунда жизни этого запуска */
    uint64_t tsc_khz;
    uint32_t cores;            /* ядер в планировании */
    uint8_t  busy[256];        /* занятость каждого ядра, % */
    uint32_t load;             /* средняя занятость, % */
    uint32_t runnable;         /* потоков готовы или исполняются (без холостых) */
    uint32_t mem_used;         /* % ОЗУ */
    uint32_t mem_total_mb;
    uint32_t loops, late;      /* циклов дисплея за секунду и сколько из них опоздали */
    uint32_t worst_gap_ms;
    uint32_t disk_ops, disk_lat_us;
    uint32_t ipc;              /* сообщений через порты за секунду */
    uint32_t faults;           /* аварийных завершений программ за секунду */
    uint32_t switches;         /* переключений потоков за секунду (все ядра) */
    uint32_t max_wait_ms;      /* дольше всех ждущий готовый поток (любой), мс */
    uint32_t kwait_ms;         /* дольше всех ждущий готовый поток ядра, мс */
    char     kwait_name[16];
    uint32_t nproc;
    mind_proc_t proc[MIND_PROC_MAX];
    uint32_t dev_errors;       /* ошибок обмена с устройствами за секунду (диски, флешки, USB) */
    char     dev_name[16];     /* устройство последней ошибки */
    uint32_t hid_suspect;      /* устройств ввода задержано рефлексом за секунду */
    uint32_t hid_rate;         /* их скорость, нажатий/с */
    char     hid_name[16];
    uint32_t denied;           /* запусков, не допущенных за секунду */
    char     denied_name[16];
    /* для рычагов 13-30 */
    uint32_t audio_mixed;      /* кадров звука от программ, смешано за секунду (0 - звука нет) */
    uint32_t audio_under;      /* недоборов микшера за секунду */
    uint32_t disk_kb;          /* КБ, переданных дискам за секунду */
    uint32_t tact_cpu_x100;    /* поток mind-tact: доля одного ядра, % x100 */
    uint32_t audio_cpu_x100;   /* поток audio: доля одного ядра, % x100 */
} mind_sample_t;

/* Профиль программы (хранится на диске между запусками). */
typedef struct {
    uint32_t runs;
    uint32_t crashes;
    uint32_t cpu_x10;          /* средняя доля одного ядра, % * 10 */
    uint32_t sysc;             /* системных вызовов в секунду, в среднем */
    uint32_t heap_kb;          /* обычный пик кучи */
    uint32_t flags;
} mind_profile_t;

typedef struct {
    void (*set_quantum)(uint32_t ticks10ms);
    void (*set_weight)(uint32_t pid, uint32_t weight);        /* 1..8, 4 - обычный квант */
    void (*set_limit)(uint32_t pid, uint32_t pct);            /* доля одного ядра на поток, 100 - без ограничения */
    void (*set_fx)(int level);                                /* 0..3 */
    unsigned (*mem_trim)(void);                               /* вернуть запасы ядра; страниц */
    void (*heap_restrict)(int level);                         /* 0 - обычно, 2 - осторожно */
    int  (*kill)(uint32_t pid);
    int  (*profile_get)(const char* name, mind_profile_t* out);   /* 0 - есть */
    void (*profile_put)(const char* name, const mind_profile_t* p);
    void (*hid_verdict)(int block);                          /* задержанные устройства ввода: 1 - отключить, 0 - отпустить */
    void (*set_bg_fps)(uint32_t fps);                         /* темп фоновых окон, кадров/с (0 - без предела) */
    void (*reserve_core)(int on, uint32_t keep_pid);          /* отдать ядро окну в фокусе (фон - на остальных); 0 - вернуть */
    void (*set_lever)(int lever, int32_t value);              /* рычаги ядра (ML_AUDIO_LEAD...); значения политики Разум держит у себя */
} mind_act_t;

typedef struct { int64_t mean_x100; uint64_t var_x100; uint32_t n; } mind_norm_t;

/* То, что переживает перезагрузку (сохраняется как есть, проверяется по crc). */
#define MIND_MAGIC   0x444E494DU   /* "MIND" */
#define MIND_VERSION 5             /* 5: отдельная модель прогноза давления памяти */
#define MIND_MODEL_COMMON \
    uint32_t magic, version; \
    mind_norm_t norm[MIND_SIG_N]; \
    uint8_t  q_best[8];        /* лучший квант (x10 мс) по группам числа ядер; 0 - не выучен */ \
    uint8_t  mode, fx_user, rtc_local, core_cap;   /* rtc_local: часы ПК идут по местному времени (Windows, Limbo/QEMU localtime); \
                                                     core_cap: предел загрузки ядер фоном при нехватке, % (0 - по умолчанию 50) */ \
    int32_t  tz_min;           /* часовой пояс для часов в оболочке, минуты от UTC */ \
    uint32_t lifetime_s, decisions, anomalies, kills, tune_trials, tune_wins;
/* Рычаги 13-30. Номер в таблице = ML_*; на экране и в журнале рычаг имеет номер 13 + ML_*. */
#define ML_N 18
enum { ML_FOCUS_W, ML_STARVE_MS, ML_RISK_THR, ML_PRE_HOLD, ML_RELEASE, ML_TACT_PERIOD, ML_ADMIT, ML_ODD_LIMIT, ML_CORE_RES,
       ML_BG_CALM, ML_BG_STEP, ML_FX_CALM, ML_FX_BAD, ML_AUDIO_LEAD, ML_AUDIO_PERIOD, ML_KFS_RUN, ML_STEAL, ML_LIM_WIN };
typedef struct { uint8_t best, flags; uint16_t trials, wins; } mind_lv_saved_t;   /* лучшее найденное значение (индекс) и счётчики */
#define MF_SEED 1                  /* модель взята из семени опыта (data/mind_seed.bin), а не накоплена на этой машине */
typedef struct {
    MIND_MODEL_COMMON
    mnet_t   net;              /* модель предвидения UI: веса этой машины */
    mnet_t   memory_net;        /* отдельная модель давления памяти, горизонт 4 с */
    uint32_t forecasts;        /* упреждающих решений за жизнь */
    mind_lv_saved_t lv[ML_N];  /* рычаги */
    uint32_t mflags;           /* MF_* */
    uint32_t crc;
} mind_model_t;
/* Сетевой блок формата v3/v2: 12 признаков. Нужен только для безопасной миграции старых файлов. */
typedef struct { int32_t w[12], b; uint32_t n, pos, tp, fp, fn, tn, loss_x1e4; } mindnet_v3_t;
typedef struct { MIND_MODEL_COMMON mindnet_v3_t net; uint32_t forecasts; mind_lv_saved_t lv[ML_N]; uint32_t mflags, crc; } mind_model_v3_t;
typedef struct { MIND_MODEL_COMMON mnet_t net; uint32_t forecasts; mind_lv_saved_t lv[ML_N]; uint32_t mflags, crc; } mind_model_v4_t;
typedef struct { MIND_MODEL_COMMON mindnet_v3_t net; uint32_t forecasts; uint32_t crc; } mind_model_v2_t;
typedef struct { MIND_MODEL_COMMON uint32_t crc; } mind_model_v1_t;
/* Опыт с диска (len байт) -> модель текущего формата. Поддерживаются форматы 1–4. */
int mind_model_load(mind_model_t* out, const void* buf, unsigned len);
/* Семя опыта (файл опыта, полученный на другой машине - в QEMU) -> стартовая модель. Берутся ТОЛЬКО веса модели
   предвидения (число наблюдений урезано до MIND_SEED_N, ниже порога доверия: предупреждения придётся заслужить на
   этой машине) и подсказки по рычагам (счётчики урезаны); нормы устройства, квант, профили, часы - нет: они зависят
   от железа. 1 - годно. */
#define MIND_SEED_N 90
int mind_model_seed(mind_model_t* out, const void* buf, unsigned len);

typedef struct { uint32_t t; uint8_t kind; uint8_t applied; uint32_t pid; char text[MIND_TEXT]; } mind_event_t;

typedef struct {
    uint32_t pid;
    char     name[16];
    uint64_t last_sysc, last_cyc;
    uint32_t seen, cpu_sum_x10, sysc_sum, heap_peak, sysc_rate, cpu_x10;
    uint32_t threads;
    uint32_t weight, limit;
    uint32_t tact_limit;       /* тактический предел доли на поток, % (0 - нет) */
    uint32_t tact_cpu_x10;     /* доля одного ядра за последнее окно тактики, % x10 */
    uint32_t odd_s, calm_s;
    uint8_t  has_profile, flagged, throttled_bg, alive;
    mind_profile_t prof;
} mind_track_t;

/* Состояние рычагов (рабочее). Секундные записи нужны для сравнения «до» и «после» пробы. */
#define ML_RING 32
#define ML_W 8                                 /* окно пробы, секунд */
typedef struct {
    uint32_t late, loops, wait, bg_pm, load, run, under, ops, kb, busy_us, tact, aud, audio_on, lead, errs, denied;
} ml_rec_t;
typedef struct {
    uint8_t idx, dir, fails, rounds, useless, hint;
    uint32_t sig, cool_until, last_try;
    uint16_t trials, wins, backs;              /* за этот запуск */
} ml_one_t;
typedef struct {
    ml_one_t one[ML_N];
    ml_rec_t rec[ML_RING];
    uint32_t rn;                               /* записей всего */
    int      state, lever, from, to;           /* 0 - нет пробы, 1 - проба, 2 - подтверждение */
    uint32_t rn0, t0, next_t, frozen_until, guard_until, guard_late_pm, emerg_t[3], emerg_n;
    int32_t  base_s;
    uint32_t base_load, base_ops, base_late_pm, base_wait, base_under, base_audio;
    uint32_t chg_t[6], chg_n, total_changes, general_rollbacks;
    uint32_t sig; int8_t sigc[8];
    uint32_t seen_fc, seen_tact, seen_core, seen_bg, seen_odd, seen_fxbad, seen_fxlow, prev_fc;
    uint32_t seen_skew;        /* последняя секунда перекоса ядер */
} ml_t;

typedef struct { int32_t x[MNET_F]; uint32_t max_used, created_t; uint8_t valid, acted; } mind_mem_pending_t;

typedef struct {
    mind_model_t m;
    ml_t lv;
    /* сигналы */
    int32_t  cur_x100[MIND_SIG_N];
    int32_t  z_x10[MIND_SIG_N];
    uint8_t  hot[MIND_SIG_N];
    uint32_t last_anom_t[MIND_SIG_N];
    uint8_t  hist[MIND_SIG_N][MIND_HIST];   /* значения, приведённые к 0..100 для графиков */
    uint8_t  health_hist[MIND_HIST];
    uint32_t hpos;
    /* здоровье */
    uint8_t  health, c_resp, c_room, c_bal, c_stab;
    uint8_t  low_health_t;
    const char* mood;
    uint32_t stab_debt;                       /* недавние сбои и необычности (затухают) */
    /* самонастройка кванта */
    int      q_idx, q_base_idx, q_dir, t_state;
    uint32_t t_cool, t_n, t_fails, t_idle;
    uint8_t  started;
    uint64_t t_score, t_base_score;
    uint32_t t_run_sum, t_base_run;
    /* анимация */
    int      fx;
    uint32_t fx_bad_s, fx_calm_s;
    /* память */
    int      mem_level;
    uint32_t last_kill_t;
    /* фон и фокус */
    uint32_t late_s, calm_s, focus_pid;
    /* процессы */
    mind_track_t tr[MIND_PROC_MAX];
    /* журнал */
    mind_event_t j[MIND_JOURNAL];
    uint32_t jn;
    uint32_t activity;                        /* событий за последнюю секунду (оболочка зажигает узлы «нейросети») */
    uint32_t starve_t;
    /* устройства */
    uint32_t dev_last_t, dev_err_s, dev_warned;
    char     dev_last[16];
    uint32_t hid_blocked;
    uint32_t denied_launches;                 /* отклонено запусков за прошлую секунду (заполняет обвязка) */
    /* тактика (десятки мс) */
    uint32_t tact_on, tact_calm_ms, tact_last_ms, tact_logged_ms, tact_steps, tact_actions;
    uint32_t tact_budget_x10;                 /* сколько процессора отдано фону при последнем ограничении */
    uint32_t tact_rel_ms, tact_back_ms;       /* время последнего шага снятия пределов; время последнего удлинения выдержки */
    uint8_t  tact_backoff;                    /* 0..MIND_REL_BACKOFF_MAX: выдержка перед снятием удлиняется вдвое за каждый возврат давления сразу после снятия */
    uint32_t bg_fps_level, bg_fps_ms, bg_calm_ms;
    uint32_t core_res_pid, core_calm_ms;                  /* ядро отдано программе в фокусе (0 - нет) и время спокойствия */         /* темп фоновых окон: ступень (0 - без предела) и время смены */
    /* предвидение (стратегия): признаки прошлой секунды ждут своего исхода */
    int32_t  nx_prev[MNET_F];
    uint8_t  nx_have, nx_acted;               /* есть пример; в его секунду действовало упреждение (исход не честный) */
    uint32_t risk_q16;                        /* вероятность опоздания экрана в следующую секунду */
    uint32_t mem_risk_q16;                     /* вероятность достижения 90% ОЗУ в следующие 4 секунды */
    mind_mem_pending_t mem_pending[5];
    uint32_t mem_pending_pos, mem_forecasts, mem_logged_t, mem_prev_used, mem_prev_t;
    uint8_t mem_prev_valid;
    uint32_t pre_until_ms;                    /* до этого момента тактика держит фон ужатым заранее */
    uint32_t pre_logged_t, pre_skip;          /* журнал не чаще; каждая 10-я опасная секунда - без упреждения (проверка) */
    int      risk_why;                        /* главный признак риска */
    char     denied_name[16];
    /* перекос ядер: норма занятости каждого ядра (скользящая, на запуск) и порог перегрузки именно этого ядра */
    int32_t  core_mean_x100[256], core_var_x100[256];
    uint16_t core_n[256];
    uint8_t  core_thr[256];                   /* порог перегрузки ядра, % (для экрана и тестов) */
    uint32_t skew_s, skew_hot, skew_cold;     /* секунд подряд с перекосом; ядер выше порога и простаивающих */
    uint32_t skew_core, skew_logged_t;        /* самое перегруженное ядро; когда последний раз писали в журнал */
    /* стабилизация: что тактика делала за запуск (показывается в `ai`, чтобы настраивать по реальным данным, а не на глаз) */
    uint32_t stab_episodes;                   /* сколько раз фон ужимался (переход «пределов нет» -> «пределы есть») */
    uint32_t stab_limited_ms;                 /* суммарно фон был ужат, мс */
    uint32_t stab_relapses;                   /* давление вернулось вскоре после снятия предела (качание) */
    uint32_t stab_longest_ms, stab_cur_ms;    /* самый долгий и текущий период с пределами */
    uint32_t tact_cost_x100;                  /* цена тактики за последнюю секунду: доля одного ядра, % x100 */
} mind_t;

void mind_init(mind_t* s, const mind_model_t* saved, unsigned cores);   /* saved - с диска или 0 */
void mind_step(mind_t* s, const mind_sample_t* in, const mind_act_t* act);

/* Допуск запусков: сколько копий одной программы и сколько запусков в секунду система выдержит сейчас.
   Зависит от числа ядер и здоровья: при тревоге копий меньше и запуски реже. Доверенная программа - не значит
   безлимитная: спам запусками одной программы останавливается здесь. */
typedef struct { uint32_t max_same, rate_same, rate_total; } mind_launch_t;

/* ---- тактика: каждые ~25 мс ----
   Быстрый контур поверх секундного Разума: если экран опаздывает или системные потоки и программа в фокусе ждут
   процессор, фоновые программы ужимаются так, чтобы фон занимал не больше core_cap% всех ядер (предел загрузки
   ядер); нет давления ~0,4 с - пределы плавно отпускаются. Когда ждать некому, фон работает на всех ядрах. */
#define MIND_TACT_MAXP 256
typedef struct {
    uint32_t t_ms, window_ms;
    uint32_t cores;
    uint32_t busy_x10;         /* занятость всех ядер за окно, % x10 (сумма по ядрам, до cores*1000) */
    uint32_t loops, late;      /* циклы дисплея за окно и опоздавшие */
    uint32_t fg_wait_ms;       /* дольше всех ждущий готовый поток ядра или программы в фокусе */
    uint32_t nproc;
    struct { uint32_t pid, threads, cpu_x10; } p[MIND_TACT_MAXP];   /* cpu_x10 - доля одного ядра за окно */
} mind_tact_in_t;
void mind_tact_step(mind_t* s, const mind_tact_in_t* in, const mind_act_t* act);
unsigned mind_core_cap(const mind_t* s);
unsigned mind_bg_fps(const mind_t* s);
uint32_t mind_core_reserved_pid(const mind_t* s);    /* программа, которой отдано ядро (0 - ядро не отдано) */       /* текущий предел кадров фоновых окон (0 - нет) */
void mind_launch_limits(const mind_t* s, unsigned cores, mind_launch_t* out);
void mind_set_mode(mind_t* s, int mode, const mind_act_t* act);

/* ---- рычаги 13-30 ---- */
int32_t     mind_lever_val(const mind_t* s, int lever);        /* текущее значение */
const char* mind_lever_name(int lever);
const char* mind_lever_unit(int lever);
int32_t     mind_lever_default(int lever);
/* Состояние для вывода: 0 - обычное значение, 1 - значение изменено (оставлено по замеру), 2 - идёт проба,
   3 - «не помогает» (не трогаю до смены нагрузки), 4 - остыл/ждёт. */
int         mind_lever_state(const mind_t* s, int lever);
void        mind_lever_stats(const mind_t* s, int lever, unsigned* trials, unsigned* wins, unsigned* backs);
void        mind_levers_reset(mind_t* s, const mind_act_t* act);
void mind_set_fx_user(mind_t* s, int fx, const mind_act_t* act);

int  mind_model_valid(const mind_model_t* m);
void mind_model_seal(mind_model_t* m);       /* посчитать crc перед записью */

const char* mind_signal_name(int sig);
const char* mind_feature_name(int f);       /* признак модели предвидения */
uint32_t mind_memory_risk(const mind_t* s);   /* вероятность давления памяти в следующие 4 с, Q16; без доверия — 0 */
void mind_make_features(const mind_t* s, const mind_sample_t* in, int32_t* out); /* Q16 признаки для прогноза/тестов */
const char* mind_signal_unit(int sig);
unsigned    mind_quantum_ms(const mind_t* s);
int         mind_fx_level(const mind_t* s);
/* Журнал: i = 0 - самое свежее. 0 - нет такой записи. */
const mind_event_t* mind_journal(const mind_t* s, unsigned i);

#endif
