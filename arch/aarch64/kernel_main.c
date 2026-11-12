#include "kernel_main.h"
#include "console.h"
#include "timer.h"
#include "mind_sys.h"
#include "experience.h"
#include "sched.h"
#include "progs.h"
#include "kdrv.h"
#include "life.h"
#include "watchdog.h"

void arm_kernel_main_loop(void) {
    for (;;) {
        console_poll();
        static uint64_t last_sec, seconds;
        if (timer_get_ticks() - last_sec >= TIMER_HZ) {
            last_sec = timer_get_ticks();
            seconds++;
            mind_sys_tick(seconds);
            experience_tick(seconds);
            sched_reap();
        }
        prog_poll();
        kdrv_poll();
        life_tick();
        watchdog_feed();
        sched_sleep_ticks(1);
    }
}
