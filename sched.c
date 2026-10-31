#include "sched.h"
#include "cpu.h"
#include "percpu.h"
#include "fpu.h"
#include "heap.h"
#include "spinlock.h"
#include "timer.h"
#include "uart.h"
#include "scale.h"
#include "arch_sched.h"
#ifndef KIRA_HOST_TEST
#include "kstack.h"
#endif

#define STACK_CANARY 0x4B49524153544B21ULL // "KIRASTK!"
#define DEFAULT_QUANTUM (4 * TIMER_PER_10MS)      /* 40 мс при любой частоте тиков */
#define MAX_QUANTUM (20 * TIMER_PER_10MS)

_Static_assert(__builtin_offsetof(thread_t, fpu) % 16 == 0, "FXSAVE area must be 16-byte aligned");
_Static_assert(_Alignof(thread_t) >= 16, "thread_t array must be 16-byte aligned");

/* Таблица потоков выделяется при старте планировщика по найденному железу (scale_threads): число ядер и ОЗУ.
   thread_hi - число слотов, которые когда-либо использовались: обходы таблицы идут только по ним, а не по всей
   ёмкости, поэтому цена переключения зависит от числа живых потоков. */
static thread_t* threads;
static unsigned thread_cap;
static unsigned thread_hi;
/* Текущий и холостой потоки лежат в данных процессора: каждое ядро видит своих. */
/* Замок очереди готовности и таблицы потоков. Удерживается на время выбора и переключения: его отпускает уже
   новый поток (finish_switch), поэтому прежний поток не может стать готовым и попасть на другое ядро, пока
   его стек ещё в работе. Прерывания на это время выключены. */
static spinlock_t sched_lock = SPINLOCK_INIT;
static int sched_ready;
static uint32_t quantum_ticks = DEFAULT_QUANTUM;
static uint64_t total_switches;
static uint32_t next_id = 1;
static uint64_t tsc_khz;
/* Дешёвый тик: спящих просматривают, только когда наступил ближайший срок пробуждения (next_wake), а холостое ядро
   ищет работу, только если с прошлого просмотра какой-то поток стал готовым или освободился (ready_epoch).
   Оба значения меняются под sched_lock. */
static uint64_t next_wake = ~0ULL;
static uint64_t ready_epoch = 1;

extern void kernel_panic(const char* reason) __attribute__((noreturn));

static void rq_remove(struct thread* t);

// Свободный слот таблицы (вызывать под sched_lock); занятый слот расширяет границу обходов.
static thread_t* take_slot(void) {
    for (unsigned i = 0; i < thread_cap; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            if (i + 1 > thread_hi) thread_hi = i + 1;
            rq_remove(&threads[i]);         /* свободный слот в очереди не стоит; страховка от чужой правки состояния */
            return &threads[i];
        }
    }
    return 0;
}

unsigned sched_thread_capacity(void) { return thread_cap; }

// Инструкции процессора, недоступные в хост-тесте, подменяются заглушками.
#ifdef KIRA_HOST_TEST
extern void host_halt(void);
#define CPU_HALT()  host_halt()
#define CPU_CLI()   ((void)0)
#define CPU_STI()   ((void)0)
#else
#define CPU_HALT()  cpu_halt()          /* cpu.h своей архитектуры */
#define CPU_CLI()   cpu_irq_off()
#define CPU_STI()   cpu_irq_on()
#endif

// Готовый поток может взять простаивающее ядро: его будят межъядерным прерыванием, а не ждут его тика (до 2 мс).
#ifdef KIRA_HOST_TEST
#define KICK_IDLE(aff) ((void)(aff))
#else
#include "smp.h"
#define KICK_IDLE(aff) smp_kick_idle(aff)
#endif

// Архитектурный хук переключения: TSS.rsp0, стек SYSCALL, CR3. Реализован в proc.c.
#ifdef KIRA_HOST_TEST
#define ARCH_SWITCH(t) ((void)(t))
#define LINE_SWITCH_FLUSH() ((void)0)
#define IDLE_TICK_OFF() ((void)0)
#define IDLE_TICK_ON()  ((void)0)
#else
extern void proc_arch_switch(const thread_t* next);
#define ARCH_SWITCH(t) proc_arch_switch(t)
/* Построчный буфер журнала этого ядра принадлежал прежнему потоку: его хвост выводится до того, как новый поток
   начнёт печатать (иначе поток, переехавший на другое ядро посреди строки, разорвал бы её, а в буфере смешались
   бы знаки двух потоков). */
#define LINE_SWITCH_FLUSH() uart_switch_flush()
/* Простаивающее ядро (кроме хранителя времени) выключает тик и спит до межъядерного прерывания. */
#define IDLE_TICK_OFF() smp_idle_tick_off()
#define IDLE_TICK_ON()  smp_idle_tick_on()
#endif

static void str_copy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void charge_current(void) {
    percpu_t* c = this_cpu();
    uint64_t now = rdtsc();
    c->current->cpu_cycles += now - c->last_switch_tsc;
    c->last_switch_tsc = now;
}

/* Квант потока с учётом веса процесса (не меньше тика). */
static uint32_t wq(const thread_t* t) {
    uint32_t q = quantum_ticks * (t->weight ? t->weight : SCHED_WEIGHT_NORMAL) / SCHED_WEIGHT_NORMAL;
    return q ? q : 1;
}

/* ---- Очереди готовых: у каждого ядра своя, по классу приоритета (FIFO внутри класса) ----
   Готовый поток стоит ровно в одной очереди (rq_cpu), холостые потоки ядер - ни в одной. Выбор следующего потока
   смотрит только свою очередь и счётчики по классам (rq_total), а не всю таблицу потоков: цена переключения не
   растёт с числом потоков. Работа более важного класса на чужой очереди забирается сразу; работа своего класса -
   когда своя очередь пуста (простаивающее ядро, конец кванта), поэтому ядра делят нагрузку по кругу, как с общей
   очередью. Пока общий замок один (sched_lock), очереди меняются под ним. */
