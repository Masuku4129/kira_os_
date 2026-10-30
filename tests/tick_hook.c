// Собирается с -mno-sse: "обработчик прерывания" не должен трогать регистры xmm.
#include <stdint.h>
#include "sched.h"
extern uint64_t fake_ticks;
void tick_hook(void) {
    fake_ticks++;
    sched_tick();
    sched_irq_exit();
}
