/* Нагрузка на процессор на 3 секунды. Счётчик итераций показывает, сколько процессорного времени получил
   процесс: два одновременных экземпляра с близкими числами - справедливое вытесняющее планирование.
   Время проверяется раз в 4096 итераций, чтобы цикл оставался процессорным, а не системным вызовом. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    char o[21]; int i = 0; while (n) o[i++] = t[--n];
    k_write(o, (u64)i);
}

int umain(void) {
    volatile u64 n = 0;
    u64 end = k_ticks() + 300;
    do {
        for (u32 i = 0; i < 4096; i++) n = n + 1;
    } while (k_ticks() < end);
    say("burn: finished, iterations "); say_u(n); say("\n");
    return 0;
}