#define NPRIO 4
typedef struct {
    thread_t* head[NPRIO];
    thread_t* tail[NPRIO];
    uint32_t cnt[NPRIO];
    uint32_t n;
} runq_t;
static runq_t rq[PERCPU_MAX_CPUS];
static uint32_t rq_total[NPRIO];

#ifdef KIRA_HOST_TEST
#define RESCHED_CPU(c) ((void)(c))
#else
#define RESCHED_CPU(c) smp_resched_cpu(c)
#endif

static void rq_push(unsigned cpu, thread_t* t, int front) {
    runq_t* q = &rq[cpu];
    unsigned p = (unsigned)t->prio;
    t->rq_cpu = (int32_t)cpu;
    if (front) {
        t->rq_prev = 0; t->rq_next = q->head[p];
        if (q->head[p]) q->head[p]->rq_prev = t; else q->tail[p] = t;
        q->head[p] = t;
    } else {
        t->rq_next = 0; t->rq_prev = q->tail[p];
        if (q->tail[p]) q->tail[p]->rq_next = t; else q->head[p] = t;
        q->tail[p] = t;
    }
    q->cnt[p]++; q->n++; rq_total[p]++;
}

/* Вытесненный, не доживший до конца кванта: в голову своей очереди, но после более тяжёлых (рефлекс поставил их туда
   именно для того, чтобы они пошли раньше). */
static void rq_push_preempted(unsigned cpu, thread_t* t) {
    runq_t* q = &rq[cpu];
    unsigned p = (unsigned)t->prio;
    thread_t* after = 0;
    for (thread_t* x = q->head[p]; x && x->weight > t->weight; x = x->rq_next) after = x;
    if (!after) { rq_push(cpu, t, 1); return; }
    t->rq_cpu = (int32_t)cpu;
    t->rq_prev = after; t->rq_next = after->rq_next;
    if (after->rq_next) after->rq_next->rq_prev = t; else q->tail[p] = t;
    after->rq_next = t;
    q->cnt[p]++; q->n++; rq_total[p]++;
}

static void rq_remove(thread_t* t) {
    if (t->rq_cpu < 0) return;
    runq_t* q = &rq[t->rq_cpu];
    unsigned p = (unsigned)t->prio;
    if (t->rq_prev) t->rq_prev->rq_next = t->rq_next; else q->head[p] = t->rq_next;
    if (t->rq_next) t->rq_next->rq_prev = t->rq_prev; else q->tail[p] = t->rq_prev;
    t->rq_next = t->rq_prev = 0;
    t->rq_cpu = -1;
    q->cnt[p]--; q->n--; rq_total[p]--;
}

static int cpu_usable(unsigned c) { return c < percpu_count() && percpu_table[c].sched_on && !percpu_table[c].failed; }
static unsigned cpu_load(unsigned c) {
    const percpu_t* pc = &percpu_table[c];
    return rq[c].n + (pc->current && pc->current != pc->idle);
}
/* Поток класса p начнёт исполняться на ядре c сразу: ядро простаивает или занято менее важным, и в его очереди нет
   работы того же или более важного класса. */
/* Рефлекс (нижний уровень Разума): вес потока - политика, которую задаёт Разум (программа в фокусе - 6, голодающая -
   8, обычная - 4). Проснувшийся поток с большим весом не ждёт конца чужого кванта в своём классе: он вытесняет
   более лёгкий поток того же класса сразу (микросекунды, без участия Разума). Только при пробуждении - вытесненный
   поток стоит в голове очереди с остатком кванта, поэтому перекидывания туда-обратно нет. */
static int outranks(const thread_t* t, const thread_t* cur) {
    if (t->prio != cur->prio) return t->prio > cur->prio;
    return t->prio > PRIO_IDLE && t->weight > (cur->weight ? cur->weight : SCHED_WEIGHT_NORMAL);
}

static int runs_now(unsigned c, const thread_t* t) {
    const percpu_t* pc = &percpu_table[c];
    unsigned p = (unsigned)t->prio;
    if (pc->current && pc->current != pc->idle && !outranks(t, pc->current)) return 0;
    for (unsigned k = p; k < NPRIO; k++) if (rq[c].cnt[k]) return 0;
    return 1;
}

/* Куда поставить готовый поток: на ядро, где он пойдёт сразу (прежнее ядро - первым: его кэш ещё тёплый), иначе на
   наименее загруженное разрешённое (при равенстве - прежнее). */
static unsigned choose_cpu(const thread_t* t) {
    unsigned n = percpu_count(), last = t->last_cpu;
    int last_ok = cpu_usable(last) && cm_test(&t->affinity, last);
    if (last_ok && runs_now(last, t)) return last;
    unsigned best = ~0u, best_load = ~0u, now_cpu = ~0u;
    for (unsigned c = 0; c < n; c++) {
        if (!cpu_usable(c) || !cm_test(&t->affinity, c)) continue;
        if (now_cpu == ~0u && runs_now(c, t)) now_cpu = c;
        unsigned l = cpu_load(c);
        if (l < best_load) { best = c; best_load = l; }
    }
    if (now_cpu != ~0u) return now_cpu;
    if (last_ok && cpu_load(last) <= best_load) return last;
    if (best != ~0u) return best;
    /* разрешённое ядро ещё не подключено (или его нет): поток ждёт в его очереди, как ждал бы в общей */
    for (unsigned c = 0; c < PERCPU_MAX_CPUS; c++) if (cm_test(&t->affinity, c)) return c;
    return this_cpu()->cpu_id;                                   /* пустая маска: вызывающий такого не ставит */
}

static thread_t* rq_first(unsigned c, unsigned p, unsigned cpu, const thread_t* self) {
    for (thread_t* t = rq[c].head[p]; t; ) {
        thread_t* nx = t->rq_next;
        if (t->state != THREAD_READY) rq_remove(t);   /* страховка: в очереди только готовые */
        else if ((t == self || !t->on_cpu) && cm_test(&t->affinity, cpu)) return t;
        t = nx;
    }
    return 0;
}

