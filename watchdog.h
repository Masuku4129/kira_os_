#ifndef WATCHDOG_H
#define WATCHDOG_H

void watchdog_init(void);
void watchdog_feed(void);
/* Progress from virtq_run's bounded device wait; never call from an unbounded loop. */
void watchdog_progress(void);
void watchdog_check(void);
void watchdog_trigger_panic(const char* reason);

#endif
