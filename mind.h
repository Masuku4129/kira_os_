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

enum { MS_LOAD, MS_QUEUE, MS_MEM, MS_LATE, MS_DISK, MS_IPC, MS_FAULT, MS_SYSC };
enum { MIND_OBSERVE = 0, MIND_ACT = 1 };
enum { MIND_FX_OFF = 0, MIND_FX_LOW = 1, MIND_FX_MID = 2, MIND_FX_HIGH = 3, MIND_FX_AUTO = 4 };

typedef enum {
    MJ_LEARN = 1, MJ_ANOMALY, MJ_FOCUS, MJ_LIMIT, MJ_RESTORE, MJ_TUNE_TRY, MJ_TUNE_KEEP, MJ_TUNE_BACK,
    MJ_FX, MJ_MEMORY, MJ_KILL, MJ_STARVE, MJ_BEHAVIOR, MJ_INFO
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
} mind_act_t;

typedef struct { int64_t mean_x100; uint64_t var_x100; uint32_t n; } mind_norm_t;

/* То, что переживает перезагрузку (сохраняется как есть, проверяется по crc). */
#define MIND_MAGIC   0x444E494DU   /* "MIND" */
#define MIND_VERSION 2             /* 2: модель предвидения (веса) */
#define MIND_MODEL_COMMON \
    uint32_t magic, version; \
    mind_norm_t norm[MIND_SIG_N]; \
    uint8_t  q_best[8];        /* лучший квант (x10 мс) по группам числа ядер; 0 - не выучен */ \
    uint8_t  mode, fx_user, rtc_local, core_cap;   /* rtc_local: часы ПК идут по местному времени (Windows, Limbo/QEMU localtime); \
                                                     core_cap: предел загрузки ядер фоном при нехватке, % (0 - по умолчанию 50) */ \
    int32_t  tz_min;           /* часовой пояс для часов в оболочке, минуты от UTC */ \
    uint32_t lifetime_s, decisions, anomalies, kills, tune_trials, tune_wins;
typedef struct {
    MIND_MODEL_COMMON
    mnet_t   net;              /* модель предвидения: веса этой машины */
    uint32_t forecasts;        /* упреждающих решений за жизнь */
    uint32_t crc;
} mind_model_t;
typedef struct { MIND_MODEL_COMMON uint32_t crc; } mind_model_v1_t;   /* прежний формат: читается и дополняется */
/* Опыт с диска (len байт) -> модель текущего формата. 1 - годен (формат 2 или поднятый из 1), 0 - нет. */
int mind_model_load(mind_model_t* out, const void* buf, unsigned len);

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

typedef struct {
    mind_model_t m;
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
    /* предвидение (стратегия): признаки прошлой секунды ждут своего исхода */
    int32_t  nx_prev[MNET_F];
    uint8_t  nx_have, nx_acted;               /* есть пример; в его секунду действовало упреждение (исход не честный) */
    uint32_t risk_q16;                        /* вероятность опоздания экрана в следующую секунду */
    uint32_t pre_until_ms;                    /* до этого момента тактика держит фон ужатым заранее */
    uint32_t pre_logged_t, pre_skip;          /* журнал не чаще; каждая 10-я опасная секунда - без упреждения (проверка) */
    int      risk_why;                        /* главный признак риска */
    char     denied_name[16];
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
void mind_launch_limits(const mind_t* s, unsigned cores, mind_launch_t* out);
void mind_set_mode(mind_t* s, int mode, const mind_act_t* act);
void mind_set_fx_user(mind_t* s, int fx, const mind_act_t* act);

int  mind_model_valid(const mind_model_t* m);
void mind_model_seal(mind_model_t* m);       /* посчитать crc перед записью */

const char* mind_signal_name(int sig);
const char* mind_feature_name(int f);       /* признак модели предвидения */
const char* mind_signal_unit(int sig);
unsigned    mind_quantum_ms(const mind_t* s);
int         mind_fx_level(const mind_t* s);
/* Журнал: i = 0 - самое свежее. 0 - нет такой записи. */
const mind_event_t* mind_journal(const mind_t* s, unsigned i);

#endif
