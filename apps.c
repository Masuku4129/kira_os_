#include "apps.h"
#include "gfxui.h"
#include "icons.h"
#include "theme.h"
#include "mind.h"

#define CARD      0x000B1528u
#define CARD_EDGE 0x00182844u
#define TRACK     0x00132139u

/* ---------------- форматирование ---------------- */

int app_fmt_u(char* out, uint64_t v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    int i = 0; while (n) out[i++] = t[--n];
    out[i] = 0;
    return i;
}

static int cat(char* d, int p, const char* s) { while (*s) d[p++] = *s++; d[p] = 0; return p; }
static int cat_u(char* d, int p, uint64_t v) { return p + app_fmt_u(d + p, v); }
static int cat_2(char* d, int p, unsigned v) { d[p++] = (char)('0' + v / 10 % 10); d[p++] = (char)('0' + v % 10); d[p] = 0; return p; }

/* x100 -> "12.3" (одна цифра после точки до 100, дальше целые; от 10000 - «к») */
static int cat_x100(char* d, int p, int64_t v) {
    if (v < 0) { d[p++] = '-'; v = -v; }
    if (v >= 1000000) { uint64_t k = (uint64_t)v / 100000; p = cat_u(d, p, k / 10); d[p++] = '.'; d[p++] = (char)('0' + k % 10); return cat(d, p, "к"); }
    if (v >= 10000) return cat_u(d, p, (uint64_t)(v + 50) / 100);
    p = cat_u(d, p, (uint64_t)v / 100);
    d[p++] = '.'; d[p++] = (char)('0' + (v / 10) % 10); d[p] = 0;
    return p;
}

void app_fmt_hms(char* out, uint32_t s) {
    int p = cat_2(out, 0, s / 3600 % 100); out[p++] = ':'; p = cat_2(out, p, s / 60 % 60); out[p++] = ':'; cat_2(out, p, s % 60);
}

void app_fmt_kb(char* out, uint64_t kb) {
    int p = 0;
    if (kb >= 1024 * 1024) { uint64_t t = kb * 10 / (1024 * 1024); p = cat_u(out, 0, t / 10); out[p++] = '.'; out[p++] = (char)('0' + t % 10); cat(out, p, " ГБ"); }
    else if (kb >= 1024) { uint64_t t = kb * 10 / 1024; p = cat_u(out, 0, t / 10); if (t < 1000) { out[p++] = '.'; out[p++] = (char)('0' + t % 10); } cat(out, p, " МБ"); }
    else { p = cat_u(out, 0, kb); cat(out, p, " КБ"); }
}

static void fmt_age(char* out, uint32_t s) {
    int p = 0;
    if (s >= 86400) { p = cat_u(out, p, s / 86400); p = cat(out, p, " д "); p = cat_u(out, p, s / 3600 % 24); cat(out, p, " ч"); return; }
    if (s >= 3600) { p = cat_u(out, p, s / 3600); p = cat(out, p, " ч "); }
    p = cat_u(out, p, s / 60 % 60); cat(out, p, " мин");
}

/* ---------------- текст ---------------- */

static int ncp(const char* s) { int n = 0; while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; } return n; }

/* Рисует не больше maxw пикселей текста; если не влезло - «…» в конце. */
static void text_fit(surface_t* s, int x, int y, const char* t, int maxw, uint32_t col) {
    int maxc = maxw / 8;
    if (maxc <= 0) return;
    if (ncp(t) <= maxc) { gfx_text(s, x, y, t, col, -1); return; }
    char buf[200]; int b = 0, n = 0;
    const char* p = t;
    while (*p && n < maxc - 1 && b < 190) {
        const char* q = p; utf8_next(&q);
        while (p < q) buf[b++] = *p++;
        n++;
    }
    b = cat(buf, b, "…");
    gfx_text(s, x, y, buf, col, -1);
}

/* Перенос по словам в maxlines строк; возвращает число нарисованных строк. */
static int text_wrap(surface_t* s, int x, int y, const char* t, int maxw, int maxlines, int lh, uint32_t col) {
    int maxc = maxw / 8, line = 0;
    const char* p = t;
    while (*p && line < maxlines) {
        const char* q = p; const char* sp = 0; int n = 0;
        while (*q && n < maxc) { if (*q == ' ') sp = q; utf8_next(&q); n++; }
        const char* end = q;
        if (*q && *q != ' ' && sp && sp > p) end = sp;
        char buf[200]; int b = 0;
        for (const char* c = p; c < end && b < 196; c++) buf[b++] = *c;
        buf[b] = 0;
        if (line == maxlines - 1 && *end) { text_fit(s, x, y + line * lh, p, maxw, col); return line + 1; }
        gfx_text(s, x, y + line * lh, buf, col, -1);
        line++;
        p = end; while (*p == ' ') p++;
    }
    return line;
}

