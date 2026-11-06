#include "apps.h"
#include "gfxui.h"
#include "icons.h"
#include "dock.h"
#include "theme.h"
#include "mind.h"

#define CARD      TH_CARD
#define CARD_EDGE TH_CARD_EDGE
#define TRACK     TH_TRACK

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

#define text_fit ui_text_fit

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

#define card ui_card

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
    /* полоса состояния: последнее решение и здоровье */
    char hr[32]; p = cat(hr, 0, "здоровье "); p = cat_u(hr, p, v->health); cat(hr, p, "%");
    if (v->nj) ui_statusbar(s, s->h - TH_BAR_H, v->j[0].text, kind_color(v->j[0].kind), hr);
    else ui_statusbar(s, s->h - TH_BAR_H, "Разум изучает нормы этой машины", TH_TEXT_DIM, hr);
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
        {                                                      /* русское название; системное имя - тусклее рядом, если помещается */
            const char* ttl = app_label(q->name);
            int cw = col[2] - col[1] - 8, tw = gfx_text_width(ttl);
            text_fit(s, col[1], y, ttl, cw, nc);
            if (ttl != q->name && tw + 8 + gfx_text_width(q->name) <= cw) gfx_text(s, col[1] + tw + 8, y, q->name, TH_TEXT_FAINT, -1);
        }
        app_fmt_u(t, q->threads); gfx_text(s, col[2], y, t, TH_TEXT_DIM, -1);
        p = cat_u(t, 0, q->cpu_x10 / 10); t[p++] = '.'; t[p++] = (char)('0' + q->cpu_x10 % 10); cat(t, p, "%");
        gfx_text(s, col[3], y, t, load_color(q->cpu_x10 / 10), -1);
        app_fmt_u(t, q->sysc); gfx_text(s, col[4], y, t, TH_TEXT_DIM, -1);
        app_fmt_kb(t, q->heap_kb); gfx_text(s, col[5], y, t, TH_TEXT_DIM, -1);
        app_fmt_u(t, q->weight); gfx_text(s, col[6], y, t, q->weight > 4 ? TH_GREEN : TH_TEXT_DIM, -1);
        if (q->limit && q->limit < 100) { p = cat_u(t, 0, q->limit); cat(t, p, "%"); gfx_text(s, col[7], y, t, TH_AMBER, -1); }
        else gfx_text(s, col[7], y, "—", TH_TEXT_FAINT, -1);
    }
    /* полоса состояния: самая занятая программа, память и здоровье */
    char sl[128], sr[48];
    if (m) {
        const __typeof__(v->p[0])* top = &v->p[idx[0]];
        p = cat(sl, 0, "самая занятая: "); p = cat(sl, p, app_label(top->name));
        if (app_info(top->name)) { p = cat(sl, p, " ("); p = cat(sl, p, top->name); p = cat(sl, p, ")"); }
        p = cat(sl, p, " · ЦП ");
        p = cat_u(sl, p, top->cpu_x10 / 10); cat(sl, p, "%");
    } else cat(sl, 0, "программ Ring 3 сейчас нет");
    p = cat(sr, 0, "память "); p = cat_u(sr, p, v->mem_used); p = cat(sr, p, "% · здоровье "); p = cat_u(sr, p, v->health); cat(sr, p, "%");
    ui_statusbar(s, s->h - TH_BAR_H, sl, TH_TEXT_DIM, sr);
}

/* ---------------- Проводник: две панели ---------------- */

#define PX(p)    (TH_GAP + (p) * (PW + TH_GAP))
#define PW       ((APP_FILES_W - 3 * TH_GAP) / 2)
#define PY       38
#define STAT_Y   (APP_FILES_H - 2 * TH_BAR_H)
#define KEYS_Y   (APP_FILES_H - TH_BAR_H)
#define PH       (STAT_Y - 6 - PY)
#define ROW_H    18
#define ROW0     46              /* первая строка списка от верха панели */
#define BTN_W    104
#define BTN_Y    5