/* Поток класса p с чужой очереди, который можно исполнять на ядре cpu: дольше всех ждущий (головы очередей). */
static thread_t* steal(unsigned cpu, unsigned p, const thread_t* self) {
    thread_t* best = 0;
    for (unsigned c = 0; c < percpu_count(); c++) {
        if (c == cpu || !rq[c].cnt[p]) continue;
        thread_t* t = rq_first(c, p, cpu, self);
        if (t && (!best || t->ready_since < best->ready_since)) best = t;
    }
    return best;
}

/* Есть ли для ядра работа класса не ниже minp (своя или чужая). */
static int has_work(unsigned cpu, unsigned minp, const thread_t* self) {
    for (int p = NPRIO - 1; p >= (int)minp; p--) {
        if (!rq_total[p]) continue;
        if (rq[cpu].cnt[p] && rq_first(cpu, (unsigned)p, cpu, self)) return 1;
        if (rq_total[p] > rq[cpu].cnt[p] && steal(cpu, (unsigned)p, self)) return 1;
    }
    return 0;
}

/* Стек потока: в ядре - ячейка с пустотой ниже (vmm), в хост-тесте - обычная память. */
#ifdef KIRA_HOST_TEST
static uint8_t* kstack_new(void) { return (uint8_t*)kmalloc(SCHED_STACK_SIZE); }
static void kstack_del(uint8_t* s) { kfree(s); }
#else
static uint8_t* kstack_new(void) {
    uint8_t* s = (uint8_t*)vmm_kstack_alloc(SCHED_STACK_SIZE);
    return s ? s : (uint8_t*)kmalloc(SCHED_STACK_SIZE);           /* окно исчерпано: стек из кучи, с меткой на дне */
}
static void kstack_del(uint8_t* s) { if (vmm_is_kstack((uint64_t)(uintptr_t)s)) vmm_kstack_free(s); else kfree(s); }
#endif

static void check_canary(const thread_t* t) {
    if (t->stack_base && *(const uint64_t*)t->stack_base != STACK_CANARY) {
        kernel_panic("Kernel thread stack overflow detected");
    }
}

// Выбор следующего потока: наивысший класс, для которого есть работа; внутри класса - своя очередь по кругу, а если
// она пуста или чужой поток ждёт намного дольше - чужая. Вытесненный, не доживший до конца кванта, стоит в голове своей очереди с остатком кванта.
static thread_t* pick_next(unsigned cpu, const thread_t* self) {
    for (int p = NPRIO - 1; p >= 0; p--) {
        if (!rq_total[p]) continue;
        thread_t* t = rq[cpu].cnt[p] ? rq_first(cpu, (unsigned)p, cpu, self) : 0;
        if (rq_total[p] > rq[cpu].cnt[p]) {
            /* Чужой поток того же класса берётся, если своя очередь пуста или он ждёт заметно дольше своего (на
               два кванта): иначе поток, попавший за долгую работу другого ядра, ждал бы, пока остальные ядра
               делят время между своими. Небольшая разница - в пользу своего (его данные в кэше этого ядра).
               Если «своя» очередь - только что истративший квант текущий поток, ждущий чужой идёт первым. */
            thread_t* far = steal(cpu, (unsigned)p, self);
            if (far && (!t || (t == self && self->quantum_left == 0) ||
                        far->ready_since + 2ULL * quantum_ticks < t->ready_since)) t = far;
        }
        if (t) { rq_remove(t); return t; }
    }
    return this_cpu()->idle;
}

/* Поток стал готовым (проснулся, создан, получил событие): в очередь подходящего ядра; ядру, где он должен идти
   сразу, - пора выбрать заново (своему - флаг, чужому - межъядерное прерывание). Под sched_lock. */
static void make_ready_locked(thread_t* t) {
    t->state = THREAD_READY;
    t->ready_since = timer_get_ticks();
    ready_epoch++;
    unsigned c = choose_cpu(t);
    rq_push(c, t, t->weight > SCHED_WEIGHT_NORMAL);       /* тяжёлый (фокус, голодающий) - в голову очереди */
    percpu_t* me = this_cpu();
    percpu_t* pc = &percpu_table[c];
    if (!pc->sched_on || !sched_ready) return;
    if (pc == me) { if (me->current == me->idle || outranks(t, me->current)) me->need_resched = 1; }
    else if (pc->current == pc->idle || outranks(t, pc->current)) RESCHED_CPU(c);
}

// Завершает переключение уже на стеке нового потока: прежний поток теперь можно запускать где угодно,
// замок очереди освобождается. Вызывается после context_switch и при первом запуске потока.
static void finish_switch(void) {
    percpu_t* c = this_cpu();
    thread_t* p = c->switch_from;
    c->switch_from = 0;
    if (p) {
        __atomic_store_n(&p->on_cpu, 0, __ATOMIC_RELEASE);
        if (p->state == THREAD_READY) ready_epoch++;     /* вытесненный поток теперь может взять другое ядро */
    }
    int do_kick = p && p->state == THREAD_READY && p->prio > PRIO_IDLE;
    cpumask_t kick = do_kick ? p->affinity : cm_none();
    spin_unlock_raw(&sched_lock);
    if (do_kick) KICK_IDLE(kick);                           /* простаивающее ядро без тика само его не заметит */
    LINE_SWITCH_FLUSH();
}