static void card(surface_t* s, int x, int y, int w, int h, const char* title, uint32_t accent) {
    ui_round_rect(s, x, y, w, h, 10, CARD, 255);
    ui_round_frame(s, x, y, w, h, 10, CARD_EDGE, 255);
    if (title) {
        gfx_rect(s, x + 12, y + 11, 3, 12, accent);
        gfx_text(s, x + 21, y + 9, title, TH_TEXT_FAINT, -1);
    }
}

/* Кривая из 60 значений 0..100 (самое старое - hist[hpos % 60]) с заливкой под ней. */
static void spark(surface_t* s, int x, int y, int w, int h, const uint8_t* hist, uint32_t hpos, uint32_t col, int fill) {
    int px = -1, py = 0;
    for (int i = 0; i < MIND_HIST; i++) {
        unsigned v = hist[(hpos + (unsigned)i) % MIND_HIST];
        if (v > 100) v = 100;
        int xx = x + i * (w - 1) / (MIND_HIST - 1);
        int yy = y + h - 1 - (int)(v * (unsigned)(h - 1) / 100);
        if (fill) for (int k = px < 0 ? xx : px + 1; k <= xx; k++) for (int j = yy; j < y + h; j++) ui_blend(s, k, j, col, 34);
        if (px >= 0) ui_seg(s, px * 16 + 8, py * 16 + 8, xx * 16 + 8, yy * 16 + 8, 12, col, 255);
        px = xx; py = yy;
    }
}

static uint32_t health_color(unsigned h) { return h >= 70 ? TH_GREEN : h >= 50 ? TH_AMBER : TH_RED; }

/* ---------------- Разум ---------------- */

static const char* const short_unit[MIND_SIG_N] = { "%", "", "%", "/с", " мкс", "/с", "/с", "/с" };

static uint32_t kind_color(int k) {
    switch (k) {
    case MJ_LEARN: case MJ_FOCUS: return TH_CYAN;
    case MJ_ANOMALY: case MJ_LIMIT: case MJ_MEMORY: case MJ_STARVE: return TH_AMBER;
    case MJ_RESTORE: return TH_GREEN;
    case MJ_TUNE_TRY: case MJ_TUNE_KEEP: case MJ_TUNE_BACK: return TH_PURPLE;
    case MJ_FX: return TH_BLUE;
    case MJ_KILL: case MJ_BEHAVIOR: return TH_RED;
    default: return TH_TEXT_DIM;
    }
}

static void orb(surface_t* s, int cx, int cy, const mind_view_t* v) {
    static const uint32_t col[4] = { TH_CYAN, TH_GREEN, TH_PURPLE, TH_AMBER };
    const unsigned val[4] = { v->c_resp, v->c_room, v->c_bal, v->c_stab };   /* каждая часть здоровья - 0..100 */
    ui_disc(s, cx, cy, 86, TH_PURPLE, 14);
    ui_disc(s, cx, cy, 52, 0x00060B16, 255);
    for (int i = 0; i < 4; i++) {
        int ro = 84 - i * 9, ri = ro - 6;
        ui_ring(s, cx, cy, ri, ro, TRACK, 255);
        unsigned sweep = val[i] >= 100 ? 3600 : val[i] * 36;
        ui_arc(s, cx, cy, ri, ro, 0, (int)sweep, col[i]);
    }
    char t[8]; app_fmt_u(t, v->health);
    int tw = ui_text_smooth_width(t, 40);
    ui_text_smooth(s, cx - tw / 2, cy - 26, t, 40, health_color(v->health));
    gfx_text(s, cx - gfx_text_width("здоровье") / 2, cy + 16, "здоровье", TH_TEXT_FAINT, -1);
}

