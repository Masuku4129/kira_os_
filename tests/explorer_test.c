// Дерево проводника: каталоги первыми, имена по порядку, раскрытые каталоги вложены, предел строк, глубина.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "explorer.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

typedef struct { const char* path; int dir; uint64_t size; } fent_t;
static fent_t FS[6000]; static int nfs;
static int fails_list;
static int lister(const char* path, void (*emit)(void*, const char*, int, uint64_t), void* ctx) {
    if (!strcmp(path, "/broken")) { fails_list++; return -1; }
    size_t pl = strlen(path); if (pl == 1) pl = 0;
    for (int i = 0; i < nfs; i++) {
        const char* p = FS[i].path;
        if (strncmp(p, path, pl) || p[pl] != '/' || strchr(p + pl + 1, '/')) continue;
        emit(ctx, p + pl + 1, FS[i].dir, FS[i].size);
    }
    return 0;
}
static void add(const char* p, int dir, uint64_t size) { FS[nfs++] = (fent_t){ strdup(p), dir, size }; }

int main(void) {
    add("/zeta.txt", 0, 10); add("/bin", 1, 0); add("/alpha", 0, 5); add("/var", 1, 0); add("/broken", 1, 0);
    add("/bin/b", 0, 1); add("/bin/a", 0, 2); add("/var/mind", 1, 0); add("/var/mind/x", 0, 3); add("/var/log", 0, 4);
    static exp_row_t rows[EXP_ROWS]; static char paths[EXP_ROWS][128];
    char open[8][128]; uint32_t tr;
    uint32_t n = exp_build(lister, open, 0, rows, paths, EXP_ROWS, &tr);
    CHECK(n == 5 && !tr);
    CHECK(!strcmp(rows[0].name, "bin") && !strcmp(rows[1].name, "broken") && !strcmp(rows[2].name, "var"));   /* каталоги первыми */
    CHECK(!strcmp(rows[3].name, "alpha") && !strcmp(rows[4].name, "zeta.txt") && rows[4].size == 10);
    CHECK(!strcmp(paths[2], "/var") && rows[0].depth == 0 && !rows[0].open);

    strcpy(open[0], "/var"); strcpy(open[1], "/var/mind"); strcpy(open[2], "/broken");
    n = exp_build(lister, open, 3, rows, paths, EXP_ROWS, &tr);
    CHECK(n == 8);
    CHECK(rows[1].open && rows[2].open && !strcmp(rows[3].name, "mind") && rows[3].depth == 1 && rows[3].open);
    CHECK(!strcmp(paths[4], "/var/mind/x") && rows[4].depth == 2);
    CHECK(!strcmp(rows[5].name, "log") && rows[5].depth == 1);
    CHECK(fails_list == 1);                                         /* сбой чтения каталога: строка есть, вложенных нет */

    /* предел строк: обход останавливается, признак усечения выставлен */
    for (int i = 0; i < EXP_DIR_MAX + 100; i++) { char p[32]; snprintf(p, sizeof p, "/var/f%05d", i); add(p, 0, (uint64_t)i << 23); }   /* размеры больше 4 ГБ */
    for (int i = 0; i < EXP_DIR_MAX; i++) { char p[32]; snprintf(p, sizeof p, "/bin/g%05d", i); add(p, 0, 1); }
    strcpy(open[3], "/bin");
    n = exp_build(lister, open, 4, rows, paths, EXP_ROWS, &tr);
    CHECK(n == EXP_ROWS && tr);
    /* в одном каталоге - не больше EXP_DIR_MAX имён, усечение отмечено */
    n = exp_build(lister, open, 1, rows, paths, EXP_ROWS, &tr);
    int under_var = 0; uint64_t maxsz = 0; for (uint32_t i = 0; i < n; i++) if (rows[i].depth == 1) { under_var++; if (rows[i].size > maxsz) maxsz = rows[i].size; }
    if (under_var != EXP_DIR_MAX || !tr || maxsz <= (1ULL << 32)) printf("under=%d tr=%u max=%llu n=%u\n", under_var, tr, (unsigned long long)maxsz, n);
    CHECK(under_var == EXP_DIR_MAX && tr && maxsz > (1ULL << 32));
    /* глубокая вложенность не ломает обход */
    nfs = 0; char p[128] = ""; char opens[10][128]; int no = 0;
    for (int d = 0; d < 12; d++) { strcat(p, "/d"); add(p, 1, 0); if (no < 10) strcpy(opens[no++], p); }
    n = exp_build(lister, opens, no, rows, paths, EXP_ROWS, &tr);
    CHECK(n == 10 && rows[9].depth == 9);
    printf("explorer: sort (dirs first), nested open dirs, failing dir, row limit, EXP_DIR_MAX per dir, >4 GB sizes, depth limit passed\n");
    return 0;
}
