#ifndef SMP_H
#define SMP_H
#include "cpumask.h"
/* ARM64: межъядерные сигналы планировщика - SGI контроллера GIC (arch/aarch64/main.c). */
void smp_kick_idle(cpumask_t affinity);   // разбудить одно простаивающее разрешённое ядро
void smp_resched_cpu(unsigned cpu);       // ядру cpu - выбрать поток заново
void smp_idle_tick_off(void);             // простаивающее ядро (кроме хранителя времени) выключает тик
void smp_idle_tick_on(void);
int  smp_halting(void);
void smp_halt_others(void);              // авария: остальные ядра встают
/* Общая консоль (console.c) - то же, что у x86-64 */
unsigned smp_online(void);
void smp_describe(void (*out)(const char*));
int  smp_selftest_start(void);          // >0 - запущено потоков; <0 - нельзя
int  smp_crash_test(unsigned mode, unsigned cpu);   // -4: на ARM64 пока нет
#endif