void app_mind_draw(surface_t* s, const mind_view_t* v) {
    gfx_fill(s, TH_CONTENT);
    char t[96]; int p;

    /* здоровье и настроение */
    card(s, 10, 10, 200, 320, 0, 0);
    orb(s, 110, 104, v);
    const char* mood = v->mood[0] ? v->mood : "просыпается";
    gfx_text(s, 110 - gfx_text_width(mood) / 2, 196, mood, TH_TEXT, -1);
    p = cat(t, 0, v->mode ? "действует" : "наблюдает");
    p = cat(t, p, " · квант "); p = cat_u(t, p, v->quantum_ms); cat(t, p, " мс");
    gfx_text(s, 110 - gfx_text_width(t) / 2, 216, t, v->mode ? TH_GREEN : TH_TEXT_DIM, -1);
    p = cat(t, 0, "анимация "); p = cat_u(t, p, v->fx); cat(t, p, v->fx_user == MIND_FX_AUTO ? " · авто" : " · вручную");
    gfx_text(s, 110 - gfx_text_width(t) / 2, 234, t, TH_TEXT_FAINT, -1);
    /* из чего сложено здоровье: кольца снаружи внутрь */
    gfx_rect(s, 22, 256, 176, 1, CARD_EDGE);
    static const char* const part[4] = { "отклик", "запас", "равновесие", "стабильность" };
    static const uint32_t pcol[4] = { TH_CYAN, TH_GREEN, TH_PURPLE, TH_AMBER };
    const unsigned pval[4] = { v->c_resp, v->c_room, v->c_bal, v->c_stab };
    for (int i = 0; i < 4; i++) {
        int y = 264 + i * 16;
        ui_disc(s, 27, y + 8, 4, pcol[i], 255);
        gfx_text(s, 38, y, part[i], TH_TEXT_DIM, -1);
        app_fmt_u(t, pval[i]);
        gfx_text(s, 198 - gfx_text_width(t), y, t, pval[i] >= 70 ? TH_TEXT : pval[i] >= 40 ? TH_AMBER : TH_RED, -1);
    }

    /* сигналы: значение, отклонение от нормы, минута истории */
    card(s, 220, 10, 370, 320, "СИГНАЛЫ И НОРМЫ", TH_CYAN);
    for (int k = 0; k < MIND_SIG_N; k++) {
        int y = 38 + k * 36;
        int az = v->z_x10[k] < 0 ? -v->z_x10[k] : v->z_x10[k];
        int learning = v->learned[k] < 60;
        uint32_t vc = learning ? TH_TEXT_DIM : az >= 40 ? TH_RED : az >= 25 ? TH_AMBER : TH_TEXT;
        gfx_text(s, 232, y + 2, mind_signal_name(k), TH_TEXT_DIM, -1);
        p = cat_x100(t, 0, v->cur_x100[k]); cat(t, p, short_unit[k]);
        gfx_text(s, 446 - gfx_text_width(t), y + 2, t, vc, -1);
        uint32_t sc = learning ? TH_TEXT_FAINT : az >= 25 ? vc : ui_mix(TH_CYAN, TH_PURPLE, (unsigned)k * 32);
        gfx_rect(s, 456, y + 20, 124, 1, TRACK);
        spark(s, 456, y + 1, 124, 20, v->hist[k], v->hpos, sc, 1);
    }

    /* счётчики */
    card(s, 10, 340, 580, 40, 0, 0);
    char age[32]; fmt_age(age, v->lifetime_s);
    p = cat(t, 0, "возраст "); p = cat(t, p, age);
    gfx_text(s, 22, 352, t, TH_TEXT_DIM, -1);
    int x = 22 + gfx_text_width(t) + 22;
    p = cat(t, 0, "решений "); p = cat_u(t, p, v->decisions);
    gfx_text(s, x, 352, t, TH_CYAN, -1); x += gfx_text_width(t) + 22;
    p = cat(t, 0, "необычностей "); p = cat_u(t, p, v->anomalies);
    gfx_text(s, x, 352, t, TH_AMBER, -1); x += gfx_text_width(t) + 22;
    p = cat(t, 0, "квант: "); p = cat_u(t, p, v->tune_wins); p = cat(t, p, " из "); p = cat_u(t, p, v->tune_trials); p = cat(t, p, " проб");
    gfx_text(s, x, 352, t, TH_PURPLE, -1); x += gfx_text_width(t) + 22;
    if (v->kills) { p = cat(t, 0, "остановок "); p = cat_u(t, p, v->kills); text_fit(s, x, 352, t, 588 - x, TH_RED); }

    /* журнал */
    card(s, 10, 390, 580, 140, "ЖУРНАЛ РЕШЕНИЙ", TH_PURPLE);
    int y = 414;
    if (!v->nj) gfx_text(s, 24, y, "пока тихо: Разум изучает нормы этой машины", TH_TEXT_FAINT, -1);
    for (unsigned i = 0; i < v->nj && y < 514; i++) {
        uint32_t c = kind_color(v->j[i].kind);
        if (v->j[i].applied) ui_disc(s, 26, y + 8, 4, c, 255);
        else { ui_ring(s, 26, y + 8, 2, 4, c, 255); }
        app_fmt_hms(t, v->j[i].t);
        gfx_text(s, 38, y, t, TH_TEXT_FAINT, -1);
        int lines = text_wrap(s, 112, y, v->j[i].text, 470, (514 - y) >= 36 ? 2 : 1, 17, i == 0 ? TH_TEXT : TH_TEXT_DIM);
        y += lines * 17 + 4;
    }
}

/* ---------------- Монитор ---------------- */

static uint32_t load_color(unsigned p) { return p < 60 ? TH_GREEN : p < 85 ? TH_AMBER : TH_RED; }

