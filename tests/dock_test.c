// Быстрый доступ: разбор /var/dock (мусор, повторы, длинные имена, CRLF), запись, добавление/удаление, предел 10.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dock.h"
#include "icons.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int main(void) {
    dock_t d; char out[256];
    dock_default(&d);
    CHECK(d.n == 3 && !strcmp(d.name[0], "plasma") && dock_find(&d, "cube3d") == 2);
    const char* t = "burn\r\nplasma\n\nBAD NAME\nplasma\nverylongprogramname1\nmt\natk-mem\n";
    CHECK(dock_parse(&d, t, (unsigned)strlen(t)) == 4);
    CHECK(!strcmp(d.name[0], "burn") && !strcmp(d.name[1], "plasma") && !strcmp(d.name[2], "mt") && !strcmp(d.name[3], "atk-mem"));
    unsigned n = dock_format(&d, out, sizeof out);
    CHECK(n == strlen("burn\nplasma\nmt\natk-mem\n") && !strcmp(out, "burn\nplasma\nmt\natk-mem\n"));
    dock_t e; CHECK(dock_parse(&e, out, n) == 4); for (int i = 0; i < 4; i++) CHECK(!strcmp(e.name[i], d.name[i]));       /* туда и обратно */
    CHECK(dock_add(&d, "mt") == 1 && dock_add(&d, "") == -1 && dock_add(&d, "a/b") == -1);
    CHECK(dock_remove(&d, "plasma") == 0 && dock_remove(&d, "plasma") == -1 && d.n == 3 && !strcmp(d.name[1], "mt"));
    for (int i = 0; i < 20; i++) { char nm[16]; snprintf(nm, sizeof nm, "p%d", i); dock_add(&d, nm); }
    CHECK(d.n == DOCK_MAX && dock_add(&d, "zzz") == -1);
    CHECK(dock_parse(&d, "", 0) == 0 && d.n == 0);
    CHECK(dock_icon("cube3d") == ICON_CUBE && dock_icon("burn") == ICON_TERMINAL && dock_color("burn") == dock_color("burn"));
    char junk[300]; srand(5);
    for (int k = 0; k < 20000; k++) { for (int i = 0; i < 300; i++) junk[i] = (char)(rand() % 128); dock_parse(&d, junk, (unsigned)(rand() % 300)); CHECK(d.n <= DOCK_MAX); dock_format(&d, out, sizeof out); }
    printf("dock: parse (junk, duplicates, long names, CRLF), format round trip, add/remove, limit, fuzz passed\n");
    return 0;
}
