#ifndef ARCH_SCHED_H
#define ARCH_SCHED_H
#include <stdint.h>
/* ARM64: начальный кадр для context_switch (context.S) - 12 слов: x29, x30 (адрес входа: его «вернёт» ret),
   x19..x28. После возврата sp = вершина стека, выровненная по 16. */
static inline uint64_t arch_thread_stack(uint64_t* top, void (*entry)(void)) {
    top -= 12;
    for (int i = 0; i < 12; i++) top[i] = 0;
    top[1] = (uint64_t)(uintptr_t)entry;
    return (uint64_t)(uintptr_t)top;
}
#endif