static const ui_key_t fkeys[9] = {
    { "F1", "Помощь", TH_BLUE }, { "F2", "Имя", TH_CYAN }, { "F3", "Вид", TH_CYAN }, { "F4", "Закреп.", TH_PURPLE },
    { "F5", "Копия", TH_GREEN }, { "F6", "Перенос", TH_GREEN }, { "F7", "Каталог", TH_AMBER }, { "F8", "Удалить", TH_RED },
    { "F9", "Служеб.", TH_BLUE },
};

int app_files_hit(const exp_view_t* e, int x, int y, app_hit_t* h) {
    h->kind = APP_HIT_NONE; h->panel = 0; h->row = 0;
    if (e->fm.mode != FM_MODE_NORMAL) return h->kind;
    if (y >= KEYS_Y) { int i = ui_keybar_hit(APP_FILES_W, 9, x); if (i >= 0) { h->kind = APP_HIT_KEY; h->row = i; } return h->kind; }
    if (y >= BTN_Y && y < BTN_Y + 26) {
        int rx = APP_FILES_W - TH_GAP - BTN_W;
        if (x >= rx && x < rx + BTN_W) h->kind = APP_HIT_REFRESH;
        else if (x >= rx - 8 - BTN_W && x < rx - 8) h->kind = APP_HIT_HIDDEN;
        return h->kind;
    }
    for (int p = 0; p < 2; p++) {
        int px = PX(p);
        if (x < px || x >= px + PW || y < PY || y >= PY + PH) continue;
        const fm_panel_t* P = &e->fm.p[p];
        h->panel = p; h->kind = APP_HIT_PANEL;
        if (x >= px + PW - 12 && y >= PY + ROW0 && y < PY + ROW0 + FM_ROWS * ROW_H && (int)P->n > FM_ROWS) {   /* полоса прокрутки */
            int th = FM_ROWS * ROW_H * FM_ROWS / (int)P->n; if (th < 14) th = 14;
            int ty = PY + ROW0 + (FM_ROWS * ROW_H - th) * P->top / ((int)P->n - FM_ROWS);
            h->kind = APP_HIT_SCROLL; h->row = y < ty ? -1 : y >= ty + th ? 1 : 0;
            return h->kind;
        }
        if (y < PY + ROW0) return h->kind;
        int i = P->top + (y - PY - ROW0) / ROW_H;
        if ((y - PY - ROW0) / ROW_H < FM_ROWS && i < (int)P->n) { h->kind = APP_HIT_ROW; h->row = i; }
        return h->kind;
    }
    return h->kind;
}

static int hit_eq(const app_hit_t* a, int kind, int panel, int row) { return a && a->kind == kind && a->panel == panel && a->row == row; }

/* Длинный путь: слева «…», справа - то, что важнее (последние каталоги). */
static void path_fit(surface_t* s, int x, int y, const char* t, int maxw, uint32_t col) {
    int maxc = maxw / 8, n = ncp(t);
    if (n <= maxc) { gfx_text(s, x, y, t, col, -1); return; }
    const char* p = t;
    for (int skip = n - (maxc - 1); skip > 0 && *p; skip--) utf8_next(&p);
    char buf[160]; int b = cat(buf, 0, "…");
    while (*p && b < 150) buf[b++] = *p++;
    buf[b] = 0;
    gfx_text(s, x, y, buf, col, -1);
}

static void fmt_size(char* out, uint64_t b) {
    if (b < 1024) { int p = cat_u(out, 0, b); cat(out, p, " Б"); }
    else app_fmt_kb(out, (b + 1023) / 1024);
}

static const char* kind_name(int k, int vol) {
    if (vol) return "флешка";
    switch (k) {
    case FM_KIND_DIR: return "папка";
    case FM_KIND_UP: return "вверх";
    case FM_KIND_PROG: return "прог.";
    case FM_KIND_IMG: return "рис.";
    case FM_KIND_TEXT: return "текст";
    case FM_KIND_WASM: return "wasm";
    default: return "файл";
    }
}

