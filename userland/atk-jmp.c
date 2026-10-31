/* ARM64: переход на код ядра (ОЗУ ядра - PXN/UXN для EL0) -> ошибка выборки команды, остановка. Четвёртая атака на
   ARM64 вместо atk-gs (у ARM64 нет сегментов). */
#include "arm-atk.h"
KIRA_ENTRY()
int umain(void) {
    say("attack: jumping into kernel memory 0x40080000 ...\n");
    void (*f)(void) = (void (*)(void))0x40080000ull;
    f();
    say("kernel code executed from EL0 (FAIL)\n");
    return 1;
}
