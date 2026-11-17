#ifndef LATENCY_H
#define LATENCY_H

#include <stdint.h>

/* Измеряемое время реакции (А8, docs/LATENCY.md). Чистая часть: логарифмические гистограммы задержек в наносекундах по ядрам, максимумы,
   пороги и счётчик превышений, слияние ядер, сброс, перевод тактов в нс. Зависит только от <stdint.h>: проверяется хост-тестом
   tests/latency_test.c. Подключение к ядру (замеры в прерывании, планировщике и критических секциях, команда latency) - latency_k.c.

   Запись делает только своё ядро (без замков и атомарных RMW: чтение-запись релаксированные), читает любое: гистограмма может оказаться
   «живой», но не рваной. Сброс не ходит по чужим ядрам: он поднимает общую эпоху, а каждое ядро при следующей записи обнуляет себя
   само (читатель пропускает ядра прежней эпохи). */

#define LAT_BINS 96     /* 4 корзины на степень двойки с 16 нс: корзина 0 - до 16 нс, последняя - от ~201 мс и дольше (максимум хранится точно) */

typedef enum {
    LAT_IRQ = 0,        /* задержка прерывания: от срока срабатывания таймера LAPIC до входа в обработчик */
    LAT_ISR,            /* длительность обработчика аппаратного прерывания (вход - до EOI) */
    LAT_IRQOFF,         /* время с выключенными прерываниями в критической секции (irq_save/irq_restore, спинлоки с irq); под флагом */
    LAT_SPIN,           /* ожидание замка (билетный спинлок с irq, замки планировщика): сколько ядро крутится, ничего не делая; место = замок */
    LAT_SCHED_NORMAL,   /* от «поток стал готовым» (пробуждение) до запуска на процессоре: класс normal */
    LAT_SCHED_CRIT,     /* то же, класс critical (системные потоки и интерактивные: вытесняют обычные сразу) */
    LAT_SCHED_RT,       /* то же, класс realtime */
    LAT_TICK,           /* части обработчика тика таймера (место = LAT_PART_*): из чего складывается время обработчика */
    LAT_KINDS
} lat_kind_t;

/* Места самых долгих замеров: «кто виноват». Небольшая таблица на ядро и вид; в неё попадают только замеры не короче LAT_SITE_MIN_NS. */
#define LAT_SITES 8
enum { LAT_PART_CLOCK = 1, LAT_PART_UART, LAT_PART_FLUSH, LAT_PART_WATCHDOG, LAT_PART_SCHED, LAT_PART_N };   /* части обработчика тика (LAT_TICK) */
#define LAT_SITE_MIN_NS 100000ULL
typedef struct { uint64_t site, count, sum_ns, max_ns; } lat_site_t;

typedef struct {
    uint64_t count;     /* замеров */
    uint64_t sum_ns;    /* сумма (для среднего) */
    uint64_t max_ns;    /* наибольшая задержка */
    uint64_t max_site;  /* где она случилась (смысл зависит от вида: адрес команды, номер потока, вектор) */
    uint64_t over;      /* сколько замеров превысили порог */
    uint32_t bin[LAT_BINS];
} lat_hist_t;

/* Корзина значения и верхняя граница корзины (нс): значение v лежит в корзине b, если lat_bin_lo(b) <= v < lat_bin_hi(b). */
unsigned lat_bin(uint64_t ns);
uint64_t lat_bin_lo(unsigned bin);
uint64_t lat_bin_hi(unsigned bin);         /* у последней корзины - UINT64_MAX */

void lat_hist_clear(lat_hist_t* h);
/* Учесть замер; limit_ns == 0 - порога нет. 1 - порог превышен. */
int lat_hist_add(lat_hist_t* h, uint64_t ns, uint64_t site, uint64_t limit_ns);
void lat_hist_merge(lat_hist_t* dst, const lat_hist_t* src);
/* Квантиль (ppm = 990000 - p99, 999000 - p99.9): верхняя граница корзины, в которую попал заданный ранг, но не больше максимума. 0 - замеров нет.
   Оценка «сверху»: настоящее значение квантиля не выше возвращённого. */
uint64_t lat_hist_quantile(const lat_hist_t* h, uint32_t ppm);

/* Перевод тактов в нс умножением (без деления в горячем пути): ns = cycles * mul >> 32, mul = 1e6 * 2^32 / кГц. */
uint64_t lat_mul_for_khz(uint64_t tsc_khz);
uint64_t lat_cycles_to_ns(uint64_t cycles, uint64_t mul);

const char* lat_kind_name(unsigned kind);
int lat_kind_from_name(const char* name);   /* -1 - нет такого вида */

/* По ядрам. */
typedef struct {
    uint32_t epoch;
    lat_hist_t h[LAT_KINDS];
    lat_site_t sites[LAT_KINDS][LAT_SITES];
    uint64_t pend_ns[LAT_KINDS];     /* наибольшее превышение порога с прошлой выдачи (0 - не было): его пишут в чёрный ящик вне горячего пути */
    uint64_t pend_site[LAT_KINDS];
    uint32_t pend_n[LAT_KINDS];      /* сколько превышений накопилось с прошлой выдачи */
} lat_cpu_t;

