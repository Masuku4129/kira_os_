#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>

// Данные одного процессора. В режиме ядра адрес структуры лежит в IA32_GS_BASE, поле self (смещение 0) даёт
// указатель одной командой `mov %gs:0`. В Ring 3 GS принадлежит программе (ядерное значение - в KERNEL_GS_BASE),
// поэтому каждый вход из Ring 3 (SYSCALL, прерывание, исключение) делает swapgs, а NMI/#DF/#MC проверяют GS_BASE.
//
// Смещения полей используются в interrupts.s (вход SYSCALL): менять порядок нельзя, проверяют _Static_assert в percpu.c.
#define PERCPU_OFF_USER_RSP    8
#define PERCPU_OFF_KERNEL_RSP  16
#define PERCPU_MAX_CPUS        256      /* = CPU_MAX (cpumask.h) */

struct thread;

typedef struct percpu {
    struct percpu* self;          /*  0 */
    uint64_t syscall_user_rsp;    /*  8: куда SYSCALL прячет пользовательский стек */
    uint64_t syscall_kernel_rsp;  /* 16: вершина стека ядра текущего потока */
    uint32_t cpu_id;              /* 24: номер в таблице (0 - загрузочный) */
    uint32_t apic_id;             /* 28 */
    struct thread* current;       /* 32 */
    struct thread* idle;          /* 40 */
    volatile uint64_t ipi_count;  /* 48: принятые IPI-пинги (проверка доставки между ядрами) */
    volatile uint32_t online;     /* 56: процессор запущен и готов (1) */
    volatile uint32_t need_resched; /* 60: на этом ядре пора переключить поток (ставится в тике, читается после EOI) */
    uint64_t last_switch_tsc;     /* 64: когда ядро последний раз сменило поток (учёт времени) */
    struct thread* switch_from;   /* 72: поток, с которого только что переключились; освобождается уже на новом стеке */
    uint64_t switches;            /* 80: переключений на этом ядре */
    uint64_t attach_tsc;          /* 88: когда ядро вошло в планировщик */
    uint64_t active_cr3;          /* 96: загруженный сейчас корень таблиц страниц */
    void* tss;                    /* 104: TSS этого ядра (0 - общий TSS загрузочного) */
    volatile uint32_t sched_on;   /* 112: ядро участвует в планировании (есть текущий и холостой поток) */
    volatile uint32_t irq_depth;  /* 116: >0, пока ядро в обработчике прерывания (вывод идёт в отдельный строковый буфер) */
    volatile uint32_t locks_held; /* 120: сколько спин-замков держит ядро; сбой при удержанных замках изолировать нельзя */
    volatile uint32_t failed;     /* 124: ядро отказало в режиме ядра и остановлено, система работает без него */
    volatile uint32_t in_fault;   /* 128: ядро уже разбирает сбой (второй сбой - авария) */
    uint32_t fail_vector;         /* 132 */
    uint64_t fail_rip;            /* 136 */
    int64_t  tsc_skew;            /* 144: TSC этого ядра минус TSC загрузочного (измерено при запуске), такты */
    uint64_t tsc_rtt;             /* 152: время обмена при этом замере (точность оценки), такты */
    uint64_t seen_epoch;          /* 160: эпоха готовых потоков, которую холостое ядро уже просмотрело */
    volatile uint32_t tick_off;   /* 168: ядро простаивает и выключило свой тик (будят межъядерным прерыванием) */
} percpu_t;

extern percpu_t percpu_table[PERCPU_MAX_CPUS];

#ifdef KIRA_HOST_TEST
extern unsigned host_cpu;      /* в тесте номер «текущего» ядра выбирает сам тест */
static inline percpu_t* this_cpu(void) { return &percpu_table[host_cpu]; }
#else
static inline percpu_t* this_cpu(void) {
    percpu_t* p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}
#endif

// Загрузочный процессор: заполняет запись 0 и ставит GS. Вызывать после gdt_init (она перезагружает GS).
void percpu_init_bsp(uint32_t apic_id);
// Запись для другого процессора (используется при запуске остальных ядер); NULL, если таблица полна.
percpu_t* percpu_alloc(uint32_t apic_id);
unsigned percpu_count(void);
void percpu_set_gs(percpu_t* p);      /* GS_BASE = p, KERNEL_GS_BASE = 0 (значение процесса) */

#endif
