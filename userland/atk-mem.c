/* ARM64: запись в память ядра (ОЗУ 0x40000000 - в карте программы есть, но только для EL1) -> ошибка доступа,
   программа останавливается, ядро живо. Версия x86-64 - atk-mem.s. */
#include "arm-atk.h"
KIRA_ENTRY()
int umain(void) {
    say("attack: writing to kernel memory 0x40000000 ...\n");
    *(volatile u64*)0x40000000ull = 0x9090909090909090ull;
    say("kernel memory was writable from EL0 (FAIL)\n");
    return 1;
}
