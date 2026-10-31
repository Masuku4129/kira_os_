/* ARM64: проверка изоляции адресных пространств (версия x86-64 - demo.s). Две копии пишут свой pid по одному и
   тому же адресу (глобальная переменная - одинаковый виртуальный адрес у обеих) и проверяют, что чужой не затёр. */
#include "kira.h"
KIRA_ENTRY()
static volatile u64 slot;
static void say(const char* s) { k_write(s, k_strlen(s)); }
int umain(void) {
    u64 pid = k_getpid();
    slot = pid;
    say("demo: wrote my pid to a shared virtual address\n");
    for (int i = 0; i < 3; i++) {
        k_sleep(30);
        if (slot != pid) { say("demo: isolation broken - another process changed my memory (FAIL)\n"); return 1; }
    }
    say("demo: address spaces are isolated (PASS)\n");
    return 0;
}
