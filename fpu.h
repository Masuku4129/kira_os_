#ifndef FPU_H
#define FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

// Включает x87/SSE для кода, исполняемого потоками (CR0.MP/NE, CR4.OSFXSR/OSXMMEXCPT),
// приводит FPU и MXCSR в исходное состояние и запоминает его как шаблон для новых потоков.
// Само ядро собрано без SSE и регистров xmm не трогает. Возвращает 0 при успехе.
int fpu_init(void);
void fpu_init_cpu(void);   /* то же управление x87/SSE для не загрузочного процессора */

// Шаблон начального состояния FPU/SSE (FXSAVE-образ, 512 байт).
const void* fpu_initial_state(void);

#endif
