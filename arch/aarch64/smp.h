#ifndef SMP_H
#define SMP_H
#include "cpumask.h"
/* ARM64: межъядерные сигналы планировщика - SGI контроллера GIC (arch/aarch64/main.c). */
void smp_kick_idle(cpumask_t affinity);   // разбудить одно простаивающее разрешённое ядро
void smp_resched_cpu(unsigned cpu);       // ядру cpu - выбрать поток заново
void smp_idle_tick_off(void);             // простаивающее ядро (кроме хранителя времени) выключает тик
void smp_idle_tick_on(void);
int  smp_halting(void);
#endif
