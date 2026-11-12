#ifndef WATCHDOG_H
#define WATCHDOG_H

void watchdog_init(void);
void watchdog_feed(void);
/* Progress from a bounded wait owned by the main kernel thread (e.g. virtio I/O). */
void watchdog_progress(void);
void watchdog_check(void);
void watchdog_trigger_panic(const char* reason);

#endif