static void kind_icon(int k, int vol, const char* name, int* icon, uint32_t* col) {
    *icon = ICON_FILE; *col = TH_TEXT_DIM;
    switch (k) {
    case FM_KIND_DIR: *icon = ICON_FOLDER; *col = vol ? TH_GREEN : TH_AMBER; break;
    case FM_KIND_UP: *icon = ICON_FOLDER; *col = TH_TEXT_FAINT; break;
    case FM_KIND_PROG:                                       /* у каждой программы /bin свой значок; неизвестная - общий */
        *icon = ICON_TERMINAL; *col = TH_GREEN;
        if (name && app_info(name)) { *icon = dock_icon(name); *col = dock_color(name); }
        break;
    case FM_KIND_IMG: *icon = ICON_BRUSH; *col = TH_PINK; break;
    case FM_KIND_TEXT: *icon = ICON_FILE; *col = TH_CYAN; break;
    case FM_KIND_WASM: *icon = ICON_CUBE; *col = TH_PURPLE; break;
    default: break;
    }
}

static int vol_of(const exp_view_t* e, const fm_panel_t* P, const fm_ent_t* r) {
    if (!(P->path[0] == '/' && !P->path[1]) || !r->dir) return -1;
    for (uint32_t k = 0; k < e->nvol && k < EXP_VOLS; k++) {
        const char* a = r->name; const char* b = e->vol[k].name; while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return (int)k;
    }
    return -1;
}

