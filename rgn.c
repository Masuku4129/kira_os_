#include "rgn.h"

int rgn_intersect(const rgn_rect_t* a, const rgn_rect_t* b, rgn_rect_t* out) {
    rgn_rect_t t = { a->x0 > b->x0 ? a->x0 : b->x0, a->y0 > b->y0 ? a->y0 : b->y0, a->x1 < b->x1 ? a->x1 : b->x1, a->y1 < b->y1 ? a->y1 : b->y1 };
    if (t.x0 >= t.x1 || t.y0 >= t.y1) return 0;
    *out = t;
    return 1;
}

int rgn_subtract(rgn_rect_t* r, int n, int cap, const rgn_rect_t* cover) {
    if (cap > RGN_MAX) cap = RGN_MAX;
    rgn_rect_t out[RGN_MAX];
    int m = 0;
    for (int i = 0; i < n; i++) {
        rgn_rect_t in;
        if (!rgn_intersect(&r[i], cover, &in)) {                  // не задето: остаётся целиком
            if (m >= cap) return n;
            out[m++] = r[i];
            continue;
        }
        // вокруг пересечения до четырёх частей: полоса сверху, полоса снизу, слева и справа от пересечения
        rgn_rect_t p[4] = {
            { r[i].x0, r[i].y0, r[i].x1, in.y0 },
            { r[i].x0, in.y1, r[i].x1, r[i].y1 },
            { r[i].x0, in.y0, in.x0, in.y1 },
            { in.x1, in.y0, r[i].x1, in.y1 },
        };
        for (int k = 0; k < 4; k++) {
            if (p[k].x0 >= p[k].x1 || p[k].y0 >= p[k].y1) continue;
            if (m >= cap) return n;
            out[m++] = p[k];
        }
    }
    for (int i = 0; i < m; i++) r[i] = out[i];
    return m;
}
