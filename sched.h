#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "cpumask.h"
#include "fpu.h"
#include "spinlock.h"

#define SCHED_STACK_SIZE  16384
#define SCHED_NAME_LEN    16

typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_ZOMBIE,     // завершён, стек ещё не освобождён (освобождает sched_reap)
    THREAD_BLOCKED     // ждёт события (мьютекс): проснётся только от sched_wake
} thread_state_t;

typedef enum {
    PRIO_IDLE = 0,
    PRIO_NORMAL = 1,
    PRIO_CRITICAL = 2, // системные потоки ядра: вытесняют обычные сразу
    PRIO_REALTIME = 3  // жёсткий реальный режим: не уступает даже системным потокам (опасно)
} thread_prio_t;

#define THREAD_F_SERVICE 1u            // поток, без которого система не живёт и не перезапускается (core): сбой в нём - авария
#define THREAD_F_IDLE    2u
#define THREAD_F_OFFCORE 4u            // Разум убрал поток программы с ядра, отданного окну в фокусе (sched_reserve_core)            // холостой поток ядра: в очереди готовых не стоит, выбирается, когда работы нет

typedef struct thread {
    uint64_t rsp;                  // сохранённый указатель стека (должен быть первым полем)
    uint32_t id;
    char name[SCHED_NAME_LEN];
    thread_state_t state;
    thread_prio_t prio;
    uint64_t wake_tick;            // для THREAD_SLEEPING
    uint32_t sleep_index;           // позиция в min-heap сроков пробуждения; UINT32_MAX - не спит по таймеру
    uint32_t quantum_left;         // оставшиеся тики кванта
    uint64_t cpu_cycles;           // накопленное время на CPU (такты TSC)
    uint64_t switches_in;          // сколько раз поток получал процессор
    uint8_t* stack_base;           // 0 для ядра-загрузчика (использует boot-стек)
    uint64_t kstack_top;           // вершина kernel-стека (TSS.rsp0 и стек SYSCALL); 0 - boot-стек
    uint64_t cr3;                  // корень таблиц процесса; 0 - адресное пространство ядра
    void* proc;                    // владелец-процесс (proc_t*) или 0 для потоков ядра
    void (*entry)(void*);
    void* arg;
    cpumask_t affinity;            // на каких ядрах можно исполнять (бит n - ядро n); по умолчанию любое
    uint32_t last_cpu;             // ядро, на котором поток исполнялся последний раз
    struct thread* wait_next;      // следующий в очереди ожидания мьютекса
    volatile uint32_t on_cpu;      // 1, пока поток исполняется на каком-то ядре или ещё не сохранил свой стек
    uint32_t flags;                // THREAD_F_*
    uint32_t mutexes_held;         // сколько мьютексов ядра держит поток (сбой с мьютексом изолировать нельзя: ожидающие встали бы навсегда)
    uint32_t pid;                  // процесс-владелец (0 - поток ядра); для ps
    uint32_t weight;               // доля кванта в четвертях (4 = обычный квант, 1..8): её меняет Разум
    uint64_t ready_since;          // тик, с которого поток готов, но не исполняется (поиск голодающих)
    uint32_t limit;                // доля одного ядра, % (0 или 100 - без ограничения): её задаёт Разум
    uint32_t lim_used;             // тиков исполнения в текущем окне ограничения
    uint64_t lim_start;            // начало окна
    uint64_t throttled_until;      // исчерпал долю: спит до этого тика
    struct thread* rq_next;        // очередь готовых своего ядра (по классу приоритета)
    struct thread* rq_prev;
    int32_t rq_cpu;                // в чьей очереди стоит (-1 - ни в чьей)
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(16))); // состояние x87/SSE потока (FXSAVE)
} thread_t;

typedef struct {
    uint32_t id;
    char name[SCHED_NAME_LEN];
    thread_state_t state;
    thread_prio_t prio;
    uint64_t cpu_cycles;
    uint64_t switches_in;
    uint32_t cpu;                  // последнее ядро потока
    uint32_t pid;                  // процесс (0 - ядро)
    uint32_t weight;
    uint64_t waiting;              // тиков в готовности без процессора (0 - не ждёт)
} thread_info_t;

// Превращает текущий контекст в поток "core" и создаёт idle-поток.
// Вызывать после heap_init() и до включения прерываний.
// Ёмкость таблицы потоков выводится из числа ядер и ОЗУ (scale_threads).
void sched_init(unsigned cpus, uint64_t ram_kb);
unsigned sched_thread_capacity(void);

// Создаёт поток. Возвращает его или 0 (нет слотов/памяти).
thread_t* sched_create(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio);

// Ядра равнозначны: поток по умолчанию может исполняться на любом ядре. У каждого ядра своя очередь готовых;
// готовый поток ставится на свободное или наименее занятое разрешённое ядро, простаивающее ядро забирает работу
// у занятых, а более важная работа на чужой очереди забирается сразу. Маска нужна тем, кому нужно конкретное ядро
// (проверки, холостые потоки ядер).
#define SCHED_AFFINITY_BSP cm_one(0)
#define SCHED_AFFINITY_ALL cm_all()
thread_t* sched_create_on(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio, cpumask_t affinity);

// Включает не загрузочное ядро в планирование: его текущий контекст становится холостым потоком этого ядра.
// Вызывается самим ядром после полной инициализации; возвращает 0 при успехе.
int sched_attach_cpu(void);
// Учёт времени ядра: такты в холостом потоке и всего с момента подключения; число переключений.
int sched_cpu_stats(unsigned cpu, uint64_t* idle_cycles, uint64_t* total_cycles, uint64_t* switches);

