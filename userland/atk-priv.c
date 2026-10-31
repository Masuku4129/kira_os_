/* ARM64: привилегированная команда в EL0 (чтение SCTLR_EL1) -> исключение «неизвестная команда», остановка.
   Версия x86-64 - atk-priv.s (HLT в Ring 3). */
#include "arm-atk.h"
KIRA_ENTRY()
int umain(void) {
    say("attack: reading SCTLR_EL1 from EL0 ...\n");
    u64 v; __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(v));
    (void)v;
    say("SCTLR_EL1 readable from EL0 (FAIL)\n");
    return 1;
}
