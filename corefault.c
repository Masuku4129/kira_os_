#include "corefault.h"

#define VEC_NMI 2
#define VEC_DF  8
#define VEC_MC  18

const char* corefault_why_panic(const corefault_input_t* in) {
    if (!in) return "missing fault context";
    if (in->in_fault)                          return "second fault while handling a fault";
    if (in->vector == VEC_NMI || in->vector == VEC_DF) return "NMI/double fault is never isolated";
    if (in->locks_held)                        return "core holds spinlocks (shared data may be half-written)";
    if (in->irq_depth)                         return "fault inside an interrupt handler";
    if (!in->have_thread)                      return "no scheduled thread on this core";
    if (in->service)                           return "kernel service thread";
    if (in->mutexes_held)                      return "thread holds kernel mutexes (waiters would hang)";
    if (in->vector == VEC_MC) {
        if (!in->other_cores)                  return "no other core to take over";
        if (!in->irq_move_ok)                  return "device interrupts cannot be moved off this core";
    }
    return 0;
}

corefault_action_t corefault_decide(const corefault_input_t* in) {
    if (!in) return COREFAULT_PANIC;
    if (corefault_why_panic(in)) return COREFAULT_PANIC;
    return in->vector == VEC_MC ? COREFAULT_FAIL_CORE : COREFAULT_KILL_THREAD;
}
