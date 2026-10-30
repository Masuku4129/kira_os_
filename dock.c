#include "dock.h"
#include "icons.h"
#include "theme.h"

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int dock_valid_name(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= DOCK_NAME - 1 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return n > 0;
}

int dock_find(const dock_t* d, const char* name) {
    for (int i = 0; i < d->n; i++) if (same(d->name[i], name)) return i;
    return -1;
}

int dock_add(dock_t* d, const char* name) {
    if (!dock_valid_name(name)) return -1;
    if (dock_find(d, name) >= 0) return 1;
    if (d->n >= DOCK_MAX) return -1;
    int i = 0; while (name[i]) { d->name[d->n][i] = name[i]; i++; } d->name[d->n][i] = 0;
    d->n++;
    return 0;
}

int dock_remove(dock_t* d, const char* name) {
    int k = dock_find(d, name);
    if (k < 0) return -1;
    for (int i = k; i + 1 < d->n; i++) for (int j = 0; j < DOCK_NAME; j++) d->name[i][j] = d->name[i + 1][j];
    d->n--;
    return 0;
}

void dock_default(dock_t* d) { d->n = 0; dock_add(d, "plasma"); dock_add(d, "paint"); dock_add(d, "cube3d"); }

int dock_parse(dock_t* d, const char* t, unsigned len) {
    d->n = 0;
    char cur[DOCK_NAME + 1]; unsigned k = 0; int bad = 0;
    for (unsigned i = 0; i <= len; i++) {
        char c = i < len ? t[i] : '\n';
        if (c == '\n' || c == '\r') {
            cur[k] = 0;
            if (k && !bad) dock_add(d, cur);
            k = 0; bad = 0;
        } else if (k < DOCK_NAME) cur[k++] = c;
        else bad = 1;
    }
    return d->n;
}

unsigned dock_format(const dock_t* d, char* out, unsigned cap) {
    unsigned p = 0;
    for (int i = 0; i < d->n; i++) {
        for (int j = 0; d->name[i][j] && p + 2 < cap; j++) out[p++] = d->name[i][j];
        if (p + 1 < cap) out[p++] = '\n';
    }
    if (p < cap) out[p] = 0;
    return p;
}

static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }

int dock_icon(const char* n) {
    if (same(n, "plasma")) return ICON_WAVE;
    if (same(n, "paint")) return ICON_BRUSH;
    if (same(n, "cube3d")) return ICON_CUBE;
    if (starts(n, "atk")) return ICON_AI;
    return ICON_TERMINAL;                          /* остальные программы пишут в терминал */
}

uint32_t dock_color(const char* n) {
    if (same(n, "plasma")) return TH_PINK;
    if (same(n, "paint")) return TH_BLUE;
    if (same(n, "cube3d")) return TH_CYAN;
    static const uint32_t pal[6] = { TH_GREEN, TH_PURPLE, TH_AMBER, TH_CYAN, TH_PINK, TH_BLUE };
    uint32_t h = 2166136261u;
    for (; *n; n++) h = (h ^ (uint8_t)*n) * 16777619u;
    return pal[h % 6];
}