// Вызывается с выключенными прерываниями и взятым sched_lock; замок возвращается освобождённым
// (при переключении его отпускает новый поток).
static void schedule_locked(void) {
    percpu_t* cpu = this_cpu();
    thread_t* prev = cpu->current;
    charge_current();
    check_canary(prev);

    uint64_t now_tick = timer_get_ticks();
    if (prev->state == THREAD_RUNNING && prev->throttled_until > now_tick) {
        prev->state = THREAD_SLEEPING;                 /* исчерпал свою долю окна: спит до его конца */
        prev->wake_tick = prev->throttled_until;
        if (prev->wake_tick < next_wake) next_wake = prev->wake_tick;
    }
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        prev->ready_since = now_tick;
        // Вытеснен, не дожив до конца кванта: встаёт в голову своей очереди и
        // сохраняет остаток (иначе поток с частыми прерываниями получал бы новый квант
        // каждый раз и никогда не уступал бы); истративший квант - в хвост.
        if (!(prev->flags & THREAD_F_IDLE)) {
            if (prev->quantum_left > 0) rq_push_preempted(cpu->cpu_id, prev);
            else rq_push(cpu->cpu_id, prev, 0);
        }
    }
    thread_t* next = pick_next(cpu->cpu_id, prev);

    next->state = THREAD_RUNNING;
    if (next->quantum_left == 0) next->quantum_left = wq(next);
    if (next == prev) { spin_unlock_raw(&sched_lock); return; }

    if (prev == cpu->idle) IDLE_TICK_ON();       /* ядро выходит из простоя: тик нужен для квантов */
    next->switches_in++;
    total_switches++;
    cpu->switches++;
    next->on_cpu = 1;
    next->last_cpu = cpu->cpu_id;
    cpu->current = next;
    cpu->switch_from = prev;
    // Состояние x87/SSE принадлежит потоку, а не ядру: сохранить своё, загрузить чужое.
    // Код ядра xmm не использует, поэтому до этого места регистры целы даже при
    // переключении из обработчика прерывания.
    fpu_save(prev->fpu);
    fpu_restore(next->fpu);
    ARCH_SWITCH(next);
    context_switch(&prev->rsp, next->rsp);
    // Сюда управление возвращается, когда prev снова выбран планировщиком (возможно, на другом ядре).
    finish_switch();
}

// Точка входа нового потока (на неё "возвращается" context_switch).
static void thread_trampoline(void) {
    finish_switch();
    CPU_STI();
    this_cpu()->current->entry(this_cpu()->current->arg);
    sched_exit();
}

static void idle_main(void* arg) {
    (void)arg;
    for (;;) {
        IDLE_TICK_OFF();
        CPU_HALT();
    }
}

static thread_t* create_thread(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio,
                               uint64_t cr3, void* proc, cpumask_t affinity, uint32_t flags);

thread_t* sched_create(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio) {
    return create_thread(name, entry, arg, prio, 0, 0, SCHED_AFFINITY_ALL, THREAD_F_SERVICE);
}

thread_t* sched_create_on(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio, cpumask_t affinity) {
    if (cm_empty(&affinity)) return 0;
    return create_thread(name, entry, arg, prio, 0, 0, affinity, 0);
}

thread_t* sched_create_proc(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio,
                            uint64_t cr3, void* proc) {
    return create_thread(name, entry, arg, prio, cr3, proc, SCHED_AFFINITY_ALL, 0);
}

static thread_t* create_thread(const char* name, void (*entry)(void*), void* arg, thread_prio_t prio,
                               uint64_t cr3, void* proc, cpumask_t affinity, uint32_t flags) {
    if (!entry) return 0;
    uint8_t* stack = kstack_new();
    if (!stack) return 0;

    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    thread_t* t = 0;
    t = take_slot();
    if (!t) {
        spin_unlock_raw(&sched_lock);
        irq_restore(f);
        kstack_del(stack);
        return 0;
    }

    *(uint64_t*)stack = STACK_CANARY;

    // Начальный кадр - как его снимает context_switch своей архитектуры (arch_sched.h): первый «возврат» уходит
    // в thread_trampoline. Вершина стека выровнена по 16 байт (требование ABI обеих архитектур).
    uint64_t* top = (uint64_t*)(((uintptr_t)(stack + SCHED_STACK_SIZE)) & ~(uintptr_t)0xF);
    uint64_t kstack_top = (uint64_t)(uintptr_t)top;

    t->rsp = arch_thread_stack(top, thread_trampoline);
    t->id = next_id++;
    str_copy(t->name, name, SCHED_NAME_LEN);
    t->prio = prio;
    t->wake_tick = 0;
    t->quantum_left = quantum_ticks;
    t->cpu_cycles = 0;
    t->switches_in = 0;
    t->stack_base = stack;
    t->kstack_top = kstack_top;
    t->cr3 = cr3;
    t->proc = proc;
    t->entry = entry;
    t->arg = arg;
    t->affinity = affinity;
    t->flags = flags;
    t->mutexes_held = 0;
    t->pid = 0;
    t->weight = SCHED_WEIGHT_NORMAL;
    t->ready_since = timer_get_ticks();
    t->limit = 0; t->lim_used = 0; t->lim_start = 0; t->throttled_until = 0;
    t->last_cpu = this_cpu()->cpu_id;
    t->wait_next = 0;
    t->on_cpu = 0;
    t->rq_next = t->rq_prev = 0;
    t->rq_cpu = -1;
    __builtin_memcpy(t->fpu, fpu_initial_state(), FPU_STATE_SIZE);
    make_ready_locked(t);        // последним: до этого слот считается свободным
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return t;
}

void sched_init(unsigned cpus, uint64_t ram_kb) {
    thread_cap = scale_threads(cpus, ram_kb);
    size_t bytes = (size_t)thread_cap * sizeof(thread_t) + _Alignof(thread_t);
    uint8_t* raw = (uint8_t*)kmalloc(bytes);
    if (!raw) kernel_panic("Cannot allocate the thread table");
    threads = (thread_t*)(((uintptr_t)raw + _Alignof(thread_t) - 1) & ~(uintptr_t)(_Alignof(thread_t) - 1));
    __builtin_memset(threads, 0, (size_t)thread_cap * sizeof(thread_t));
    thread_hi = 1;
    uint64_t f = irq_save();
    for (unsigned i = 0; i < thread_cap; i++) { threads[i].state = THREAD_UNUSED; threads[i].rq_cpu = -1; }

    // Текущий контекст (kernel_main на boot-стеке) становится потоком "core"
    thread_t* core = &threads[0];
    core->id = next_id++;
    str_copy(core->name, "core", SCHED_NAME_LEN);
    core->state = THREAD_RUNNING;
    core->prio = PRIO_CRITICAL;
    core->stack_base = 0;
    core->kstack_top = 0;
    core->cr3 = 0;
    core->proc = 0;
    core->quantum_left = quantum_ticks;
    core->weight = SCHED_WEIGHT_NORMAL;
    core->affinity = SCHED_AFFINITY_ALL;
    core->flags = THREAD_F_SERVICE;
    core->mutexes_held = 0;
    core->last_cpu = 0;
    core->on_cpu = 1;
    core->rq_cpu = -1;
    __builtin_memcpy(core->fpu, fpu_initial_state(), FPU_STATE_SIZE);
    this_cpu()->current = core;
    this_cpu()->last_switch_tsc = rdtsc();
    this_cpu()->attach_tsc = this_cpu()->last_switch_tsc;
    this_cpu()->sched_on = 1;
    irq_restore(f);

    thread_t* idle = sched_create("idle", idle_main, 0, PRIO_IDLE);
    if (!idle) kernel_panic("Cannot create idle thread");
    f = irq_save();
    spin_lock_raw(&sched_lock);
    rq_remove(idle);                        /* холостой поток в очереди не стоит: его выбирают, когда работы нет */
    idle->affinity = SCHED_AFFINITY_BSP;
    idle->flags |= THREAD_F_IDLE;
    this_cpu()->idle = idle;
    spin_unlock_raw(&sched_lock);
    irq_restore(f);

    sched_ready = 1;
    uart_print("[KIRA SCHED] Preemptive scheduler online: threads core(critical) + idle, round-robin per priority class, quantum and per-process weight set by the mind state.\n");
}

