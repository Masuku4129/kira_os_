/* printf/snprintf с вещественными: %f %e %g, ширина, точность, флаги, округление до чётного, inf/nan, граничные значения. Вывод сверяется с glibc. */
#define KCC_PRINTF_FLOAT 1
#include <stdio.h>
static double zero = 0.0;
int main(void) {
    static const double v[] = { 0.0, -0.0, 1.0, -1.5, 0.5, 1.5, 2.5, 0.125, 0.375, 3.14159265358979, 1e-5, 123456.789, 1e15, 1e16, 123456789012345678.0,
        5e-324, 2.2250738585072014e-308, 0.1, 0.2 + 0.1, 9.9999995, 99999.95, 0.95, 0.05, 1e22, 4.35, 1.005, 2.675, 100.0, 999999.4, 0.000123456 };
    for (int i = 0; i < (int)(sizeof v / sizeof v[0]); i++) {
        printf("%f|%.0f|%.2f|%e|%.0e|%.3E|%g|%G|%.3g|%.10g\n", v[i], v[i], v[i], v[i], v[i], v[i], v[i], v[i], v[i], v[i]);
        printf("[%12.4f][%-12.2f][%+.1f][% .1f][%012.3f][%#.0f][%#g][%10.3e]\n", v[i], v[i], v[i], v[i], v[i], v[i], v[i], v[i]);
    }
    printf("%f\n%e\n%g\n", 1e100, 1e100, 1e100);
    printf("%f\n%.3e\n%g\n", 1.7976931348623157e308, 1.7976931348623157e308, 1.7976931348623157e308);
    printf("%.300f\n", 5e-324);
    double inf = 1.0 / zero, nan_ = zero / zero;
    printf("%f %f %F %e %g %G %5.1f|%-6f|%06f\n", inf, -inf, inf, inf, nan_, nan_, inf, inf, inf);
    printf("%.20f\n%.30e\n%.40g\n", 0.1, 0.1, 1.0 / 3);
    printf("%.0f %.0f %.0f %.0f %.1f %.1f\n", 0.5, 1.5, 2.5, 3.5, 0.25, 0.35);
    printf("%5.2f%%|%*.*f|%-*.1f|\n", 99.5, 10, 3, 2.0 / 3, 8, 7.77);
    char b[32];
    int r = snprintf(b, sizeof b, "%.3f", 1234.56789);
    printf("%d %s\n", r, b);
    r = snprintf(b, 8, "%f", 1e10);
    printf("%d %s\n", r, b);
    printf("%f\n", (float)0.1f);
    printf("%d %f %s %c %f %ld %f\n", 42, 1.25, "str", 'x', 2.5f, 123456789012L, -0.001);
    return 0;
}
