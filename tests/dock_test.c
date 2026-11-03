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
    CHECK(d.n == 5 && !strcmp(d.name[0], "plasma") && dock_find(&d, "cube3d") == 2 && dock_find(&d, "edit") == 3 && dock_find(&d, "drvman") == 4);
    CHECK(dock_icon("drvman") == ICON_CHIP && dock_title("drvman") && !strcmp(dock_title("drvman"), "Драйверы") && !strcmp(dock_title("paint"), "Рисование") && !dock_title("zork"));
    /* единая таблица программ: названия по заданию, у каждой программы свой значок, неизвестная - общий значок и своё имя */
    static const struct { const char* sys; const char* title; } want[] = {
        { "plasma", "Плазма" }, { "paint", "Рисование" }, { "cube3d", "3D-куб" }, { "wasm", "Запуск WebAssembly" }, { "edit", "Текстовый редактор" },
        { "view", "Просмотр" }, { "fetch", "Загрузка из сети" }, { "serve", "Веб-сервер" }, { "rec", "Запись звука" }, { "net6", "Сеть IPv6" },
        { "kcc", "Компилятор C" }, { "drvget", "Установка драйверов (консоль)" }, { "drvman", "Драйверы" },
    };
    int nw = (int)(sizeof want / sizeof want[0]);
    for (int i = 0; i < nw; i++) {
        const app_info_t* a = app_info(want[i].sys);
        CHECK(a && !strcmp(a->title, want[i].title) && !strcmp(app_label(want[i].sys), want[i].title) && app_desc(want[i].sys)[0]);
        CHECK(dock_icon(want[i].sys) == a->icon && dock_color(want[i].sys) == a->color && a->icon > ICON_TERMINAL && a->icon < ICON_LAST);
        for (int j = 0; j < i; j++) CHECK(dock_icon(want[i].sys) != dock_icon(want[j].sys));   /* у каждой свой значок */
    }
    for (int i = 0; i < app_info_count(); i++) {                                  /* вся таблица: корректные ключи, без повторов, названия влезают в заголовок окна */
        const app_info_t* a = app_info_at(i);
        CHECK(dock_valid_name(a->sys) && a->title[0] && strlen(a->title) < 63 && a->desc[0] && a->icon >= ICON_TERMINAL && a->icon < ICON_LAST);
        for (int j = 0; j < i; j++) CHECK(strcmp(app_info_at(j)->sys, a->sys));
    }
    CHECK(app_info_at(-1) == 0 && app_info_at(app_info_count()) == 0);
    CHECK(app_info("zork") == 0 && dock_icon("zork") == ICON_TERMINAL && app_label("zork") && !strcmp(app_label("zork"), "zork") && !strcmp(app_desc("zork"), ""));
    { const char* nm = "xyzzy"; CHECK(app_label(nm) == nm); }                      /* своё имя как есть */
    CHECK(dock_icon("atk-mem") == ICON_AI && dock_icon("") == ICON_TERMINAL);
    CHECK(!app_info("Paint") && !app_info("paint "));                             /* ключ - точное системное имя */
    /* терминал и консоль в таблицу не входят: остаются с системными именами */
    CHECK(!app_info("Console") && !app_info("Files") && !app_info("Mind") && !app_info("Monitor"));
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
    CHECK(dock_icon("cube3d") == ICON_CUBE && dock_icon("burn") == ICON_FLAME && dock_icon("myprog") == ICON_TERMINAL && dock_color("myprog") == dock_color("myprog"));
    char junk[300]; srand(5);
    for (int k = 0; k < 20000; k++) { for (int i = 0; i < 300; i++) junk[i] = (char)(rand() % 128); dock_parse(&d, junk, (unsigned)(rand() % 300)); CHECK(d.n <= DOCK_MAX); dock_format(&d, out, sizeof out); }
    printf("dock: parse (junk, duplicates, long names, CRLF), format round trip, add/remove, limit, fuzz passed\n");
    return 0;
}