void app_monitor_draw(surface_t* s, const mind_view_t* v, uint32_t threads) {
    gfx_fill(s, TH_CONTENT);
    char t[96]; int p;
    unsigned n = v->cores ? v->cores : 1;
    if (n > 64) n = 64;

    p = cat(t, 0, "ЯДРА: "); p = cat_u(t, p, n); p = cat(t, p, " · ЗАГРУЗКА "); p = cat_u(t, p, v->load);
    p = cat(t, p, "% · ГОТОВЫ К РАБОТЕ "); p = cat_u(t, p, v->runnable); p = cat(t, p, " · ПОТОКОВ "); cat_u(t, p, threads);
    card(s, 10, 10, 580, 150, t, TH_CYAN);
    int ax = 24, aw = 552, ay = 38, ah = 92;
    int slot = aw / (int)n, bw = slot - 4; if (bw > 56) bw = 56; if (bw < 2) bw = 2;
    int x0 = ax + (aw - slot * (int)n) / 2 + (slot - bw) / 2;
    for (unsigned i = 0; i < n; i++) {
        int x = x0 + (int)i * slot;
        unsigned b = v->busy[i] > 100 ? 100 : v->busy[i];
        ui_round_rect(s, x, ay, bw, ah, bw >= 8 ? 4 : 0, TRACK, 255);
        int h = (int)(b * (unsigned)ah / 100); if (h < 2) h = 2;
        ui_vgrad(s, x, ay + ah - h, bw, h, ui_mix(TH_CYAN, TH_PURPLE, b * 255 / 100), ui_mix(TH_CYAN, TH_PURPLE, 40));
        if (b >= 85) gfx_rect(s, x, ay + ah - h, bw, 2, TH_RED);
        if (slot >= 22) { app_fmt_u(t, i); gfx_text(s, x + bw / 2 - gfx_text_width(t) / 2, ay + ah + 4, t, TH_TEXT_FAINT, -1); }
    }

    card(s, 10, 170, 580, 110, "МИНУТА: ЗДОРОВЬЕ, ЗАГРУЗКА, ПАМЯТЬ", TH_GREEN);
    for (int g = 0; g <= 4; g++) gfx_rect(s, 24, 198 + g * 18, 552, 1, TRACK);
    spark(s, 24, 196, 552, 74, v->hist[MS_MEM], v->hpos, TH_PURPLE, 0);
    spark(s, 24, 196, 552, 74, v->hist[MS_LOAD], v->hpos, TH_CYAN, 1);
    spark(s, 24, 196, 552, 74, v->health_hist, v->hpos, TH_GREEN, 0);

    p = cat(t, 0, "ПРОГРАММЫ: "); cat_u(t, p, v->nproc);
    card(s, 10, 290, 580, 170, t, TH_PURPLE);
    static const char* const head[] = { "PID", "ПРОГРАММА", "ПОТОКИ", "ЦП", "ВЫЗ/С", "ПАМЯТЬ", "ВЕС", "ЛИМИТ" };
    static const int col[] = { 22, 62, 196, 262, 318, 392, 474, 520 };
    for (int i = 0; i < 8; i++) gfx_text(s, col[i], 316, head[i], TH_TEXT_FAINT, -1);
    gfx_rect(s, 22, 334, 556, 1, CARD_EDGE);
    /* по убыванию ЦП: показываем самые занятые */
    int idx[16], m = (int)(v->nproc > 16 ? 16 : v->nproc);
    for (int i = 0; i < m; i++) idx[i] = i;
    for (int i = 1; i < m; i++) { int k = idx[i], j = i - 1; while (j >= 0 && v->p[idx[j]].cpu_x10 < v->p[k].cpu_x10) { idx[j + 1] = idx[j]; j--; } idx[j + 1] = k; }
    if (!m) gfx_text(s, 22, 344, "программ Ring 3 нет: запустите из боковой панели или командой run", TH_TEXT_FAINT, -1);
    for (int r = 0; r < m && r < 6; r++) {
        int y = 340 + r * 19;
        const __typeof__(v->p[0])* q = &v->p[idx[r]];
        if (q->focus) ui_round_rect(s, 16, y - 1, 568, 18, 4, TH_CYAN, 22);
        uint32_t nc = q->flagged ? TH_RED : q->focus ? TH_CYAN : TH_TEXT;
        app_fmt_u(t, q->pid); gfx_text(s, col[0], y, t, TH_TEXT_DIM, -1);
        text_fit(s, col[1], y, q->name, col[2] - col[1] - 8, nc);
        app_fmt_u(t, q->threads); gfx_text(s, col[2], y, t, TH_TEXT_DIM, -1);
        p = cat_u(t, 0, q->cpu_x10 / 10); t[p++] = '.'; t[p++] = (char)('0' + q->cpu_x10 % 10); cat(t, p, "%");
        gfx_text(s, col[3], y, t, load_color(q->cpu_x10 / 10), -1);
        app_fmt_u(t, q->sysc); gfx_text(s, col[4], y, t, TH_TEXT_DIM, -1);
        app_fmt_kb(t, q->heap_kb); gfx_text(s, col[5], y, t, TH_TEXT_DIM, -1);
        app_fmt_u(t, q->weight); gfx_text(s, col[6], y, t, q->weight > 4 ? TH_GREEN : TH_TEXT_DIM, -1);
        if (q->limit && q->limit < 100) { p = cat_u(t, 0, q->limit); cat(t, p, "%"); gfx_text(s, col[7], y, t, TH_AMBER, -1); }
        else gfx_text(s, col[7], y, "—", TH_TEXT_FAINT, -1);
    }
}