static void draw_panel(surface_t* s, const exp_view_t* e, int p, const app_hit_t* hover, int drop_panel) {
    const fm_panel_t* P = &e->fm.p[p];
    int px = PX(p), act = e->fm.active == p;
    char t[200];
    ui_card(s, px, PY, PW, PH, 0, 0);
    if (act) ui_round_frame(s, px, PY, PW, PH, TH_RADIUS, TH_CYAN, 190);
    if (drop_panel == p) ui_round_frame(s, px + 1, PY + 1, PW - 2, PH - 2, TH_RADIUS - 1, TH_GREEN, 255);
    ui_round_rect(s, px + 6, PY + 5, PW - 12, 22, 6, act ? ui_mix(TH_CARD, TH_CYAN, 40) : TH_TRACK, 255);
    icon_draw(s, px + 20, PY + 16, 14, ICON_FOLDER, act ? TH_CYAN : TH_TEXT_FAINT, 0);
    path_fit(s, px + 34, PY + 8, P->path, PW - 46, act ? TH_TEXT : TH_TEXT_DIM);
    gfx_text(s, px + 26, PY + 28, "Имя", TH_TEXT_FAINT, -1);
    gfx_text(s, px + PW - 14 - 56 - 8 - gfx_text_width("Размер"), PY + 28, "Размер", TH_TEXT_FAINT, -1);
    gfx_text(s, px + PW - 14 - 56 + 2, PY + 28, "Тип", TH_TEXT_FAINT, -1);
    gfx_rect(s, px + 8, PY + ROW0 - 4, PW - 16, 1, TH_CARD_EDGE);
    if (!P->n) gfx_text(s, px + 16, PY + ROW0 + 4, e->mounted ? "пусто" : "диска нет", TH_TEXT_FAINT, -1);
    for (int i = 0; i < FM_ROWS && P->top + i < (int)P->n; i++) {
        int idx = P->top + i;
        const fm_ent_t* r = &P->e[idx];
        int y = PY + ROW0 + i * ROW_H;
        int vol = vol_of(e, P, r);
        if (idx == P->cur) {
            ui_round_rect(s, px + 4, y, PW - 18, ROW_H - 1, 5, TH_CYAN, act ? 66 : 24);
            if (act) ui_round_frame(s, px + 4, y, PW - 18, ROW_H - 1, 5, TH_CYAN, 150);
        } else if (hit_eq(hover, APP_HIT_ROW, p, idx)) ui_round_rect(s, px + 4, y, PW - 18, ROW_H - 1, 5, TH_CYAN, 16);
        else if (i & 1) ui_round_rect(s, px + 4, y, PW - 18, ROW_H - 1, 5, TH_ROW_ALT, 255);
        if (r->mark) gfx_rect(s, px + 5, y + 2, 3, ROW_H - 5, TH_AMBER);
        int icon; uint32_t ic; kind_icon(r->kind, vol >= 0, r->name, &icon, &ic);
        icon_draw(s, px + 20, y + 9, 14, icon, ic, 0);
        uint32_t nc = r->mark ? TH_AMBER : r->kind == FM_KIND_UP ? TH_TEXT_DIM : r->dir ? TH_TEXT : TH_TEXT_DIM;
        if (idx == P->cur) nc = r->mark ? TH_AMBER : TH_TEXT;
        int sizew = 66, namew = PW - 26 - 8 - 56 - sizew - 14;
        const char* shown = r->kind == FM_KIND_PROG ? app_label(r->name) : r->name;   /* /bin: русское название, сортировка и запуск - по системному имени */
        int nl = gfx_text_width(shown); if (nl > namew) nl = namew;
        ui_text_fit(s, px + 32, y + 1, shown, namew, nc);
        if (r->kind == FM_KIND_PROG && shown != r->name) {                   /* системное имя тускло рядом, пока помещается */
            int lw = namew - nl - 8;
            if (lw > 0 && gfx_text_width(r->name) <= lw) gfx_text(s, px + 32 + nl + 8, y + 1, r->name, TH_TEXT_FAINT, -1);
        }
        if (vol >= 0) {                                                      /* метка флешки после имени */
            int lw = namew - nl - 8;
            if (lw > 24) ui_text_fit(s, px + 32 + nl + 8, y + 1, e->vol[vol].label, lw, TH_GREEN);
            fmt_size(t, (uint64_t)e->vol[vol].total_kb * 1024);
        } else if (r->dir) t[0] = 0;
        else fmt_size(t, r->size);
        if (t[0]) gfx_text(s, px + PW - 14 - 56 - 8 - gfx_text_width(t), y + 1, t, TH_TEXT_FAINT, -1);
        gfx_text(s, px + PW - 14 - 56 + 2, y + 1, kind_name(r->kind, vol >= 0), vol >= 0 ? TH_GREEN : ic == TH_TEXT_DIM ? TH_TEXT_FAINT : ic, -1);
    }
    if ((int)P->n > FM_ROWS) {                                               /* полоса прокрутки */
        int th = FM_ROWS * ROW_H * FM_ROWS / (int)P->n; if (th < 14) th = 14;
        int ty = PY + ROW0 + (FM_ROWS * ROW_H - th) * P->top / ((int)P->n - FM_ROWS);
        ui_round_rect(s, px + PW - 9, PY + ROW0, 4, FM_ROWS * ROW_H, 2, TH_TRACK, 255);
        ui_round_rect(s, px + PW - 9, ty, 4, th, 2, act ? TH_CYAN : TH_TEXT_FAINT, 255);
    }
    int fy = PY + PH - 20;
    gfx_rect(s, px + 8, fy - 3, PW - 16, 1, TH_CARD_EDGE);
    int q = cat_u(t, 0, P->n > 0 && P->e[0].kind == FM_KIND_UP ? P->n - 1 : P->n);
    q = cat(t, q, " объектов"); if (P->trunc) q = cat(t, q, "+");
    if (P->hidden && !e->fm.show_hidden) { q = cat(t, q, " · скрыто "); q = cat_u(t, q, P->hidden); }
    gfx_text(s, px + 12, fy, t, TH_TEXT_FAINT, -1);
    if (P->nmark) {
        char z[24]; fmt_size(z, P->mark_bytes);
        q = cat(t, 0, "отмечено "); q = cat_u(t, q, P->nmark); q = cat(t, q, " · "); cat(t, q, z);
        gfx_text(s, px + PW - 12 - gfx_text_width(t), fy, t, TH_AMBER, -1);
    }
}

static void draw_dialog_frame(surface_t* s, int w, int h, const char* title, uint32_t accent, int* x, int* y) {
    *x = (APP_FILES_W - w) / 2; *y = 130;
    ui_glow(s, *x, *y, w, h, TH_RADIUS, 5, accent, 150);
    ui_round_rect(s, *x, *y, w, h, TH_RADIUS, TH_PANEL1, 255);
    ui_round_rect(s, *x, *y, w, 30, TH_RADIUS, TH_PANEL0, 255);
    gfx_rect(s, *x + 1, *y + TH_RADIUS, w - 2, 30 - TH_RADIUS, TH_PANEL0);
    ui_round_frame(s, *x, *y, w, h, TH_RADIUS, ui_mix(TH_BORDER, accent, 160), 255);
    gfx_rect(s, *x + 14, *y + 9, 3, 13, accent);
    ui_text_bold(s, *x + 24, *y + 7, title, TH_TEXT);
}