void sched_detach_space(void) {
    uint64_t f = irq_save();
    this_cpu()->current->cr3 = 0;
    this_cpu()->current->proc = 0;
    irq_restore(f);
}

void sched_yield(void) {
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    this_cpu()->current->quantum_left = 0; // добровольно отдаёт остаток кванта: уходит в хвост очереди
    schedule_locked();
    irq_restore(f);
}

void sched_sleep_ticks(uint32_t ticks) {
    if (ticks == 0) { sched_yield(); return; }
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    this_cpu()->current->wake_tick = timer_get_ticks() + ticks;
    this_cpu()->current->state = THREAD_SLEEPING;
    if (this_cpu()->current->wake_tick < next_wake) next_wake = this_cpu()->current->wake_tick;
    schedule_locked();
    irq_restore(f);
}

void sched_exit(void) {
    CPU_CLI();
    spin_lock_raw(&sched_lock);
    this_cpu()->current->state = THREAD_ZOMBIE;
    schedule_locked();
    for (;;) CPU_HALT(); // сюда не возвращаемся
}

unsigned sched_reap(void) {
    unsigned n = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++) {
        thread_t* t = &threads[i];
        /* on_cpu: пока ядро не ушло со стека завершённого потока, освобождать его нельзя */
        if (t->state == THREAD_ZOMBIE && t != this_cpu()->current && !t->on_cpu) {
            uint8_t* stack = t->stack_base;
            t->state = THREAD_UNUSED;
            t->stack_base = 0;
            spin_unlock_raw(&sched_lock);
            irq_restore(f);
            kstack_del(stack);
            f = irq_save();
            spin_lock_raw(&sched_lock);
            n++;
        }
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return n;
}

void sched_tick(void) {
    if (!sched_ready) return;
    percpu_t* cpu = this_cpu();
    if (!cpu->sched_on) return;
    uint64_t now = timer_get_ticks();
    unsigned id = cpu->cpu_id;
    thread_t* cur = cpu->current;

    /* Обычный тик обходится без общего замка: квант и доля процессора текущего потока принадлежат этому ядру.
       Замок нужен, только когда есть что решать: настал срок пробуждения, холостое ядро может взять работу, квант
       кончился. Иначе каждое ядро брало бы замок 500 раз в секунду, и на виртуальной машине, где ядер больше, чем
       у хозяина, очередь за замком (ожидающий ядро-поток вытеснен хозяином) останавливала всех. */
    int need = now >= __atomic_load_n(&next_wake, __ATOMIC_RELAXED);
    if (cur == cpu->idle) {
        need |= cpu->seen_epoch != __atomic_load_n(&ready_epoch, __ATOMIC_RELAXED);
    } else {
        if (cur->limit && cur->limit < 100) {           /* ограничение доли процессора (Разум) */
            if (now - cur->lim_start >= SCHED_LIMIT_WINDOW) { cur->lim_start = now; cur->lim_used = 0; }
            if (++cur->lim_used * 100 >= cur->limit * SCHED_LIMIT_WINDOW) {
                cur->throttled_until = cur->lim_start + SCHED_LIMIT_WINDOW;
                cpu->need_resched = 1;
            }
        }
        if (cur->quantum_left > 0) cur->quantum_left--;
        need |= cur->quantum_left == 0 && !cpu->need_resched;
    }
    if (!need) return;

    spin_lock_raw(&sched_lock);
    if (now >= next_wake) {
        uint64_t nw = ~0ULL;
        for (unsigned i = 0; i < thread_hi; i++) {
            thread_t* t = &threads[i];
            if (t->state != THREAD_SLEEPING) continue;
            if (t->wake_tick <= now) {
                make_ready_locked(t);                   // в очередь ядра, где пойдёт; более важный вытесняет сразу
            } else if (t->wake_tick < nw) {
                nw = t->wake_tick;
            }
        }
        next_wake = nw;
    }

    if (cur == cpu->idle) {
        if (cpu->seen_epoch != ready_epoch) {
            cpu->seen_epoch = ready_epoch;
            if (has_work(id, PRIO_NORMAL, cur)) cpu->need_resched = 1;
        }
    } else if (cur->quantum_left == 0 && !cpu->need_resched) {
        /* конец кванта: уступить, если работа того же или более важного класса ждёт здесь или на другом ядре */
        if (has_work(id, cur->prio > PRIO_IDLE ? (unsigned)cur->prio : PRIO_NORMAL, cur)) cpu->need_resched = 1;
        if (!cpu->need_resched) cur->quantum_left = wq(cur); // конкурентов нет - новый квант
    }
    spin_unlock_raw(&sched_lock);
}

void sched_irq_exit(void) {
    if (!sched_ready) return;
    percpu_t* cpu = this_cpu();
    if (!cpu->need_resched) return;
    cpu->need_resched = 0;
    spin_lock_raw(&sched_lock);
    schedule_locked(); // прерывания в обработчике уже выключены
}