/* ---------------- Проводник ---------------- */

#define TREE_X 10
#define TREE_Y 44
#define TREE_W 270
#define TREE_H 416
#define ROW_H  19
#define PREV_X 290

int app_files_visible_rows(void) { return (TREE_H - 16 - 26) / ROW_H; }

/* Программа из /bin: имя файла (иначе 0). */
const char* app_files_program(const exp_view_t* e) {
    const char* p = e->sel_path;
    if (p[0] != '/' || p[1] != 'b' || p[2] != 'i' || p[3] != 'n' || p[4] != '/' || !p[5] || e->prev_len < 0) return 0;
    for (const char* q = p + 5; *q; q++) if (*q == '/') return 0;
    return p + 5;
}

#define RUN_X (PREV_X + 300 - 12 - 104)
#define RUN_Y (TREE_Y + 10)
#define RUN_W 104
#define RUN_H 28
#define PIN_X (RUN_X - 8 - RUN_W)

void app_files_draw(surface_t* s, const exp_view_t* e, int hover, int scroll, const char* status, int pinned) {
    gfx_fill(s, TH_CONTENT);
    char t[200]; int p;
    /* шапка: том и кнопка «обновить» */
    icon_draw(s, 24, 20, 20, ICON_FOLDER, TH_AMBER, 0);
    if (e->mounted) {
        char a[24], b[24]; app_fmt_kb(a, e->total_kb - e->free_kb); app_fmt_kb(b, e->total_kb);
        p = cat(t, 0, "KiraFS · занято "); p = cat(t, p, a); p = cat(t, p, " из "); p = cat(t, p, b);
        p = cat(t, p, " · объектов "); p = cat_u(t, p, e->files);
        if (e->nvol) { p = cat(t, p, " · флешек "); cat_u(t, p, e->nvol); }
    } else cat(t, 0, "Диск с KiraFS не подключён: проводнику нечего показать");
    gfx_text(s, 42, 12, t, e->mounted ? TH_TEXT_DIM : TH_AMBER, -1);
    ui_round_rect(s, 490, 6, 100, 28, 8, hover == APP_FILES_REFRESH ? ui_mix(CARD, TH_CYAN, 50) : CARD, 255);
    ui_round_frame(s, 490, 6, 100, 28, 8, hover == APP_FILES_REFRESH ? TH_CYAN : CARD_EDGE, 255);
    gfx_text(s, 540 - gfx_text_width("обновить") / 2, 12, "обновить", TH_CYAN, -1);

    /* дерево */
    card(s, TREE_X, TREE_Y, TREE_W, TREE_H, 0, 0);
    int vis = app_files_visible_rows();
    for (int i = 0; i < vis && scroll + i < (int)e->nrows; i++) {
        const exp_row_t* r = &e->row[scroll + i];
        int y = TREE_Y + 8 + i * ROW_H;
        if (r->sel) ui_round_rect(s, TREE_X + 4, y, TREE_W - 8, ROW_H - 1, 5, TH_CYAN, 40);
        else if (hover == scroll + i) ui_round_rect(s, TREE_X + 4, y, TREE_W - 8, ROW_H - 1, 5, TH_CYAN, 18);
        int x = TREE_X + 10 + r->depth * 14;
        if (r->dir) {
            int cx = x + 4, cy = y + 9;
            if (r->open) { gfx_line(s, cx - 3, cy - 2, cx, cy + 1, TH_TEXT_DIM); gfx_line(s, cx + 3, cy - 2, cx, cy + 1, TH_TEXT_DIM); }
            else { gfx_line(s, cx - 1, cy - 4, cx + 2, cy - 1, TH_TEXT_DIM); gfx_line(s, cx - 1, cy + 2, cx + 2, cy - 1, TH_TEXT_DIM); }
        }
        int vol = -1;                                                  /* том (флешка) в корне */
        if (r->depth == 0 && r->dir) for (uint32_t k = 0; k < e->nvol && k < EXP_VOLS; k++) {
            const char* a = r->name; const char* b = e->vol[k].name; while (*a && *a == *b) { a++; b++; }
            if (!*a && !*b) vol = (int)k;
        }
        icon_draw(s, x + 18, y + 9, 14, r->dir ? ICON_FOLDER : ICON_FILE, vol >= 0 ? TH_GREEN : r->dir ? TH_AMBER : TH_CYAN, 0);
        int nx = x + 30, sizew = 0;
        if (!r->dir) { app_fmt_kb(t, (r->size + 1023) / 1024); sizew = gfx_text_width(t); gfx_text(s, TREE_X + TREE_W - 12 - sizew, y + 1, t, TH_TEXT_FAINT, -1); }
        else if (vol >= 0) {                                           /* метка и размер флешки */
            app_fmt_kb(t, e->vol[vol].total_kb);
            int tw = gfx_text_width(t); gfx_text(s, TREE_X + TREE_W - 12 - tw, y + 1, t, TH_TEXT_FAINT, -1);
            int lx = x + 30 + gfx_text_width(r->name) + 8, lw = TREE_X + TREE_W - 20 - tw - lx;
            if (lw > 24) text_fit(s, lx, y + 1, e->vol[vol].label, lw, TH_GREEN);
            sizew = TREE_X + TREE_W - 18 - nx - gfx_text_width(r->name); /* имени тома - ровно его ширина, дальше метка */
        }
        text_fit(s, nx, y + 1, r->name, TREE_X + TREE_W - 18 - sizew - nx, r->sel ? TH_TEXT : r->dir ? TH_TEXT : TH_TEXT_DIM);
    }
    if (e->mounted && !e->nrows) gfx_text(s, TREE_X + 14, TREE_Y + 12, "пусто", TH_TEXT_FAINT, -1);
    int total = (int)e->nrows;
    if (total > vis || scroll > 0) {                                  /* прокрутка */
        int by = TREE_Y + TREE_H - 30;
        gfx_rect(s, TREE_X + 8, by - 2, TREE_W - 16, 1, CARD_EDGE);
        static const char* const arrow[2] = { "↑", "↓" };
        for (int k = 0; k < 2; k++) {
            int bx = TREE_X + 10 + k * 40, hv = hover == (k ? APP_FILES_DOWN : APP_FILES_UP);
            ui_round_rect(s, bx, by + 2, 32, 22, 6, hv ? ui_mix(CARD, TH_CYAN, 60) : TRACK, 255);
            gfx_text(s, bx + 12, by + 5, arrow[k], TH_CYAN, -1);
        }
        p = cat_u(t, 0, (uint64_t)scroll + 1); p = cat(t, p, "–"); p = cat_u(t, p, (uint64_t)(scroll + vis < total ? scroll + vis : total));
        p = cat(t, p, " из "); p = cat_u(t, p, (uint64_t)total); if (e->truncated) cat(t, p, "+");
        gfx_text(s, TREE_X + TREE_W - 12 - gfx_text_width(t), by + 5, t, TH_TEXT_FAINT, -1);
    }

    /* просмотр */
    card(s, PREV_X, TREE_Y, 300, TREE_H, 0, 0);
    if (!e->sel_path[0]) {
        gfx_text(s, PREV_X + 16, TREE_Y + 14, "выберите файл слева", TH_TEXT_FAINT, -1);
        gfx_text(s, PREV_X + 16, TREE_Y + 34, "каталог: щелчок раскрывает", TH_TEXT_FAINT, -1);
        gfx_text(s, PREV_X + 16, TREE_Y + 54, "программа в /bin: двойной щелчок", TH_TEXT_FAINT, -1);
        gfx_text(s, PREV_X + 16, TREE_Y + 70, "или кнопка «запустить»", TH_TEXT_FAINT, -1);
        gfx_text(s, PREV_X + 16, TREE_Y + 94, "«закрепить» - значок в левой", TH_TEXT_FAINT, -1);
        gfx_text(s, PREV_X + 16, TREE_Y + 110, "панели для быстрого запуска", TH_TEXT_FAINT, -1);
        if (status && status[0]) text_wrap(s, PREV_X + 14, TREE_Y + TREE_H - 40, status, 272, 2, 17, TH_GREEN);
        return;
    }
    const char* prog = app_files_program(e);
    int hy = 0;                                                        /* у программы сверху ряд кнопок */
    if (prog) {
        int hv = hover == APP_FILES_RUN;
        ui_round_rect(s, RUN_X, RUN_Y, RUN_W, RUN_H, 8, hv ? ui_mix(CARD, TH_GREEN, 80) : ui_mix(CARD, TH_GREEN, 30), 255);
        ui_round_frame(s, RUN_X, RUN_Y, RUN_W, RUN_H, 8, TH_GREEN, hv ? 255 : 160);
        gfx_text(s, RUN_X + RUN_W / 2 - gfx_text_width("запустить") / 2, RUN_Y + 6, "запустить", hv ? TH_TEXT : TH_GREEN, -1);
        const char* pl = pinned ? "открепить" : "закрепить";
        uint32_t pc = pinned ? TH_AMBER : TH_CYAN;
        hv = hover == APP_FILES_PIN;
        ui_round_rect(s, PIN_X, RUN_Y, RUN_W, RUN_H, 8, hv ? ui_mix(CARD, pc, 80) : ui_mix(CARD, pc, 30), 255);
        ui_round_frame(s, PIN_X, RUN_Y, RUN_W, RUN_H, 8, pc, hv ? 255 : 160);
        gfx_text(s, PIN_X + RUN_W / 2 - gfx_text_width(pl) / 2, RUN_Y + 6, pl, hv ? TH_TEXT : pc, -1);
        hy = 36;
    }
    text_fit(s, PREV_X + 14, TREE_Y + 10 + hy, e->sel_path, 272, TH_CYAN);
    if (e->prev_len < 0) { gfx_text(s, PREV_X + 14, TREE_Y + 32 + hy, "не удалось прочитать", TH_RED, -1); return; }
    char sz[24]; app_fmt_u(sz, e->sel_size);
    p = cat(t, 0, sz); p = cat(t, p, " байт"); if (e->prev_bin) cat(t, p, " · двоичный");
    gfx_text(s, PREV_X + 14, TREE_Y + 28 + hy, t, TH_TEXT_FAINT, -1);
    gfx_rect(s, PREV_X + 12, TREE_Y + 48 + hy, 276, 1, CARD_EDGE);
    int y = TREE_Y + 56 + hy, ymax = TREE_Y + TREE_H - 20;
    if (status && status[0]) {                                         /* итог последнего запуска */
        ymax -= 40;
        gfx_rect(s, PREV_X + 12, TREE_Y + TREE_H - 46, 276, 1, CARD_EDGE);
        text_wrap(s, PREV_X + 14, TREE_Y + TREE_H - 40, status, 272, 2, 17, TH_GREEN);
    }
    if (e->prev_bin) {                                                 /* шестнадцатеричный вид: 8 байт в строке */
        static const char hx[] = "0123456789ABCDEF";
        for (int off = 0; off < e->prev_len && y <= ymax; off += 8, y += 17) {
            p = 0;
            t[p++] = hx[(off >> 12) & 15]; t[p++] = hx[(off >> 8) & 15]; t[p++] = hx[(off >> 4) & 15]; t[p++] = hx[off & 15]; t[p++] = ' ';
            for (int k = 0; k < 8 && off + k < e->prev_len; k++) { uint8_t c = (uint8_t)e->prev[off + k]; t[p++] = ' '; t[p++] = hx[c >> 4]; t[p++] = hx[c & 15]; }
            t[p] = 0;
            gfx_text(s, PREV_X + 14, y, t, TH_TEXT_DIM, -1);
        }
        return;
    }
    const char* q = e->prev; const char* end = e->prev + e->prev_len;
    while (q < end && y <= ymax) {                                     /* текст: строки файла с переносом по словам */
        int b = 0;
        while (q < end && *q != '\n' && b < 190) {
            char c = *q++;
            if (c == '\r') continue;
            if (c == '\t') { t[b++] = ' '; t[b++] = ' '; continue; }
            t[b++] = c;
        }
        t[b] = 0;
        while (q < end && *q != '\n') q++;                             /* слишком длинная строка: остаток не показываем */
        if (q < end) q++;
        int lines = b ? text_wrap(s, PREV_X + 14, y, t, 272, (ymax - y) / 17 + 1, 17, TH_TEXT) : 1;
        y += lines * 17;
    }
    if (e->prev_len >= 0 && e->sel_size > (uint64_t)e->prev_len && y <= ymax + 17) gfx_text(s, PREV_X + 14, ymax + 2, "…", TH_TEXT_FAINT, -1);
}