typedef struct {
    lat_cpu_t* cpu;
    unsigned ncpu;
    volatile uint32_t epoch;         /* поднимается сбросом */
    uint64_t limit[LAT_KINDS];       /* пороги, нс; 0 - нет */
    uint64_t dropped;                /* замеров с номером ядра за пределами таблицы */
} lat_set_t;

void lat_set_init(lat_set_t* s, lat_cpu_t* mem, unsigned ncpu);
/* Запись замера на ядре cpu (вызывает только это ядро). 1 - порог превышен. */
int lat_set_record(lat_set_t* s, unsigned cpu, unsigned kind, uint64_t ns, uint64_t site);
void lat_set_reset(lat_set_t* s);
int lat_set_limit(lat_set_t* s, unsigned kind, uint64_t ns);
/* Сумма гистограмм всех ядер этой эпохи по виду. */
void lat_set_merge(const lat_set_t* s, unsigned kind, lat_hist_t* out);
/* Копия гистограммы одного ядра (пусто, если ядро прежней эпохи). */
int lat_set_cpu_hist(const lat_set_t* s, unsigned cpu, unsigned kind, lat_hist_t* out);
/* Места: слить таблицы всех ядер по виду в out[LAT_SITES]: одинаковые места складываются, остаётся LAT_SITES самых долгих по максимуму (по убыванию). */
unsigned lat_set_sites(const lat_set_t* s, unsigned kind, lat_site_t* out);
/* Забрать накопленное превышение (вызывает то же ядро). 1 - было. */
int lat_set_take_pending(lat_set_t* s, unsigned cpu, unsigned kind, uint64_t* ns, uint64_t* site, uint32_t* n);

/* ---- подключение к ядру (latency_k.c) ---- */
#ifdef KIRA_HOST_TEST
/* В хост-тестах другого кода (планировщик и т.п.) замеров нет: пустые заглушки. */
#define lat_irqoff_on 0
static inline void lat_irqoff_cut(void) {}
static inline void lat_irq_entry(uint64_t rip) { (void)rip; }
static inline void lat_isr_note(uint64_t t, uint64_t v) { (void)t; (void)v; }
static inline void lat_sched_note(unsigned p, uint64_t t, uint32_t id) { (void)p; (void)t; (void)id; }
static inline void lat_flush(void) {}
static inline void lat_inject(void) {}
static inline void lat_spin_note(uint64_t t, uint64_t s) { (void)t; (void)s; }
static inline void lat_part(unsigned p, uint64_t t) { (void)p; (void)t; }
static inline void lat_timer_restart(void) {}
#else
extern volatile int lat_irqoff_on;     /* флаг замера критических секций: ноль - стоимость в irq_save/irq_restore одна проверка (объявлено и в cpu.h) */
void lat_irqoff_begin(uint64_t flags); /* из irq_save, только когда флаг поднят; flags - прежнее состояние */
void lat_irqoff_end(uint64_t flags);   /* из irq_restore, только когда флаг поднят */
void lat_irqoff_cut(void);             /* перед переключением потока: открытая секция закрывается здесь */

void lat_init(unsigned cpus, uint64_t tsc_khz);     /* после калибровки TSC и таймера; до этого замеры молча не ведутся */
void lat_irq_entry(uint64_t interrupted_rip);       /* первая строка обработчика прерывания таймера: сколько прошло с его срока */
void lat_isr_note(uint64_t tsc_start, uint64_t vector);   /* после обработчика аппаратного прерывания */
void lat_sched_note(unsigned prio, uint64_t ready_tsc, uint32_t tid);  /* поток, ставший готовым в ready_tsc, получил процессор */
void lat_part(unsigned part, uint64_t tsc_start);       /* часть обработчика тика закончилась: сколько она длилась */
void lat_spin_note(uint64_t tsc_start, uint64_t site);   /* ядро ждало замок с tsc_start */
void lat_inject(void);                              /* проверка границ: искусственная задержка в обработчике тика (latency inject), по умолчанию выключена */
void lat_flush(void);                               /* из тика: накопленные превышения порогов - в чёрный ящик */
void lat_command(const char* arg);                  /* консоль: latency [short|hist | reset | limit ИМЯ нс | irqoff on|off | bench] */

/* Архитектура: сколько наносекунд прошло с последнего срока срабатывания таймера до сейчас (по счётчику самого таймера) и каков период.
   Возврат: 0 - не поддержано; 1 - периодический таймер, сроки на сетке (x86-64, LAPIC: слипшиеся сроки ядро считает само по запомненному последнему);
   2 - каждый срок известен точно (ARM64: CNTV_CVAL хранит сработавший срок, сетки нет). */
int lat_arch_timer_elapsed_ns(uint64_t* elapsed_ns, uint64_t* period_ns);
void lat_timer_restart(void);          /* таймер этого ядра остановлен или перезапущен: сетка сроков началась заново */
#endif

#endif