// Подключает не загрузочное ядро: его текущий контекст (стек запуска) становится холостым потоком ядра.
int sched_attach_cpu(void) {
    percpu_t* cpu = this_cpu();
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    thread_t* t = 0;
    t = take_slot();
    if (!t) { spin_unlock_raw(&sched_lock); irq_restore(f); return -1; }
    t->id = next_id++;
    t->name[0] = 'i'; t->name[1] = 'd'; t->name[2] = 'l'; t->name[3] = 'e';
    unsigned n = 4, v = cpu->cpu_id; char dg[10]; int k = 0;
    do { dg[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && n + 1 < SCHED_NAME_LEN) t->name[n++] = dg[--k];
    t->name[n] = '\0';
    t->prio = PRIO_IDLE;
    t->wake_tick = 0;
    t->quantum_left = quantum_ticks;
    t->cpu_cycles = 0;
    t->switches_in = 0;
    t->stack_base = 0;           // стек запуска ядра: освобождать нечего
    t->kstack_top = 0;
    t->cr3 = 0;
    t->proc = 0;
    t->entry = 0;
    t->arg = 0;
    t->affinity = cm_one(cpu->cpu_id);
    t->flags = THREAD_F_IDLE;
    t->rq_next = t->rq_prev = 0;
    t->rq_cpu = -1;
    t->mutexes_held = 0;
    t->pid = 0;
    t->weight = SCHED_WEIGHT_NORMAL;
    t->limit = 0; t->throttled_until = 0;
    t->last_cpu = cpu->cpu_id;
    t->on_cpu = 1;
    __builtin_memcpy(t->fpu, fpu_initial_state(), FPU_STATE_SIZE);
    t->state = THREAD_RUNNING;
    cpu->current = t;
    cpu->idle = t;
    cpu->last_switch_tsc = rdtsc();
    cpu->attach_tsc = cpu->last_switch_tsc;
    cpu->sched_on = 1;
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return 0;
}

int sched_cpu_stats(unsigned cpu, uint64_t* idle_cycles, uint64_t* total_cycles, uint64_t* switches) {
    if (cpu >= PERCPU_MAX_CPUS) return -1;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    const percpu_t* c = &percpu_table[cpu];
    int ok = c->sched_on && c->idle;
    if (ok) {
        uint64_t now = rdtsc();
        uint64_t idle = c->idle->cpu_cycles;
        if (c->current == c->idle) idle += now - c->last_switch_tsc;
        if (idle_cycles) *idle_cycles = idle;
        if (total_cycles) *total_cycles = now - c->attach_tsc;
        if (switches) *switches = c->switches;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return ok ? 0 : -1;
}

void sched_set_quantum(uint32_t ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > MAX_QUANTUM) ticks = MAX_QUANTUM;
    quantum_ticks = ticks;
}

uint32_t sched_get_quantum(void) { return quantum_ticks; }
void sched_set_tsc_khz(uint64_t khz) { tsc_khz = khz; }
uint64_t sched_total_switches(void) { return total_switches; }
const thread_t* sched_current(void) { return this_cpu()->current; }

// Суммарное бездействие всех ядер: по нему организм судит о нагрузке системы (делить на число ядер).
uint64_t sched_idle_cycles(void) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < percpu_count(); i++) {
        uint64_t idle = 0;
        if (sched_cpu_stats(i, &idle, 0, 0) == 0) sum += idle;
    }
    return sum;
}

// Для аварийного отчёта (остальные ядра уже остановлены, замки сняты): что исполнялось на каждом ядре и где
// основной поток core. Читает без замка - только для диагностики.
static void put_num(void (*out)(const char*), uint64_t v) {
    char b[21]; int i = 20; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(&b[i]);
}

static void put_hex(void (*out)(const char*), uint64_t v) {
    char b[19]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0; out(b);
}

#ifndef KIRA_HOST_TEST
extern char stack_bottom[], stack_top[];      /* стек запуска (boot.s): на нём живёт поток core */
#endif

// Где стояло ядро и цепочка вызовов (ядро собрано с указателями кадров). Кадры читаются только внутри стека этого
// потока, поэтому обход не может сам упасть на чужой памяти.
static void trace_line(void (*out)(const char*), const char* what, uint64_t rip, uint64_t fp, uint64_t lo, uint64_t hi) {
    out("[KIRA SCHED]     "); out(what); put_hex(out, rip);
    if (fp && lo) {
        out(" calls");
        for (int n = 0; n < 12 && fp >= lo && fp + 16 <= hi && !(fp & 7); n++) {
            const uint64_t* f = (const uint64_t*)(uintptr_t)fp;
            out(" "); put_hex(out, f[1]);
            if (f[0] <= fp) break;                /* кадры идут только вверх по стеку */
            fp = f[0];
        }
    }
    out("\n");
}

static void crash_trace(void (*out)(const char*), unsigned cpu, const percpu_t* c, const thread_t* t) {
    uint64_t lo = 0, hi = 0;
#ifndef KIRA_HOST_TEST
    if (t && t->stack_base) { lo = (uint64_t)(uintptr_t)t->stack_base; hi = lo + SCHED_STACK_SIZE; }
    else if (t && cpu == 0) { lo = (uint64_t)(uintptr_t)stack_bottom; hi = (uint64_t)(uintptr_t)stack_top; }
#else
    (void)cpu; (void)t;
#endif
    if (__atomic_load_n(&c->halted, __ATOMIC_ACQUIRE)) trace_line(out, "now at ", c->halt_rip, c->halt_rbp, lo, hi);
    else if (c == this_cpu()) out("[KIRA SCHED]     (this core reports)\n");
    else out("[KIRA SCHED]     did not answer the halt NMI\n");
    trace_line(out, "last irq entered at ", c->irq_rip, c->irq_rbp, lo, hi);
}