static void draw_help(surface_t* s) {
    static const char* const ln[] = {
        "Tab / ← →    - другая панель", "↑ ↓ PgUp PgDn Home End - курсор", "Enter        - войти / открыть, Backspace - вверх",
        "Insert, пробел - отметить", "Esc          - снять отметки", "F2 имя  F3 вид  F4 закрепить программу /bin",
        "F5 копия  F6 перенос - в каталог другой панели", "F7 новый каталог  F8 / Del удалить", "F9 служебные файлы  F10 свернуть окно",
        "мышь: щелчок - выбор, двойной - открыть,", "перетащить файл на другую панель - копия",
    };
    int n = (int)(sizeof ln / sizeof ln[0]), x, y;
    draw_dialog_frame(s, 440, 56 + n * 18, "Клавиши Проводника", TH_BLUE, &x, &y);
    for (int i = 0; i < n; i++) gfx_text(s, x + 18, y + 42 + i * 18, ln[i], TH_TEXT_DIM, -1);
}

static void draw_dialog(surface_t* s, const fm_t* f) {
    int x, y;
    if (f->mode == FM_MODE_HELP) { draw_help(s); return; }
    if (f->mode == FM_MODE_INPUT) {
        draw_dialog_frame(s, 420, 124, f->dlg_title, TH_CYAN, &x, &y);
        gfx_text(s, x + 18, y + 40, f->dlg_text, TH_TEXT_DIM, -1);
        ui_round_rect(s, x + 16, y + 62, 388, 28, 7, TH_TRACK, 255);
        ui_round_frame(s, x + 16, y + 62, 388, 28, 7, TH_CYAN, 200);
        ui_text_fit(s, x + 24, y + 68, f->input, 360 - 8, TH_TEXT);
        int cw = gfx_text_width(f->input); if (cw > 352) cw = 352;
        gfx_rect(s, x + 24 + cw, y + 68, 2, 16, TH_CYAN);
        gfx_text(s, x + 18, y + 98, "Enter - принять · Esc - отмена", TH_TEXT_FAINT, -1);
    } else if (f->mode == FM_MODE_CONFIRM) {
        draw_dialog_frame(s, 440, 112, f->dlg_title, TH_RED, &x, &y);
        text_wrap(s, x + 18, y + 40, f->dlg_text, 404, 2, 18, TH_TEXT);
        ui_button(s, x + 18, y + 72, 150, 26, "Enter - да, удалить", TH_RED, 0, 1);
        ui_button(s, x + 178, y + 72, 120, 26, "Esc - нет", TH_CYAN, 0, 0);
    } else if (f->mode == FM_MODE_BUSY) {
        draw_dialog_frame(s, 440, 128, f->dlg_title, TH_GREEN, &x, &y);
        char t[128]; int p = cat(t, 0, f->prog_name); (void)p;
        ui_text_fit(s, x + 18, y + 40, t, 404, TH_TEXT);
        int pct = -1;
        if (f->prog_total) pct = (int)(f->prog_done * 100 / f->prog_total);
        else if (f->prog_files_total) pct = (int)((uint64_t)f->prog_files * 100 / f->prog_files_total);
        ui_round_rect(s, x + 18, y + 64, 404, 14, 6, TH_TRACK, 255);
        if (pct >= 0) {
            int fw = 404 * (pct > 100 ? 100 : pct) / 100;
            if (fw >= 8) { ui_round_rect(s, x + 18, y + 64, fw, 14, 6, ui_mix(TH_CYAN, TH_PURPLE, (unsigned)pct * 255 / 100), 255); }
        }
        char a[24], b[24];
        p = 0;
        if (f->prog_total) { fmt_size(a, f->prog_done); fmt_size(b, f->prog_total); p = cat(t, p, a); p = cat(t, p, " из "); p = cat(t, p, b); p = cat(t, p, " · "); }
        p = cat(t, p, "объектов "); p = cat_u(t, p, f->prog_files);
        if (f->prog_files_total) { p = cat(t, p, " из "); p = cat_u(t, p, f->prog_files_total); }
        if (pct >= 0) { p = cat(t, p, " · "); p = cat_u(t, p, (uint64_t)(pct > 100 ? 100 : pct)); cat(t, p, "%"); }
        gfx_text(s, x + 18, y + 86, t, TH_TEXT_DIM, -1);
        gfx_text(s, x + 18, y + 104, "Esc - прервать", TH_TEXT_FAINT, -1);
    }
}

