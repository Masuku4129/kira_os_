#ifndef KIRA_X86_TLB_SHOOTDOWN_H
#define KIRA_X86_TLB_SHOOTDOWN_H

#include <stdint.h>
#include "cpumask.h"

/* Acknowledgements are tied to one request generation. An old acknowledgement
   must never satisfy a newer shootdown, including after CPU-mask reuse. */

/* Never reuse a generation: on uint64 exhaustion the caller must fail closed. */
static inline int tlb_shootdown_next_generation(uint64_t current, uint64_t* next) {
    if (!next || current == UINT64_MAX) return 0;
    *next = current + 1;
    return *next != 0;
}
static inline int tlb_shootdown_all_acked(const cpumask_t* targets,
                                         const volatile uint64_t ack[CPU_MAX],
                                         uint64_t generation) {
    if (!targets || !ack || !generation) return 0;
    for (unsigned cpu = 0; cpu < CPU_MAX; cpu++) {
        if (cm_test(targets, cpu) &&
            __atomic_load_n(&ack[cpu], __ATOMIC_ACQUIRE) != generation)
            return 0;
    }
    return 1;
}

#endif