// То же, но поток принадлежит процессу: у него своё адресное пространство (cr3).
thread_t* sched_create_proc(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio,
                            uint64_t cr3, void* proc);
// Вызывается из потока процесса при завершении: дальше он живёт в адресном пространстве ядра.
void sched_detach_space(void);

void sched_yield(void);

// Блокировка до события. Вызывается с выключенными прерываниями и взятым замком l: замок отпускается уже после
// того, как поток помечен ожидающим, поэтому пробуждение не может потеряться. Возвращается после sched_wake.
void sched_block_and_unlock(spinlock_t* l);
// Вес процесса (все его потоки): квант = базовый * weight / 4, weight 1..8. Больше вес - больше доля процессора,
// голодания нет: очередь остаётся круговой. Возвращает число затронутых потоков.
#define SCHED_WEIGHT_NORMAL 4
unsigned sched_set_proc_weight(uint32_t pid, uint32_t weight);
// Ограничение доли процессора для потоков процесса: в каждом окне SCHED_LIMIT_WINDOW тиков поток исполняется не больше
// pct процентов, остальное время спит (ядро свободно для других и, на эмуляторе, для машины-хозяина). 100 - снять.
#define SCHED_LIMIT_WINDOW 50           /* окно по умолчанию; Разум меняет через sched_set_limit_window */
void sched_set_limit_window(uint32_t ticks);   /* 10..200 тиков */
void sched_set_steal_bias(uint32_t quanta);     /* во сколько квантов чужой поток должен ждать дольше своего, чтобы ядро взяло его (1..16; по умолчанию 2) */
unsigned sched_set_proc_limit(uint32_t pid, uint32_t pct);
/* Рычаг Разума: ядро core - только программе keep_pid (окно в фокусе) и потокам ядра; потоки остальных программ
   уходят с него (стоящие в его очереди - в другие, идущий на нём - вытесняется). core < 0 - вернуть всё как было.
   Повторный вызов с тем же core подхватывает новые потоки. Возвращает число перенесённых потоков. */
unsigned sched_reserve_core(int core, uint32_t keep_pid);
int      sched_reserved_core(void);          /* -1 - ядро не отдано */
// Сколько потоков (кроме холостых) сейчас готовы или исполняются.
unsigned sched_runnable(void);

// Ожидание события с ограничением по времени (порты, futex). Вызывать с взятым спин-замком l (прерывания уже
// выключены им): поток засыпает, l отпускается уже после пометки «спит», поэтому пробуждение, сделанное под тем же
// замком, не теряется. ticks = ~0 - без срока. Возвращается с отпущенным l; что случилось - проверяет вызывающий.
void sched_wait_and_unlock(spinlock_t* l, uint64_t ticks);
// Разбудить поток, ждущий в sched_wait_and_unlock (на других состояниях не действует). Вызывать под тем же замком.
void sched_wake_waiter(thread_t* t);
// Разбудить все спящие потоки процесса (процесс завершается: они увидят это при выходе из ядра).
void sched_wake_proc(void* proc);

// Новый срок пробуждения спящего потока (раньше или позже прежнего); на потоки в других состояниях не действует.
void sched_set_wake_tick(thread_t* t, uint64_t tick);
void sched_wake(thread_t* t);
unsigned sched_cpu_count(void);
/* Общий замок планировщика на ядре cpu: такты ожидания, взятий с ожиданием, всего взятий (с запуска). */
void sched_lock_stats(unsigned cpu, uint64_t* wait_cycles, uint64_t* contended, uint64_t* taken);          // ядер, участвующих в планировании
int sched_thread_alive(uint32_t id);     // поток с таким id существует и не завершён
// Можно ли сейчас уснуть: планировщик работает, прерывания включены, не обработчик прерывания, спин-замков нет.
int sched_can_sleep(void);
void sched_crash_report(void (*out)(const char*));
void sched_crash_wait_halted(void);
unsigned sched_trace_current(uint64_t rbp, uint64_t* out, unsigned max);   // цепочка вызовов прерванного кода ядра                  // после аварийного NMI: дождаться, пока ядра запишут, где стояли   // аварийный отчёт: что на каждом ядре, где поток core

// Отказ ядра в режиме ядра (вызывается уже на запасном стеке этого ядра, прерывания выключены): текущий поток
// погибает, ядро выходит из планирования, потоки, которым разрешено только это ядро, гибнут вместе с ним.
// Возвращает число таких потоков. Остальные ядра продолжают работу.
unsigned sched_core_fail(void);
void sched_sleep_ticks(uint32_t ticks);
void sched_exit(void) __attribute__((noreturn));

// Освобождает стеки завершённых потоков. Возвращает сколько освобождено.
unsigned sched_reap(void);

// Вызывается из обработчика таймера (контекст прерывания, IF=0).
void sched_tick(void);

// Вызывается после EOI в конце обработки любого IRQ: здесь происходит вытеснение.
void sched_irq_exit(void);

// Квант (в тиках) для обычных потоков; задаётся из состояния организма.
void sched_set_quantum(uint32_t ticks);
uint32_t sched_get_quantum(void);

// Частота TSC в кГц (для перевода тактов во время); 0, если неизвестна.
void sched_set_tsc_khz(uint64_t khz);

// Статистика
uint64_t sched_idle_cycles(void);        // такты, проведённые в idle-потоке
uint64_t sched_total_switches(void);
unsigned sched_snapshot(thread_info_t* out, unsigned max); // копия таблицы потоков
const thread_t* sched_current(void);

void sched_dump(void);                   // печать таблицы потоков в UART

// Реализовано на ассемблере (interrupts.s)
void context_switch(uint64_t* old_rsp_slot, uint64_t new_rsp);

#endif