/* Цепочка вызовов кода, прерванного на этом ядре (rbp - его указатель кадра), в пределах стека текущего потока. */
unsigned sched_trace_current(uint64_t fp, uint64_t* out, unsigned max) {
    unsigned n = 0;
#ifndef KIRA_HOST_TEST
    const thread_t* t = this_cpu()->current;
    uint64_t lo = 0, hi = 0;
    if (t && t->stack_base) { lo = (uint64_t)(uintptr_t)t->stack_base; hi = lo + SCHED_STACK_SIZE; }
    else if (t && this_cpu()->cpu_id == 0) { lo = (uint64_t)(uintptr_t)stack_bottom; hi = (uint64_t)(uintptr_t)stack_top; }
    while (n < max && fp && lo && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
        const uint64_t* f = (const uint64_t*)(uintptr_t)fp;
        out[n++] = f[1];
        if (f[0] <= fp) break;
        fp = f[0];
    }
#else
    (void)fp; (void)out; (void)max;
#endif
    return n;
}

/* Остальные ядра получили аварийное NMI: подождать (до ~50 мс), пока они запишут, где стояли. */
void sched_crash_wait_halted(void) {
#ifndef KIRA_HOST_TEST
    uint64_t khz = tsc_khz ? tsc_khz : 1000000, t0 = rdtsc();
    for (;;) {
        unsigned pending = 0;
        for (unsigned i = 0; i < percpu_count(); i++) {
            const percpu_t* c = &percpu_table[i];
            if (c != this_cpu() && c->online && !__atomic_load_n(&c->halted, __ATOMIC_ACQUIRE)) pending++;
        }
        if (!pending || rdtsc() - t0 > khz * 50) return;
        __asm__ volatile ("pause");
    }
#endif
}

void sched_crash_report(void (*out)(const char*)) {
    static const char* const st[] = { "unused", "ready", "RUNNING", "sleep", "zombie", "blocked" };
    out("[KIRA SCHED] ticks "); put_num(out, timer_get_ticks());
    out(", next wake "); put_num(out, next_wake == ~0ULL ? 0 : next_wake);
    out(", ready epoch "); put_num(out, ready_epoch);
    out(", sched_lock next "); put_num(out, sched_lock.next); out(" owner "); put_num(out, sched_lock.owner);
#ifndef KIRA_HOST_TEST
    out(", reported by core "); put_num(out, this_cpu()->cpu_id);
#endif
    out("\n");
    for (unsigned i = 0; i < percpu_count(); i++) {
        const percpu_t* c = &percpu_table[i];
        out("[KIRA SCHED] core "); put_num(out, i);
        if (!c->sched_on) { out(c->failed ? ": failed\n" : ": not scheduling\n"); continue; }
        const thread_t* t = c->current;
        out(": running '"); out(t ? t->name : "?"); out("'");
        if (t) { out(" prio "); put_num(out, t->prio); out(", quantum left "); put_num(out, t->quantum_left); }
        out(", irq depth "); put_num(out, c->irq_depth); out(", locks "); put_num(out, c->locks_held); out("\n");
        crash_trace(out, i, c, t);
    }
    if (threads && thread_hi) {
        const thread_t* core = &threads[0];
        out("[KIRA SCHED] thread 'core': "); out(core->state <= THREAD_BLOCKED ? st[core->state] : "?");
        out(", wake at "); put_num(out, core->wake_tick); out(", on cpu flag "); put_num(out, core->on_cpu);
        out(", last core "); put_num(out, core->last_cpu); out(", mutexes "); put_num(out, core->mutexes_held); out("\n");
    }
}

int sched_can_sleep(void) {
#ifdef KIRA_HOST_TEST
    return 0;
#else
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0" : "=r"(fl));
    percpu_t* c = this_cpu();
    return sched_ready && (fl & 0x200) && c->sched_on && c->current && c->current != c->idle &&
           c->irq_depth == 0 && c->locks_held == 0;
#endif
}

int sched_thread_alive(uint32_t id) {
    int alive = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++) {
        const thread_t* t = &threads[i];
        if (t->id == id && t->state != THREAD_UNUSED && t->state != THREAD_ZOMBIE) { alive = 1; break; }
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return alive;
}

unsigned sched_core_fail(void) {
    percpu_t* c = this_cpu();
    unsigned id = c->cpu_id, orphans = 0;
    spin_lock_raw(&sched_lock);
    thread_t* cur = c->current;
    thread_t* idle = c->idle;
    /* Ядро продолжает стоять на стеке текущего потока, поэтому on_cpu у него остаётся 1: sched_reap не освободит
       стек из-под ног (за отказавшее ядро платим одним стеком потока). */
    if (cur && cur != idle) cur->state = THREAD_ZOMBIE;
    c->switch_from = 0;
    c->sched_on = 0;                               /* до переселения очереди: на это ядро больше не ставят */
    for (unsigned i = 0; i < thread_hi; i++) {
        thread_t* t = &threads[i];
        if (t == cur || t == idle || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE) continue;
        if (!cm_test(&t->affinity, id)) continue;
        cm_clear(&t->affinity, id);
        if (cm_empty(&t->affinity) && t->state != THREAD_BLOCKED) {   // идти больше некуда; ждущего в очереди мьютекса не трогаем
            rq_remove(t);
            t->state = THREAD_ZOMBIE;
            orphans++;
        }
    }
    for (unsigned p = 0; p < NPRIO; p++) {         /* очередь отказавшего ядра - на остальные ядра */
        while (rq[id].head[p]) {
            thread_t* t = rq[id].head[p];
            rq_remove(t);
            rq_push(choose_cpu(t), t, 0);
        }
    }
    if (idle) {                                    // холостой поток умершего ядра больше не выбирается
        idle->affinity = cm_none();
        if (idle->stack_base) idle->state = THREAD_ZOMBIE;
        else if (idle != cur) idle->state = THREAD_UNUSED;
        if (idle != cur) __atomic_store_n(&idle->on_cpu, 0, __ATOMIC_RELEASE);
    }
    c->sched_on = 0;
    c->online = 0;
    __atomic_store_n(&c->failed, 1, __ATOMIC_RELEASE);
    spin_unlock_raw(&sched_lock);
    return orphans;
}

