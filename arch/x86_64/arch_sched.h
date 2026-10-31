#ifndef ARCH_SCHED_H
#define ARCH_SCHED_H
#include <stdint.h>
/* x86-64: начальный кадр для context_switch (context.s): 6 регистров rbp rbx r12..r15, адрес входа (его «вернёт»
   ret) и фиктивный адрес возврата - так на входе в trampoline rsp = 8 (mod 16), как у обычной функции. */
static inline uint64_t arch_thread_stack(uint64_t* top, void (*entry)(void)) {
    *--top = 0;
    *--top = (uint64_t)(uintptr_t)entry;
    for (int i = 0; i < 6; i++) *--top = 0;
    return (uint64_t)(uintptr_t)top;
}
#endif
