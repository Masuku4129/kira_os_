/* Запуск команды с замером пикового RSS потомка (ru_maxrss через wait4). Нужен потому, что у процесса, запущенного из Python, ru_maxrss наследует RSS родителя (~37 МиБ) через vfork/exec.
   Использование: fmt_rss prog args... ; в конце stderr печатает "MAXRSS_KB n". Код возврата - потомка. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int c, char** v) {
    if (c < 2) return 2;
    pid_t p = fork(); if (p == 0) { execvp(v[1], v + 1); _exit(127); }
    int st; struct rusage ru; if (wait4(p, &st, 0, &ru) < 0) return 3;
    fprintf(stderr, "MAXRSS_KB %ld\n", ru.ru_maxrss); return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
