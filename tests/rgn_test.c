#include "../rgn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
enum { G = 40 };
static void paint(unsigned char (*g)[G], const rgn_rect_t* r, int v) { for (int y = r->y0; y < r->y1; y++) for (int x = r->x0; x < r->x1; x++) if (x >= 0 && y >= 0 && x < G && y < G) g[y][x] += (unsigned char)v; }
static rgn_rect_t rnd(void) { int x0 = rand() % G, y0 = rand() % G; return (rgn_rect_t){ x0, y0, x0 + rand() % 25, y0 + rand() % 25 }; }
int main(void) {
    rgn_rect_t a = { 0, 0, 10, 10 }, b = { 5, 5, 20, 20 }, o;
    CHECK(rgn_intersect(&a, &b, &o) && o.x0 == 5 && o.y0 == 5 && o.x1 == 10 && o.y1 == 10);
    rgn_rect_t c = { 10, 0, 20, 10 };
    CHECK(!rgn_intersect(&a, &c, &o));                              // касание по краю - не пересечение
    rgn_rect_t e = { 3, 3, 3, 9 };
    CHECK(!rgn_intersect(&a, &e, &o));                              // пустой прямоугольник

    rgn_rect_t r[RGN_MAX] = { { 0, 0, 10, 10 } };
    rgn_rect_t cov = { 20, 20, 30, 30 };
    CHECK(rgn_subtract(r, 1, RGN_MAX, &cov) == 1 && r[0].x1 == 10);        // не задето
    rgn_rect_t cov2 = { -5, -5, 50, 50 };
    CHECK(rgn_subtract(r, 1, RGN_MAX, &cov2) == 0);                        // закрыто целиком
    r[0] = (rgn_rect_t){ 0, 0, 10, 10 };
    rgn_rect_t cov3 = { 3, 3, 6, 6 };
    CHECK(rgn_subtract(r, 1, RGN_MAX, &cov3) == 4);                        // дыра посередине: четыре части

    srand(12345);
    for (int it = 0; it < 20000; it++) {                                   // точное сравнение с растром: части не пересекаются, объединение = было минус закрытое
        rgn_rect_t s[RGN_MAX]; int n = 1 + rand() % 4;
        unsigned char before[G][G]; memset(before, 0, sizeof before);
        int m = 0;
        for (int i = 0; i < n; i++) { rgn_rect_t q = rnd(); if (q.x1 <= q.x0 || q.y1 <= q.y0) continue; int ok = 1; for (int j = 0; j < m; j++) { rgn_rect_t t; if (rgn_intersect(&s[j], &q, &t)) ok = 0; } if (ok) s[m++] = q; }
        for (int i = 0; i < m; i++) paint(before, &s[i], 1);
        rgn_rect_t cv = rnd();
        int k = rgn_subtract(s, m, RGN_MAX, &cv);
        unsigned char after[G][G]; memset(after, 0, sizeof after);
        for (int i = 0; i < k; i++) { CHECK(s[i].x0 < s[i].x1 && s[i].y0 < s[i].y1); paint(after, &s[i], 1); }
        unsigned char cm[G][G]; memset(cm, 0, sizeof cm); paint(cm, &cv, 1);
        for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) {
            unsigned want = cm[y][x] ? 0 : before[y][x];
            if (after[y][x] != want) { printf("FAIL it %d at %d,%d: %d want %u\n", it, x, y, after[y][x], want); return 1; }
        }
    }
    {   // переполнение: набор остаётся прежним, ничего не теряется
        rgn_rect_t s[RGN_MAX] = { { 0, 0, 10, 10 } };
        rgn_rect_t cv = { 4, 4, 6, 6 };
        CHECK(rgn_subtract(s, 1, 3, &cv) == 1 && s[0].x0 == 0 && s[0].x1 == 10 && s[0].y1 == 10);
    }
    if (fails) return 1;
    printf("rgn: OK\n");
    return 0;
}
