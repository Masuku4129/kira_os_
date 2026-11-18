// Единая тема: контраст текста и состояний к поверхностям не ниже 4,5:1 (WCAG AA), три состояния различимы.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "theme.h"
static double lin(int c) { double v = c / 255.0; return v <= 0.03928 ? v / 12.92 : __builtin_pow((v + 0.055) / 1.055, 2.4); }
static double lum(uint32_t c) { return 0.2126 * lin((c >> 16) & 255) + 0.7152 * lin((c >> 8) & 255) + 0.0722 * lin(c & 255); }
static double cr(uint32_t a, uint32_t b) { double x = lum(a), y = lum(b); if (x < y) { double t = x; x = y; y = t; } return (x + 0.05) / (y + 0.05); }
int main(void) {
    const uint32_t fg[] = { TH_FG, TH_FG_DIM, TH_FG_FAINT, TH_ACCENT, TH_OK, TH_WARN, TH_ALARM };
    const char* fn[] = { "FG", "FG_DIM", "FG_FAINT", "ACCENT", "OK", "WARN", "ALARM" };
    const uint32_t bg[] = { TH_SURF_DESK, TH_SURF_BAR, TH_SURF_WIN, TH_SURF_HEAD, TH_SURF_CONTENT, TH_SURF_ROW_ALT, TH_SURF_TRACK };
    const char* bn[] = { "DESK", "BAR", "WIN", "HEAD", "CONTENT", "ROW_ALT", "TRACK" };
    double worst = 99; int bad = 0;
    for (int i = 0; i < 7; i++) for (int j = 0; j < 7; j++) {
        double c = cr(fg[i], bg[j]); if (c < worst) worst = c;
        if (c < 4.5) { printf("FAIL: %s на %s = %.2f < 4.5\n", fn[i], bn[j], c); bad++; }
    }
    if (TH_OK == TH_WARN || TH_WARN == TH_ALARM || TH_OK == TH_ALARM || TH_ACCENT == TH_OK || TH_ACCENT == TH_ALARM) { printf("FAIL: состояния совпадают\n"); bad++; }
    if (TH_BAR_H < 24 || TH_ROW_H < 20) { printf("FAIL: цели клика меньше нормы\n"); bad++; }
    printf("theme contrast: худший %.2f:1, %s\n", worst, bad ? "FAIL" : "OK");
    return bad != 0;
}
