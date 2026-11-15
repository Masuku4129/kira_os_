// Хост-тест регулятора темпа кадров компоновщика (framegov.c): ступени темпа фоновых окон и анимация фона.
#include "framegov.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    framegov_t g; framegov_init(&g);
    CHECK(g.bg_fps == 0 && g.fx == 3);
    /* спокойно: ничего не меняется */
    for (int i = 0; i < 10; i++) CHECK(!framegov_second(&g, 60, 0, FGOV_FX_AUTO));
    /* опоздания каждую секунду: 30, 15, 8, 4 и дальше не ниже */
    static const unsigned want[] = { 30, 15, 8, 4 };
    for (int i = 0; i < 4; i++) { CHECK(framegov_second(&g, 60, 5, FGOV_FX_AUTO) == 1 && g.bg_fps == want[i]); }
    CHECK(!framegov_second(&g, 60, 5, FGOV_FX_AUTO) && g.bg_fps == 4);
    /* одна спокойная секунда не отпускает; две - ступень выше */
    CHECK(!framegov_second(&g, 60, 0, FGOV_FX_AUTO) && g.bg_fps == 4);
    CHECK(framegov_second(&g, 60, 0, FGOV_FX_AUTO) && g.bg_fps == 8);
    /* опоздание посреди спокойствия сбрасывает счёт */
    CHECK(!framegov_second(&g, 60, 0, FGOV_FX_AUTO));
    CHECK(framegov_second(&g, 60, 1, FGOV_FX_AUTO) && g.bg_fps == 4);
    for (int i = 0; i < 8; i++) framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.bg_fps == 0);                                       /* вернулся без предела */
    /* секунда без кадров не считается */
    CHECK(!framegov_second(&g, 0, 0, FGOV_FX_AUTO) && g.bg_fps == 0);
    /* анимация (авто): 3 секунды по 10% - уровень ниже; 30 спокойных - выше */
    framegov_init(&g);
    framegov_second(&g, 60, 6, FGOV_FX_AUTO); framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 3);
    framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 2);
    for (int i = 0; i < 9; i++) framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 0);
    framegov_second(&g, 60, 6, FGOV_FX_AUTO); framegov_second(&g, 60, 6, FGOV_FX_AUTO); framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 0);                                           /* ниже нуля не идёт */
    for (int i = 0; i < 29; i++) framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.fx == 0);
    framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.fx == 1);
    /* вручную - как задано, без адаптации */
    framegov_second(&g, 60, 30, 2); CHECK(g.fx == 2);
    for (int i = 0; i < 5; i++) framegov_second(&g, 60, 30, 2);
    CHECK(g.fx == 2);
    framegov_second(&g, 60, 0, 0); CHECK(g.fx == 0);
    printf(fails ? "FRAMEGOV TESTS FAILED\n" : "framegov: bg pacing ladder and animation levels OK\n");
    return fails != 0;
}
