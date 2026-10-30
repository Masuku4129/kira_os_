#include "gfxui.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
static int near(int a, int b, int tol) { return abs(a - b) <= tol; }
int main(void) {
    /* углы: 0 - вверх, по часовой стрелке */
    CHECK(ui_angle10(0, -10) == 0 && ui_angle10(10, 0) == 900 && ui_angle10(0, 10) == 1800 && ui_angle10(-10, 0) == 2700);
    CHECK(near(ui_angle10(10, -10), 450, 3) && near(ui_angle10(10, 10), 1350, 3) && near(ui_angle10(-10, 10), 2250, 3) && near(ui_angle10(-10, -10), 3150, 3));
    CHECK(near(ui_angle10(7, -3), 668, 4) && near(ui_angle10(3, -7), 232, 4));
    int worst = 0;                                        /* по всей окружности ошибка не больше полградуса */
    static const int ex[3] = { 1, 2, 3 }; (void)ex;
    for (int a = 0; a < 3600; a += 7) {
        double rad = a * 3.14159265358979 / 1800.0;
        int dx = (int)(10000 * __builtin_sin(rad)), dy = (int)(-10000 * __builtin_cos(rad));
        int d = abs(ui_angle10(dx, dy) - a); if (d > 1800) d = 3600 - d;
        if (d > worst) worst = d;
    }
    CHECK(worst <= 5);
    /* дуга: 25% закрашивает верхне-правую четверть и не трогает остальные */
    uint32_t buf[100 * 100] = {0};
    surface_t s = { buf, 100, 100, 100 };
    ui_arc(&s, 50, 50, 30, 40, 0, 900, 0x00FFFFFF);
    CHECK(buf[(50 - 35) * 100 + 50 + 6] == 0x00FFFFFF);   /* вверху справа от оси */
    CHECK(buf[(50 + 35) * 100 + 50 - 6] == 0);            /* внизу слева */
    CHECK(buf[(50 - 35) * 100 + 50 - 6] == 0);            /* вверху слева */
    CHECK(buf[50 * 100 + 50] == 0);                       /* центр пустой */
    /* круг и кольцо сглажены: на границе есть промежуточные цвета, внутри сплошной */
    for (int i = 0; i < 10000; i++) buf[i] = 0;
    ui_disc(&s, 50, 50, 20, 0x00FF0000, 255);
    CHECK(buf[50 * 100 + 50] == 0x00FF0000);
    int mid = 0, full = 0; for (int i = 0; i < 10000; i++) { if (buf[i] == 0x00FF0000) full++; else if (buf[i]) mid++; }
    CHECK(mid >= 40 && mid <= 160);                       /* кромка сглажена, но тонкая */
    CHECK(full > 1100 && full < 1300);                    /* площадь круга r=20: ~1257 */
    CHECK(buf[0] == 0 && buf[50 * 100 + 75] == 0);
    /* скруглённый прямоугольник: угол срезан, центр залит, рисование за краем не падает */
    for (int i = 0; i < 10000; i++) buf[i] = 0;
    ui_round_rect(&s, 10, 10, 80, 60, 12, 0x0000FF00, 255);
    CHECK(buf[10 * 100 + 10] == 0 && buf[40 * 100 + 50] == 0x0000FF00 && buf[10 * 100 + 50] == 0x0000FF00);
    ui_round_rect(&s, -30, -30, 400, 400, 12, 0x000000FF, 128);
    ui_round_frame(&s, -5, -5, 200, 200, 12, 0x00FFFFFF, 255);
    /* прозрачность: смешение с фоном */
    for (int i = 0; i < 10000; i++) buf[i] = 0x00000000;
    ui_blend(&s, 5, 5, 0x00FFFFFF, 128);
    CHECK(near((int)(buf[5 * 100 + 5] & 255), 128, 2));
    printf("ui: angles, arcs, anti-aliased discs, rounded rects, alpha passed\n");
    return 0;
}
