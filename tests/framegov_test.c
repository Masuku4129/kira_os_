// Хост-тест регулятора анимации фона (framegov.c): авто-снижение при опозданиях, возврат после покоя, ручной уровень.
#include "framegov.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    framegov_t g; framegov_init(&g);
    CHECK(g.fx == 3);
    for (int i = 0; i < 10; i++) framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.fx == 3);
    framegov_second(&g, 60, 6, FGOV_FX_AUTO); framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 3);
    framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 2);
    for (int i = 0; i < 9; i++) framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 0);
    for (int i = 0; i < 3; i++) framegov_second(&g, 60, 6, FGOV_FX_AUTO);
    CHECK(g.fx == 0);                                           /* ниже нуля не идёт */
    for (int i = 0; i < 29; i++) framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.fx == 0);
    framegov_second(&g, 60, 0, FGOV_FX_AUTO);
    CHECK(g.fx == 1);
    framegov_second(&g, 0, 0, FGOV_FX_AUTO); CHECK(g.fx == 1);   /* секунда без кадров не считается */
    framegov_second(&g, 60, 30, 2); CHECK(g.fx == 2);            /* вручную - как задано */
    for (int i = 0; i < 5; i++) framegov_second(&g, 60, 30, 2);
    CHECK(g.fx == 2);
    framegov_second(&g, 60, 0, 0); CHECK(g.fx == 0);
    printf(fails ? "FRAMEGOV TESTS FAILED\n" : "framegov: animation levels OK\n");
    return fails != 0;
}