int app_files_hit(const exp_view_t* e, int scroll, int x, int y) {
    if (x >= 490 && x < 590 && y >= 6 && y < 34) return APP_FILES_REFRESH;
    if (app_files_program(e) && x >= RUN_X && x < RUN_X + RUN_W && y >= RUN_Y && y < RUN_Y + RUN_H) return APP_FILES_RUN;
    if (app_files_program(e) && x >= PIN_X && x < PIN_X + RUN_W && y >= RUN_Y && y < RUN_Y + RUN_H) return APP_FILES_PIN;
    if (x < TREE_X || x >= TREE_X + TREE_W || y < TREE_Y || y >= TREE_Y + TREE_H) return APP_FILES_NONE;
    int vis = app_files_visible_rows(), total = (int)e->nrows;
    if (total > vis || scroll > 0) {
        int by = TREE_Y + TREE_H - 30;
        if (y >= by + 2 && y < by + 24) {
            if (x >= TREE_X + 10 && x < TREE_X + 42) return APP_FILES_UP;
            if (x >= TREE_X + 50 && x < TREE_X + 82) return APP_FILES_DOWN;
            return APP_FILES_NONE;
        }
    }
    int i = (y - TREE_Y - 8) / ROW_H;
    if (y < TREE_Y + 8 || i >= vis || scroll + i >= total) return APP_FILES_NONE;
    return scroll + i;
}

