#ifndef MIND_SYS_H
#define MIND_SYS_H

#include <stdint.h>
#include "mind.h"

/* Разум в ядре: раз в секунду (основной поток core) собирает замеры со всей системы, вызывает mind_step,
   исполняет решения (планировщик, память, анимация), печатает новые записи журнала, хранит модель в /var/mind.dat
   и профили программ в /var/mind/<имя>. Оболочка читает готовый снимок (mind_view_t) без ожидания. */

#define MIND_VIEW_J 8

typedef struct {
    uint32_t t, lifetime_s;
    uint8_t  health, c_resp, c_room, c_bal, c_stab;
    char     mood[32];
    uint8_t  mode, fx, fx_user;
    uint32_t quantum_ms;
    uint32_t cores, load, mem_used, runnable;
    uint8_t  busy[64];
    uint32_t frames_per_s, late_per_s;
    int32_t  cur_x100[MIND_SIG_N], mean_x100[MIND_SIG_N], z_x10[MIND_SIG_N];
    uint32_t learned[MIND_SIG_N];
    uint8_t  hist[MIND_SIG_N][MIND_HIST];
    uint8_t  health_hist[MIND_HIST];
    uint32_t hpos;
    uint32_t decisions, anomalies, kills, tune_trials, tune_wins;
    uint32_t activity;
    uint32_t nj;
    uint32_t journal_seq;              /* монотонный номер записи: GUI замечает события даже в одну секунду */
    struct { uint32_t t; uint8_t kind, applied; char text[MIND_TEXT]; } j[MIND_VIEW_J];
    int32_t  tz_min;
    uint8_t  rtc_local;
    uint32_t nproc;
    struct { uint32_t pid, threads, heap_kb, cpu_x10, sysc, weight, limit; char name[16]; uint8_t flagged, focus; } p[16];
    /* предвидение */
    uint32_t risk;                         /* % */
    int      risk_why;
    uint32_t net_n, net_pos, net_prec, net_rec, forecasts, net_loss_x1e4;
    uint8_t  net_trusted;
    int32_t  net_w[MNET_F];
    uint32_t bg_fps, core_cap;
    uint32_t core_res_pid; int core_res;   /* ядро фокуса: кому отдано (0 - никому) и какое (-1 - нет) */             /* тактика: предел кадров фоновых окон (0 - нет), предел загрузки ядер фоном */
    uint32_t comp_par, comp_ns_kpx_par, comp_ns_kpx_ser;   /* композитор: параллельно ли сейчас и цена 1000 точек */
    /* рычаги 13-30 */
    struct { int32_t val, def; uint16_t trials, wins, backs; uint8_t state; } lv[ML_N];
    uint32_t lv_changes, lv_general, lv_seed;
} mind_view_t;

void mind_sys_init(void);                 /* после файловой системы */
void mind_sys_tick(uint64_t seconds);     /* раз в секунду из основного потока */
void mind_sys_save(void);                 /* модель на диск сейчас */
void mind_sys_view(mind_view_t* out);     /* снимок для оболочки (из любого потока) */
uint32_t mind_sys_version(void);          /* растёт после каждого шага: оболочке есть что перерисовать */

/* Команды консоли и настройки оболочки (выполняются в основном потоке ядра при следующем шаге). */
void mind_sys_request_mode(int mode);
void mind_sys_request_fx(int fx_user);
void mind_sys_request_tz(int32_t minutes);
void mind_sys_set_telemetry(int on);
int  mind_sys_telemetry(void);
void mind_sys_print(const char* what);    /* ai, ai why, ai norms, ai procs */
int32_t mind_sys_tz(void);
/* Часы ПК (CMOS) идут по UTC (Linux, QEMU по умолчанию) или по местному времени (Windows, Limbo). Отсюда - сколько
   минут прибавить к времени часов, чтобы получить местное: пояс или 0. */
void mind_sys_request_rtc_local(int on);
/* Допуск запуска программы (вызывает загрузчик). same - сколько копий уже работает. 1 - можно; 0 - нет,
   причина в why. Пределы - mind_launch_limits по текущему здоровью и числу ядер. */
int  mind_sys_admit(const char* name, unsigned same, char* why, unsigned cap);
/* Предел загрузки ядер фоном, когда системе или окну в фокусе не хватает процессора (10..100 %). */
void mind_sys_request_cap(int pct);
unsigned mind_sys_core_cap(void);
int  mind_sys_rtc_local(void);
int32_t mind_sys_local_offset(void);

#endif
