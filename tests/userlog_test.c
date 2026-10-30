// Хост-тест журнала вывода программ: строка собирается по процессу и выводится целиком с одним префиксом.
#include <stdio.h>
#include <string.h>
#include "userlog.h"

static char out[8192]; static size_t on; static int lines;
static void emit(const char* l) { size_t n = strlen(l); memcpy(out + on, l, n); on += n; out[on] = 0; lines++; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static userlog_t s;
static void reset(void) { memset(&s, 0, sizeof s); on = 0; out[0] = 0; lines = 0; }

int main(void) {
    reset();
    userlog_write(&s, 5, "burn: finished, iterations ", 27, emit);
    CHECK(lines == 0);                                                           // незаконченная строка ещё не выведена
    userlog_write(&s, 5, "123", 3, emit);
    userlog_write(&s, 5, "\n", 1, emit);
    CHECK(strcmp(out, "[USER pid 5] burn: finished, iterations 123\n") == 0);   // один префикс на строку

    reset();                                                                     // два процесса пишут вперемешку (разные ядра)
    userlog_write(&s, 2, "burn: finished, iterations ", 27, emit);
    userlog_write(&s, 3, "burn: finished, iterations 95662080\n", 36, emit);
    userlog_write(&s, 2, "91725824\n", 9, emit);
    CHECK(strcmp(out, "[USER pid 3] burn: finished, iterations 95662080\n[USER pid 2] burn: finished, iterations 91725824\n") == 0);

    reset();
    userlog_write(&s, 10, "x\ny\n", 4, emit);
    CHECK(strcmp(out, "[USER pid 10] x\n[USER pid 10] y\n") == 0);              // несколько строк в одном вызове
    userlog_write(&s, 3, "", 0, emit);
    CHECK(lines == 2);                                                           // пустая запись ничего не печатает

    reset();                                                                     // процесс завершился посреди строки
    userlog_write(&s, 7, "tail", 4, emit);
    userlog_flush(&s, 7, emit);
    CHECK(strcmp(out, "[USER pid 7] tail\n") == 0);
    userlog_flush(&s, 7, emit); CHECK(lines == 1);

    reset();                                                                     // длинная строка - кусками, ничего не теряется
    char longl[400]; memset(longl, 'a', sizeof longl);
    userlog_write(&s, 4, longl, sizeof longl, emit);
    userlog_flush(&s, 4, emit);
    CHECK(lines == 3);
    size_t as = 0; for (size_t i = 0; i < on; i++) if (out[i] == 'a') as++;
    CHECK(as == 400);

    reset();                                                                     // больше процессов, чем мест: давний выталкивается целым куском
    for (uint32_t p = 1; p <= USERLOG_SLOTS + 3; p++) userlog_write(&s, p, "part", 4, emit);
    CHECK(lines == 3);
    for (uint32_t p = 1; p <= USERLOG_SLOTS + 3; p++) userlog_flush(&s, p, emit);
    CHECK(lines == USERLOG_SLOTS + 3);

    if (fails) { printf("userlog: %d FAILED\n", fails); return 1; }
    printf("userlog: whole lines per process, interleaved writers, exit flush, long lines, slot pressure passed\n");
    return 0;
}