/* ---------------- Настройки ---------------- */

typedef struct { int row; const char* label; int code; } seg_t;
static const seg_t segs[] = {
    { 0, "наблюдает", 10 }, { 0, "действует", 11 },
    { 1, "авто", 24 }, { 1, "0", 20 }, { 1, "1", 21 }, { 1, "2", 22 }, { 1, "3", 23 },
    { 2, "UTC", 32 }, { 2, "местн", 33 }, { 2, "-", 30 }, { 2, 0, -1 }, { 2, "+", 31 },
    { 3, "выкл", 40 }, { 3, "вкл", 41 },
};
#define NSEG ((int)(sizeof segs / sizeof segs[0]))
#define SET_ROW_Y(r) (12 + (r) * 64)

static int seg_w(int i) {
    if (!segs[i].label) return 84;
    int w = gfx_text_width(segs[i].label) + 24;
    return w < 34 ? 34 : w;
}

static void seg_rect(int i, int* x, int* y, int* w, int* h) {
    int r = segs[i].row, right = 500;
    for (int k = NSEG - 1; k > i; k--) if (segs[k].row == r) right -= seg_w(k) + 6;
    *w = seg_w(i);
    *x = right - *w; *y = SET_ROW_Y(r) + 15; *h = 30;
}

int app_settings_hit(int x, int y) {
    for (int i = 0; i < NSEG; i++) {
        if (segs[i].code < 0) continue;
        int sx, sy, sw, sh; seg_rect(i, &sx, &sy, &sw, &sh);
        if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) return segs[i].code;
    }
    return -1;
}

