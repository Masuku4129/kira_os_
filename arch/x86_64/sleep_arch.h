#ifndef SLEEP_ARCH_H
#define SLEEP_ARCH_H
/* x86-64: связь сна S3 с запуском ядер (smp.c). Уровень 2. */
#include <stdint.h>
int  sleep_park_pending(void);    /* 1 - загрузочное ядро просит остальные встать на время сна */
void sleep_ap_park(void);         /* вызывается ядром с выключенными прерываниями из своего холостого цикла */
#endif
