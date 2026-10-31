/* ARM64: указатель на память ядра в системный вызов write - ядро обязано отказать. Версия x86-64 - atk-sys.s. */
#include "arm-atk.h"
KIRA_ENTRY()
int umain(void) {
    i64 r = ksys(K_WRITE, 0x40000000ull, 64, 0);
    if (r == -1) { say("kernel pointer passed to write() was rejected (PASS)\n"); return 0; }
    say("write() accepted a kernel pointer (FAIL)\n");
    return 1;
}