void app_files_draw(surface_t* s, const exp_view_t* e, const app_hit_t* hover, int drop_panel, const char* status, int pinned) {
    gfx_fill(s, TH_CONTENT);
    char t[200]; int p;
    icon_draw(s, 24, 18, 20, ICON_FOLDER, TH_AMBER, 0);
    if (e->mounted) {
        char a[24], b[24]; app_fmt_kb(a, e->total_kb - e->free_kb); app_fmt_kb(b, e->total_kb);
        p = cat(t, 0, "KiraFS · занято "); p = cat(t, p, a); p = cat(t, p, " из "); p = cat(t, p, b);
        p = cat(t, p, " · объектов "); p = cat_u(t, p, e->files);
        if (e->nvol) { p = cat(t, p, " · флешек "); p = cat_u(t, p, e->nvol); if (e->rovol) { p = cat(t, p, " · только чтение "); cat_u(t, p, e->rovol); } }
    } else cat(t, 0, "Диск с KiraFS не подключён: проводнику нечего показать");
    ui_text_fit(s, 42, 10, t, APP_FILES_W - 42 - 2 * BTN_W - 40, e->mounted ? TH_TEXT_DIM : TH_AMBER);
    int rx = APP_FILES_W - TH_GAP - BTN_W;
    ui_button(s, rx, BTN_Y, BTN_W, 26, "обновить", TH_CYAN, hover && hover->kind == APP_HIT_REFRESH, 0);
    ui_button(s, rx - 8 - BTN_W, BTN_Y, BTN_W, 26, e->fm.show_hidden ? "служеб.: да" : "служеб.: нет", TH_BLUE, hover && hover->kind == APP_HIT_HIDDEN, e->fm.show_hidden);
    draw_panel(s, e, 0, hover, drop_panel);
    draw_panel(s, e, 1, hover, drop_panel);

    /* полоса состояния: итог операции или сведения о строке под курсором */
    const fm_t* f = &e->fm;
    const fm_panel_t* P = &f->p[f->active];
    const char* left = ""; uint32_t lc = TH_TEXT_DIM; char info[256];
    if (f->msg[0]) { left = f->msg; lc = f->msg_err ? TH_RED : TH_GREEN; }
    else if (status && status[0]) { left = status; lc = TH_GREEN; }
    else if (P->n && P->cur < (int)P->n) {
        const fm_ent_t* r = &P->e[P->cur];
        p = cat(info, 0, r->kind == FM_KIND_UP ? "вверх, в родительский каталог" : r->name);   /* системное имя */
        if (r->kind == FM_KIND_PROG && app_info(r->name)) { p = cat(info, p, " · "); p = cat(info, p, app_label(r->name)); }
        if (r->kind != FM_KIND_UP) {
            p = cat(info, p, " · "); p = cat(info, p, r->dir ? "каталог" : kind_name(r->kind, 0));
            if (!r->dir) { char z[24]; fmt_size(z, r->size); p = cat(info, p, " · "); cat(info, p, z); }
        }
        left = info;
    }
    char right[64]; p = cat(right, 0, f->active ? "правая панель" : "левая панель");
    if (!f->show_hidden && P->hidden) { p = cat(right, p, " · скрыто "); cat_u(right, p, P->hidden); }
    ui_statusbar(s, STAT_Y, left, lc, right);

    ui_key_t k[9];
    for (int i = 0; i < 9; i++) k[i] = fkeys[i];
    if (pinned) k[3].label = "Открепить";
    ui_keybar(s, KEYS_Y, k, 9, hover && hover->kind == APP_HIT_KEY ? hover->row : -1);
    if (f->mode != FM_MODE_NORMAL) draw_dialog(s, f);
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
    ui_statusbar(s, s->h - TH_BAR_H, "щелчок по кнопке применяет настройку сразу", TH_TEXT_DIM, st->mode ? "Разум действует" : "Разум наблюдает");
}