static int seg_on(const app_settings_t* st, int code) {
    if (code == 10 || code == 11) return st->mode == code - 10;
    if (code >= 20 && code <= 24) return st->fx_user == code - 20;
    if (code == 40 || code == 41) return st->telemetry == code - 40;
    if (code == 32 || code == 33) return st->rtc_local == code - 32;
    return 0;
}

void app_settings_draw(surface_t* s, const app_settings_t* st, int hover) {
    gfx_fill(s, TH_CONTENT);
    static const char* const title[4] = { "Разум", "Анимация фона", "Часовой пояс", "Телеметрия" };
    static const char* const hint[4] = { "наблюдать или управлять системой", "авто - решает Разум",
                                         "часы ПК: UTC / местное", "замеры Разума в журнал раз в секунду" };
    static const uint32_t acc[4] = { TH_PURPLE, TH_CYAN, TH_GREEN, TH_AMBER };
    for (int r = 0; r < 4; r++) {
        int y = SET_ROW_Y(r);
        card(s, 10, y, 500, 58, 0, 0);
        gfx_rect(s, 22, y + 14, 3, 30, acc[r]);
        ui_text_bold(s, 34, y + 12, title[r], TH_TEXT);
        gfx_text(s, 34, y + 31, hint[r], TH_TEXT_FAINT, -1);
    }
    for (int i = 0; i < NSEG; i++) {
        int x, y, w, h; seg_rect(i, &x, &y, &w, &h);
        if (segs[i].code < 0) {                                         /* поле пояса */
            char t[16]; int p = cat(t, 0, "UTC"); int tz = st->tz_min;
            t[p++] = tz < 0 ? '-' : '+'; if (tz < 0) tz = -tz;
            p = cat_u(t, p, (uint64_t)tz / 60); if (tz % 60) { t[p++] = ':'; p = cat_2(t, p, (unsigned)tz % 60); }
            t[p] = 0;
            gfx_text(s, x + w / 2 - gfx_text_width(t) / 2, y + 7, t, TH_TEXT, -1);
            continue;
        }
        int on = seg_on(st, segs[i].code), hv = hover == segs[i].code;
        uint32_t a = acc[segs[i].row];
        ui_round_rect(s, x, y, w, h, 8, on ? ui_mix(CARD, a, 70) : hv ? ui_mix(CARD, a, 30) : TRACK, 255);
        ui_round_frame(s, x, y, w, h, 8, on ? a : hv ? ui_mix(CARD_EDGE, a, 120) : CARD_EDGE, 255);
        gfx_text(s, x + w / 2 - gfx_text_width(segs[i].label) / 2, y + 7, segs[i].label, on ? TH_TEXT : TH_TEXT_DIM, -1);
    }
    /* о системе */
    int y = SET_ROW_Y(4);
    card(s, 10, y, 500, 132, "СИСТЕМА", TH_BLUE);
    char t[96], a[24]; int p;
    p = cat(t, 0, "ядер "); p = cat_u(t, p, (uint64_t)st->cores); p = cat(t, p, " · память "); app_fmt_kb(a, (uint64_t)st->ram_mb * 1024); cat(t, p, a);
    gfx_text(s, 24, y + 34, t, TH_TEXT_DIM, -1);
    p = cat(t, 0, "экран "); p = cat_u(t, p, (uint64_t)st->scr_w); p = cat(t, p, "×"); p = cat_u(t, p, (uint64_t)st->scr_h);
    p = cat(t, p, " · прерывания "); cat(t, p, st->irqc ? st->irqc : "?");
    gfx_text(s, 24, y + 54, t, TH_TEXT_DIM, -1);
    gfx_text(s, 24, y + 74, st->experience ? "опыт и модель Разума сохраняются на диск" : "диска нет: опыт живёт до перезагрузки",
             st->experience ? TH_GREEN : TH_AMBER, -1);
    p = cat(t, 0, "анимация сейчас: уровень "); p = cat_u(t, p, (uint64_t)st->fx);
    gfx_text(s, 24, y + 94, t, TH_TEXT_FAINT, -1);
}