unsigned sched_set_proc_weight(uint32_t pid, uint32_t weight) {
    if (!pid) return 0;
    if (weight < 1) weight = 1;
    if (weight > 8) weight = 8;
    unsigned n = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++) {
        thread_t* t = &threads[i];
        if (t->pid != pid || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE) continue;
        t->weight = weight;
        if (t->quantum_left > wq(t)) t->quantum_left = wq(t);
        n++;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return n;
}

unsigned sched_set_proc_limit(uint32_t pid, uint32_t pct) {
    if (!pid) return 0;
    if (pct < 5) pct = 5;
    if (pct >= 100) pct = 0;
    unsigned n = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++) {
        thread_t* t = &threads[i];
        if (t->pid != pid || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE) continue;
        t->limit = pct; t->lim_used = 0; t->lim_start = timer_get_ticks();
        if (!pct) t->throttled_until = 0;
        n++;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return n;
}

unsigned sched_runnable(void) {
    unsigned n = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++) {
        const thread_t* t = &threads[i];
        if ((t->state == THREAD_READY || t->state == THREAD_RUNNING) && t->prio > PRIO_IDLE) n++;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return n;
}

unsigned sched_cpu_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < percpu_count(); i++) if (percpu_table[i].sched_on) n++;
    return n ? n : 1;
}

// Поток блокируется до sched_wake. Замок l отпускается после того, как поток помечен ожидающим и
// планировщик взят: ни одно пробуждение не может проскочить между проверкой условия и сном.
void sched_block_and_unlock(spinlock_t* l) {
    spin_lock_raw(&sched_lock);
    spin_unlock_raw(l);
    this_cpu()->current->state = THREAD_BLOCKED;
    schedule_locked();
}

void sched_wait_and_unlock(spinlock_t* l, uint64_t ticks) {
    spin_lock_raw(&sched_lock);
    spin_unlock_raw(l);
    thread_t* cur = this_cpu()->current;
    uint64_t now = timer_get_ticks();
    cur->wake_tick = ticks >= ~0ULL - now ? ~0ULL : now + ticks;
    cur->state = THREAD_SLEEPING;
    if (cur->wake_tick < next_wake) next_wake = cur->wake_tick;
    schedule_locked();
}

void sched_wake_waiter(thread_t* t) {
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    if (t->state == THREAD_SLEEPING) make_ready_locked(t);
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
}

void sched_wake_proc(void* proc) {
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    for (unsigned i = 0; i < thread_hi; i++)
        if (threads[i].proc == proc && threads[i].state == THREAD_SLEEPING) make_ready_locked(&threads[i]);
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
}

void sched_set_wake_tick(thread_t* t, uint64_t tick) {
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    if (t->state == THREAD_SLEEPING) {
        t->wake_tick = tick;
        if (tick < next_wake) next_wake = tick;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
}

void sched_wake(thread_t* t) {
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    if (t->state == THREAD_BLOCKED) make_ready_locked(t);
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
}

unsigned sched_snapshot(thread_info_t* out, unsigned max) {
    uint64_t now_t = timer_get_ticks();
    unsigned n = 0;
    uint64_t f = irq_save();
    spin_lock_raw(&sched_lock);
    uint64_t now = rdtsc();
    for (unsigned i = 0; i < thread_hi && n < max; i++) {
        const thread_t* t = &threads[i];
        if (t->state == THREAD_UNUSED) continue;
        thread_info_t* o = &out[n++];
        o->id = t->id;
        str_copy(o->name, t->name, SCHED_NAME_LEN);
        o->state = t->state;
        o->prio = t->prio;
        uint64_t extra = 0;
        for (unsigned c = 0; c < percpu_count(); c++)
            if (percpu_table[c].current == t) extra = now - percpu_table[c].last_switch_tsc;   // идёт прямо сейчас
        o->cpu_cycles = t->cpu_cycles + extra;
        o->switches_in = t->switches_in;
        o->cpu = t->last_cpu;
        o->pid = t->pid;
        o->weight = t->weight;
        o->waiting = (t->state == THREAD_READY && t->prio > PRIO_IDLE) ? now_t - t->ready_since : 0;
    }
    spin_unlock_raw(&sched_lock);
    irq_restore(f);
    return n;
}

void sched_dump(void) {
    thread_info_t* info = (thread_info_t*)kmalloc(sizeof(thread_info_t) * thread_cap);
    if (!info) { uart_print("ps: not enough memory for the snapshot\n"); return; }
    unsigned n = sched_snapshot(info, thread_cap);
    static const char* const state_names[] = { "unused", "ready", "RUNNING", "sleep", "zombie", "blocked" };
    static const char* const prio_names[] = { "idle", "normal", "critical", "realtime" };

    uart_print("\n  ID  PID NAME             STATE    PRIO      CPU(ms)  SWITCHES  CORE\n");
    for (unsigned i = 0; i < n; i++) {
        uart_print("  ");
        uart_print_dec(info[i].id);
        uart_print("  ");
        if (info[i].pid) uart_print_dec(info[i].pid); else uart_print("-");
        uart_print("  ");
        uart_print(info[i].name);
        unsigned len = 0; while (info[i].name[len]) len++;
        for (; len < 16; len++) uart_print(" ");
        uart_print(" ");
        uart_print(state_names[info[i].state]);
        uart_print("  ");
        uart_print(prio_names[info[i].prio]);
        uart_print("  ");
        uart_print_dec(tsc_khz ? info[i].cpu_cycles / tsc_khz : info[i].cpu_cycles);
        uart_print(tsc_khz ? "  " : " cycles  ");
        uart_print_dec(info[i].switches_in);
        uart_print("  ");
        uart_print_dec(info[i].cpu);
        uart_print("\n");
    }
    uart_print("  quantum: ");
    uart_print_dec(quantum_ticks * 10 / TIMER_PER_10MS);
    uart_print(" ms, context switches: ");
    uart_print_dec(total_switches);
    uart_print(", threads: "); uart_print_dec(n); uart_print(" of "); uart_print_dec(thread_cap);
    uart_print("\n\n");
    kfree(info);
}
