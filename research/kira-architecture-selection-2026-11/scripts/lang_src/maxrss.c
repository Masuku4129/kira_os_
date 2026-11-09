/* замена /usr/bin/time -v (на машине нет GNU time): запускает команду, wait4() -> ru_maxrss (КБ) в файл argv[1]. Тот же источник, что у time -v. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int c, char **v) {
    if (c < 3) return 2;
    pid_t p = fork();
    if (!p) { execvp(v[2], v + 2); _exit(127); }
    int st; struct rusage ru; wait4(p, &st, 0, &ru);
    FILE *f = fopen(v[1], "w"); fprintf(f, "Maximum resident set size (kbytes): %ld\n", ru.ru_maxrss); fclose(f);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
