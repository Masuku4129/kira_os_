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
    case MJ_KILL: case MJ_BEHAVIOR: return TH_RED;
    default: return TH_TEXT_DIM;
    }
}




/* Панель Mind читает только опубликованный снимок ядра.
   Разделы - крупная стеклянная панель сверху; страницы рисуются ниже неё в подповерхности (их координаты не меняются). */
#define MIND_NAV_H 46
#define MIND_NAV_W 580
static const char* const mind_names[6] = { "Состояние", "Прогноз", "Обучение", "Решения", "Процессы", "Оборудование" };
/* Ширина ячейки пропорциональна подписи, вместе они занимают всю панель. */
static int mind_nav_geom(int i, int* x) {
    int tw[6], sum = 0;
    for (int k = 0; k < 6; k++) { tw[k] = gfx_text_width(mind_names[k]) + 20; sum += tw[k]; }
    int room = MIND_NAV_W - 8, px = 14;
    for (int k = 0; k < i; k++) px += tw[k] * room / sum;
    *x = px;
    return (i == 5 ? 14 + room - px : tw[i] * room / sum);
}
static void mind_tabs_draw(surface_t* s, unsigned page) {
    gfx_rect(s, 0, 0, s->w, MIND_NAV_H, TH_CONTENT);
    ui_round_rect(s, 10, 7, MIND_NAV_W, 34, 17, TH_BAR0, 255);
    ui_round_frame(s, 10, 7, MIND_NAV_W, 34, 17, ui_mix(TH_BORDER, TH_CYAN, 50), 255);
    for (int i = 0; i < 6; i++) {
        int x, cw = mind_nav_geom(i, &x);
        if (i == (int)page) {
            ui_round_rect(s, x, 10, cw, 28, 14, TH_CYAN, 52);
            ui_round_frame(s, x, 10, cw, 28, 14, TH_CYAN, 130);
        }
        int lw = gfx_text_width(mind_names[i]); if (lw > cw - 8) lw = cw - 8;
        ui_text_fit(s, x + (cw - lw) / 2, 16, mind_names[i], cw - 8, i == (int)page ? TH_CYAN : TH_TEXT_DIM);
    }
}
int app_mind_hit(int x, int y) {
    if (y < 10 || y >= 38) return -1;
    for (int i = 0; i < 6; i++) { int px, cw = mind_nav_geom(i, &px); if (x >= px && x < px + cw) return i; }
    return -1;
}
static void mind_page_status(surface_t* s, const mind_view_t* v, const char* msg) {
    char r[32]; int p = cat(r, 0, "здоровье "); p = cat_u(r, p, v->health); cat(r, p, "%");
    ui_statusbar(s, s->h - TH_BAR_H, msg, TH_TEXT_DIM, r);
}

static void m0_health(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    /* здоровье: одно крупное число и четыре подписанные шкалы, из которых оно сложено */
    card(s, 10, 10, 200, 320, "ЗДОРОВЬЕ СИСТЕМЫ", health_color(v->health));
    char hv[8]; app_fmt_u(hv, v->health);
    int hw = ui_text_smooth_width(hv, 56);
    ui_text_smooth(s, 110 - (hw + 36) / 2, 38, hv, 56, health_color(v->health));
    gfx_text(s, 110 - (hw + 36) / 2 + hw + 4, 72, "/100", TH_TEXT_FAINT, -1);
    const char* mood = v->mood[0] ? v->mood : "просыпается";
    { int mw = gfx_text_width(mood); if (mw > 190) mw = 190; ui_text_fit(s, 110 - mw / 2, 108, mood, 190, TH_TEXT); }      /* длинный текст не вылезает за карточку: кэш не оставит следов */
    p = cat(t, 0, v->mode ? "действует" : "наблюдает");
    p = cat(t, p, " · квант "); p = cat_u(t, p, v->quantum_ms); cat(t, p, " мс");
    { int tw = gfx_text_width(t); if (tw > 190) tw = 190; ui_text_fit(s, 110 - tw / 2, 128, t, 190, v->mode ? TH_GREEN : TH_TEXT_DIM); }
    gfx_rect(s, 22, 156, 176, 1, CARD_EDGE);
    static const char* const part[4] = { "отклик", "запас", "равновесие", "стабильность" };
    static const uint32_t pcol[4] = { TH_CYAN, TH_GREEN, TH_PURPLE, TH_AMBER };
    const unsigned pval[4] = { v->c_resp, v->c_room, v->c_bal, v->c_stab };
    for (int i = 0; i < 4; i++) {
        int y = 168 + i * 36;
        gfx_text(s, 24, y, part[i], TH_TEXT_DIM, -1);
        app_fmt_u(t, pval[i]);
        gfx_text(s, 196 - gfx_text_width(t), y, t, pval[i] >= 70 ? TH_TEXT : pval[i] >= 40 ? TH_AMBER : TH_RED, -1);
        ui_round_rect(s, 24, y + 20, 172, 6, 3, TRACK, 255);
        unsigned pv = pval[i] > 100 ? 100 : pval[i];
        if (pv) ui_round_rect(s, 24, y + 20, (int)(172 * pv / 100) < 6 ? 6 : (int)(172 * pv / 100), 6, 3, pcol[i], 255);
    }

}

static void m0_sigframe(surface_t* s, const mind_view_t* v) {
    (void)v;
    card(s, 220, 10, 370, 320, "СИГНАЛЫ И НОРМЫ", TH_CYAN);
}
/* Одна строка сигнала: значение, отклонение от нормы, минута истории. Строка - отдельная карточка кэша, поэтому перерисовка
   сигналов делится на кадры (8 маленьких частей вместо одной крупной). */
static void m0_sig_row(surface_t* s, const mind_view_t* v, int k) {
    char t[128]; int p;
    int y = 38 + k * 36;
    if (k == 0) ui_vgrad(s, 232, 39, 346, 1, TH_CYAN, TH_CARD_EDGE);       /* линия под заголовком карточки: стирание строки задевает её, рисуем снова */
    int az = v->z_x10[k] < 0 ? -v->z_x10[k] : v->z_x10[k];
    int learning = v->learned[k] < 60;
    uint32_t vc = learning ? TH_TEXT_DIM : az >= 40 ? TH_RED : az >= 25 ? TH_AMBER : TH_TEXT;
    gfx_text(s, 232, y + 2, mind_signal_name(k), TH_TEXT_DIM, -1);
    p = cat_x100(t, 0, v->cur_x100[k]); cat(t, p, short_unit[k]);
    { int tw = gfx_text_width(t); if (tw > 216) tw = 216; ui_text_fit(s, 446 - tw, y + 2, t, 216, vc); }   /* не шире строки: не вылезает за карточку */
    uint32_t sc = learning ? TH_TEXT_FAINT : az >= 25 ? vc : ui_mix(TH_CYAN, TH_PURPLE, (unsigned)k * 32);
    gfx_rect(s, 456, y + 20, 124, 1, TRACK);
    spark(s, 456, y + 1, 124, 20, v->hist[k], v->hpos, sc, 1);
}
#define SIGROW(n) static void m0_sig##n(surface_t* s, const mind_view_t* v) { m0_sig_row(s, v, n); }
SIGROW(0) SIGROW(1) SIGROW(2) SIGROW(3) SIGROW(4) SIGROW(5) SIGROW(6) SIGROW(7)
_Static_assert(MIND_SIG_N == 8, "строки сигналов окна Разума: по карточке на сигнал");
static void m0_counters(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
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

}

static void m0_journal(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
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

static void m0_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    /* полоса состояния: последнее решение и здоровье */
    char hr[32]; p = cat(hr, 0, "здоровье "); p = cat_u(hr, p, v->health); cat(hr, p, "%");
    if (v->nj) ui_statusbar(s, s->h - TH_BAR_H, v->j[0].text, kind_color(v->j[0].kind), hr);
    else ui_statusbar(s, s->h - TH_BAR_H, "Разум изучает нормы этой машины", TH_TEXT_DIM, hr);
}

static void m1_risk(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 10, 580, 145, "ПРОГНОЗ ОТКЛИКА ЭКРАНА · СЛЕДУЮЩАЯ СЕКУНДА", TH_CYAN);
    unsigned risk = v->risk > 100 ? 100 : v->risk;
    uint32_t c = risk >= 70 ? TH_RED : risk >= 40 ? TH_AMBER : TH_GREEN;
    p = cat(t, 0, "вероятность опоздания: "); p = cat_u(t, p, risk); cat(t, p, "%");
    ui_text_bold(s, 28, 54, t, c);
    ui_round_rect(s, 28, 84, 540, 18, 8, TRACK, 255);
    if (risk) ui_round_rect(s, 28, 84, (int)(540 * risk / 100), 18, 8, c, 255);
    if (v->net_trusted) {
        const char* why = v->risk_why >= 0 && v->risk_why < MNET_F ? mind_feature_name(v->risk_why) : "признак не выделен";
        p = cat(t, 0, "главный фактор: "); cat(t, p, why); gfx_text(s, 28, 120, t, TH_TEXT_DIM, -1);
    } else gfx_text(s, 28, 120, "Модель пока не заслужила доверия: предупреждения не используются", TH_AMBER, -1);
}

static void m1_history(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 165, 280, 160, "ИСТОРИЯ ПРОГНОЗОВ", TH_PURPLE);
    p = cat(t, 0, "прогнозов: "); p = cat_u(t, p, v->forecasts); gfx_text(s, 26, 204, t, TH_TEXT, -1);
    p = cat(t, 0, "примеров: "); p = cat_u(t, p, v->net_n); gfx_text(s, 26, 232, t, TH_TEXT_DIM, -1);
    p = cat(t, 0, "положительных: "); p = cat_u(t, p, v->net_pos); gfx_text(s, 26, 260, t, TH_TEXT_DIM, -1);
}

static void m1_quality(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 300, 165, 290, 160, "КАЧЕСТВО ПРЕДУПРЕЖДЕНИЙ", TH_GREEN);
    p = cat(t, 0, "точность: "); p = cat_u(t, p, v->net_prec); cat(t, p, "%"); gfx_text(s, 316, 204, t, TH_TEXT, -1);
    p = cat(t, 0, "полнота: "); p = cat_u(t, p, v->net_rec); cat(t, p, "%"); gfx_text(s, 316, 232, t, TH_TEXT, -1);
    p = cat(t, 0, "доверие: "); cat(t, p, v->net_trusted ? "подтверждено" : "не подтверждено");
    gfx_text(s, 316, 260, t, v->net_trusted ? TH_GREEN : TH_AMBER, -1);
}

static void m1_limits(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 335, 580, 190, "ОГРАНИЧЕНИЯ ПРОГНОЗА", TH_BLUE);
    gfx_text(s, 26, 376, "Оценивается только вероятность задержки экрана на следующей секунде.", TH_TEXT_DIM, -1);
    gfx_text(s, 26, 402, "Это не универсальный прогноз производительности всей системы.", TH_TEXT_DIM, -1);
    gfx_text(s, 26, 438, "При недостаточном доверии модель не используется для предупреждений.", TH_TEXT_FAINT, -1);
    gfx_text(s, 26, 474, "Причина указана, только если ядро выделило значимый признак.", TH_TEXT_FAINT, -1);
}

static void m1_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    mind_page_status(s, v, "прогноз · диагностическая оценка, не команда управления");
}

static void m2_state(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 10, 580, 125, "СОСТОЯНИЕ ОБУЧЕНИЯ", TH_PURPLE);
    p = cat(t, 0, "примеров: "); p = cat_u(t, p, v->net_n);
    p = cat(t, p, " · положительных: "); p = cat_u(t, p, v->net_pos); gfx_text(s, 26, 48, t, TH_TEXT, -1);
    p = cat(t, 0, "точность: "); p = cat_u(t, p, v->net_prec); p = cat(t, p, "% · полнота: ");
    p = cat_u(t, p, v->net_rec); cat(t, p, "%"); gfx_text(s, 26, 74, t, TH_TEXT_DIM, -1);
    p = cat(t, 0, "потеря: "); p = cat_u(t, p, v->net_loss_x1e4 / 10000); p = cat(t, p, ".");
    { unsigned f = v->net_loss_x1e4 % 10000; t[p++] = (char)('0' + f / 1000); t[p++] = (char)('0' + f / 100 % 10); t[p++] = (char)('0' + f / 10 % 10); t[p++] = (char)('0' + f % 10); t[p] = 0; }
    gfx_text(s, 26, 100, t, TH_TEXT_DIM, -1);
    gfx_text(s, 370, 100, v->net_trusted ? "модель доверенная" : "модель не доверенная", v->net_trusted ? TH_GREEN : TH_AMBER, -1);
}

static void m2_weights(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 145, 580, 380, "ВЕСА ПРИЗНАКОВ · Q16", TH_CYAN);
    for (int i = 0; i < MNET_F; i++) {
        int x = i / 6 ? 310 : 26, y = 182 + (i % 6) * 52;
        int32_t value = v->net_w[i]; int64_t mag = value; if (mag < 0) mag = -mag;
        char w[24]; int q = 0; if (value < 0) w[q++] = '-';
        q = cat_u(w, q, (uint64_t)mag / MNET_ONE);
        unsigned f = (unsigned)(((uint64_t)mag % MNET_ONE) * 100 / MNET_ONE);
        w[q++] = '.'; w[q++] = (char)('0' + f / 10); w[q++] = (char)('0' + f % 10); w[q] = 0;
        text_fit(s, x, y, mind_feature_name(i), 164, TH_TEXT_DIM);
        gfx_text(s, x, y + 19, w, TH_CYAN, -1);
        ui_round_rect(s, x + 112, y + 23, 145, 5, 2, TRACK, 255);
        unsigned fill = (unsigned)(mag > 4LL * MNET_ONE ? 4LL * MNET_ONE : mag) * 145 / (4 * MNET_ONE);
        if (fill) ui_round_rect(s, x + 112, y + 23, (int)fill, 5, 2, value < 0 ? TH_AMBER : TH_GREEN, 255);
    }
}

static void m2_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    mind_page_status(s, v, v->lv_seed ? "обучение · стартовая модель содержит семя опыта" : "обучение · опыт текущей машины");
}

static void m3_summary(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 10, 580, 90, "СВОДКА РЕШЕНИЙ", TH_PURPLE);
    p = cat(t, 0, "решений: "); p = cat_u(t, p, v->decisions);
    p = cat(t, p, " · отклонений: "); p = cat_u(t, p, v->anomalies);
    p = cat(t, p, " · остановок: "); p = cat_u(t, p, v->kills); gfx_text(s, 26, 48, t, TH_TEXT, -1);
    uint32_t lever_backs = 0;
    for (int i = 0; i < ML_N; i++) lever_backs += v->lv[i].backs;
    p = cat(t, 0, "изменений рычагов: "); p = cat_u(t, p, v->lv_changes);
    p = cat(t, p, " · их откатов: "); p = cat_u(t, p, lever_backs);
    p = cat(t, p, " · общих: "); p = cat_u(t, p, v->lv_general); gfx_text(s, 26, 74, t, TH_TEXT_DIM, -1);
}

static void m3_levers(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 110, 580, 415, "АДАПТИВНЫЕ ПАРАМЕТРЫ · 13–30", TH_CYAN);
    for (int i = 0; i < ML_N; i++) {
        int x = i / 9 ? 310 : 26, y = 142 + (i % 9) * 40;
        const char* state = v->lv[i].state == 2 ? "проверка" : v->lv[i].state == 1 ? "изменён" : v->lv[i].state == 3 ? "не помогает" : "по умолчанию";
        uint32_t c = v->lv[i].state == 2 ? TH_AMBER : v->lv[i].state == 1 ? TH_GREEN : v->lv[i].state == 3 ? TH_TEXT_FAINT : TH_TEXT_DIM;
        text_fit(s, x, y, mind_lever_name(i), 164, TH_TEXT);
        char val[40]; int q = cat(val, 0, "№"); q = cat_u(val, q, 13 + i); q = cat(val, q, "  ");
        if (v->lv[i].val < 0) { q = cat(val, q, "-"); q = cat_u(val, q, (uint64_t)-(int64_t)v->lv[i].val); }
        else q = cat_u(val, q, (uint64_t)v->lv[i].val);
        cat(val, q, mind_lever_unit(i)); text_fit(s, x, y + 17, val, 164, TH_CYAN);
        text_fit(s, x + 170, y + 5, state, 94, c);
        char stats[48]; q = cat(stats, 0, "пробы "); q = cat_u(stats, q, v->lv[i].trials);
        q = cat(stats, q, " · успех "); q = cat_u(stats, q, v->lv[i].wins); text_fit(s, x + 170, y + 21, stats, 94, TH_TEXT_FAINT);
    }
}

static void m3_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    mind_page_status(s, v, "решения · параметры ядра проверяются и откатываются при ухудшении");
}

static void m4_list(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 10, 580, 515, "ПРОЦЕССЫ · СНИМОК ЯДРА", TH_BLUE);
    static const int x[6] = { 24, 72, 224, 284, 360, 444 };
    static const char* const h[6] = { "PID", "ПРОГРАММА", "ПОТОКИ", "ЦП", "ПАМЯТЬ", "ВЕС / ЛИМИТ" };
    for (int i = 0; i < 6; i++) gfx_text(s, x[i], 44, h[i], TH_TEXT_FAINT, -1);
    gfx_rect(s, 22, 64, 556, 1, CARD_EDGE);
    unsigned n = v->nproc > 16 ? 16 : v->nproc;
    if (!n) gfx_text(s, 26, 82, "активных пользовательских процессов нет", TH_TEXT_FAINT, -1);
    for (unsigned i = 0; i < n; i++) {
        int y = 72 + (int)i * 26; if (i & 1) ui_round_rect(s, 18, y - 3, 564, 24, 4, TH_ROW_ALT, 255);
        char t[32]; app_fmt_u(t, v->p[i].pid); gfx_text(s, x[0], y, t, TH_TEXT_DIM, -1);
        text_fit(s, x[1], y, app_label(v->p[i].name), 144, v->p[i].flagged ? TH_RED : v->p[i].focus ? TH_CYAN : TH_TEXT);
        app_fmt_u(t, v->p[i].threads); gfx_text(s, x[2], y, t, TH_TEXT_DIM, -1);
        int q = cat_u(t, 0, v->p[i].cpu_x10 / 10); t[q++] = '.'; t[q++] = (char)('0' + v->p[i].cpu_x10 % 10); t[q++] = '%'; t[q] = 0;
        gfx_text(s, x[3], y, t, v->p[i].cpu_x10 >= 850 ? TH_RED : TH_TEXT_DIM, -1);
        app_fmt_kb(t, v->p[i].heap_kb); gfx_text(s, x[4], y, t, TH_TEXT_DIM, -1);
        q = cat(t, 0, "в"); q = cat_u(t, q, v->p[i].weight); q = cat(t, q, "/");
        if (!v->p[i].limit || v->p[i].limit >= 100) cat(t, q, "—");
        else { q = cat_u(t, q, v->p[i].limit); cat(t, q, "%"); }
        text_fit(s, x[5], y, t, 128, TH_TEXT_DIM);
    }
}

static void m4_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    mind_page_status(s, v, "процессы · до 16 записей из системного снимка");
}

static void m5_cpu(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 10, 580, 145, "ПРОЦЕССОР · ЗАГРУЗКА ЯДЕР", TH_CYAN);
    unsigned n = v->cores > 64 ? 64 : v->cores;
    if (!n) {
        /* Не подменять отсутствующий снимок одним фиктивным ядром с нулевой загрузкой. */
        gfx_text(s, 30, 72, "нет данных о числе доступных ядер", TH_TEXT_FAINT, -1);
        gfx_text(s, 30, 96, "загрузка процессора пока не измерена", TH_TEXT_FAINT, -1);
    } else {
        int slot = 540 / (int)n, bw = slot - 4; if (bw > 48) bw = 48; if (bw < 2) bw = 2;
        int x0 = 30 + (540 - slot * (int)n) / 2 + (slot - bw) / 2;
        for (unsigned i = 0; i < n; i++) {
            int x = x0 + (int)i * slot; unsigned b = v->busy[i] > 100 ? 100 : v->busy[i];
            ui_round_rect(s, x, 48, bw, 78, bw >= 8 ? 4 : 0, TRACK, 255);
            int hh = (int)(b * 78 / 100); if (hh < 2) hh = 2;
            ui_round_rect(s, x, 126 - hh, bw, hh, bw >= 8 ? 4 : 0, b >= 85 ? TH_RED : b >= 60 ? TH_AMBER : TH_CYAN, 255);
            if (slot >= 22) { char t[12]; app_fmt_u(t, i); gfx_text(s, x + bw / 2 - gfx_text_width(t) / 2, 132, t, TH_TEXT_FAINT, -1); }
        }
    }
}

static void m5_mem(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 165, 280, 145, "ПАМЯТЬ", TH_PURPLE);
    p = cat(t, 0, "занято "); p = cat_u(t, p, v->mem_used); cat(t, p, "%");
    ui_text_bold(s, 26, 208, t, v->mem_used >= 85 ? TH_RED : TH_TEXT);
    ui_round_rect(s, 26, 236, 248, 14, 6, TRACK, 255);
    if (v->mem_used) ui_round_rect(s, 26, 236, (int)(248 * (v->mem_used > 100 ? 100 : v->mem_used) / 100), 14, 6, v->mem_used >= 85 ? TH_RED : TH_PURPLE, 255);
    p = cat(t, 0, "готовых потоков: "); cat_u(t, p, v->runnable); gfx_text(s, 26, 272, t, TH_TEXT_DIM, -1);
}

static void m5_screen(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 300, 165, 290, 145, "ЭКРАН И КОМПОЗИТОР", TH_GREEN);
    p = cat(t, 0, "кадров/с: "); p = cat_u(t, p, v->frames_per_s); gfx_text(s, 316, 204, t, TH_TEXT, -1);
    p = cat(t, 0, "опозданий/с: "); p = cat_u(t, p, v->late_per_s); gfx_text(s, 316, 230, t, v->late_per_s ? TH_AMBER : TH_GREEN, -1);
    p = cat(t, 0, "сборка: "); cat(t, p, v->comp_par ? "параллельная" : "одно ядро"); gfx_text(s, 316, 256, t, TH_TEXT_DIM, -1);
}

static void m5_policy(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    card(s, 10, 320, 580, 205, "ПОЛИТИКИ РЕСУРСОВ", TH_BLUE);
    gfx_text(s, 26, 360, "окна: открытые 60 кадров/с, свёрнутые 4", TH_TEXT_DIM, -1);
    p = cat(t, 0, "лимит фона при нехватке ЦП: "); p = cat_u(t, p, v->core_cap); cat(t, p, "%"); gfx_text(s, 26, 386, t, TH_TEXT_DIM, -1);
    gfx_text(s, 26, 412, "темп кадров и анимацию решает компоновщик", TH_TEXT_FAINT, -1);
    p = cat(t, 0, "композитор, нс/1000 точек: "); p = cat_u(t, p, v->comp_ns_kpx_par);
    p = cat(t, p, " / "); cat_u(t, p, v->comp_ns_kpx_ser); gfx_text(s, 26, 438, t, TH_TEXT_FAINT, -1);
    p = cat(t, 0, "ошибки устройств: "); p = cat_u(t, p, v->dev_errors);
    if (v->dev_errors && v->dev_name[0]) { p = cat(t, p, " · "); cat(t, p, v->dev_name); }
    gfx_text(s, 26, 464, t, v->dev_errors ? TH_AMBER : TH_TEXT_DIM, -1);
    p = cat(t, 0, "подозрительный ввод: ");
    if (v->hid_suspect) {
        p = cat_u(t, p, v->hid_suspect); p = cat(t, p, " · ");
        p = cat(t, p, v->hid_name[0] ? v->hid_name : "устройство"); p = cat(t, p, " · ");
        p = cat_u(t, p, v->hid_rate); cat(t, p, " нажатий/с");
    } else cat(t, p, "не обнаружен");
    gfx_text(s, 26, 486, t, v->hid_suspect ? TH_AMBER : TH_TEXT_DIM, -1);
    p = cat(t, 0, "отклонено запусков: "); p = cat_u(t, p, v->denied);
    if (v->denied && v->denied_name[0]) { p = cat(t, p, " · "); cat(t, p, v->denied_name); }
    gfx_text(s, 26, 508, t, v->denied ? TH_AMBER : TH_TEXT_DIM, -1);
}

static void m5_status(surface_t* s, const mind_view_t* v) {
    char t[128]; int p = 0; (void)t; (void)p; (void)v;
    mind_page_status(s, v, "оборудование · показатели, опубликованные ядром");
}


/* ---------------- окно «Разум»: карточки с кэшем ----------------
   Каждый раздел - набор карточек (прямоугольник, подпись входных значений, рисование). Карточка перерисовывается, только если изменились
   значения, которые она показывает (подпись - хеш этих значений, посчитанный по тем же правилам, что и рисование: например «ядро учится»
   вместо числа секунд наблюдения, класс отклонения вместо самого отклонения). Остальные пиксели остаются в поверхности окна: поверхность живёт
   между вызовами. Перерисовка нескольких карточек делится на кадры по бюджету времени (app_mind_draw_step), поэтому цикл экрана не получает
   разовой паузы в несколько миллисекунд раз в секунду. Хост-тест tests/mind_cards_test.c меняет каждый байт снимка и проверяет, что результат
   с кэшем побайтно равен полной перерисовке. */
#define HZ_SEED 2166136261u
static uint32_t hz(uint32_t h, uint32_t x) { h ^= x; h *= 16777619u; h ^= h >> 13; return h; }
static uint32_t hs(uint32_t h, const char* s) { while (*s) h = hz(h, (uint8_t)*s++); return hz(h, 0x100u); }
static uint32_t hhist(uint32_t h, const uint8_t* hist, uint32_t hpos) {      /* кривая в порядке рисования: сдвиг начала сам по себе пикселей не меняет */
    for (int i = 0; i < MIND_HIST; i++) { unsigned x = hist[(hpos + (unsigned)i) % MIND_HIST]; h = hz(h, x > 100 ? 100 : x); }
    return h;
}

static uint32_t g_m0_health(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    h = hz(h, v->health); h = hs(h, v->mood); h = hz(h, v->mode != 0); h = hz(h, v->quantum_ms);
    h = hz(h, v->c_resp); h = hz(h, v->c_room); h = hz(h, v->c_bal); h = hz(h, v->c_stab);
    return h;
}
static uint32_t g_m0_sig_row(const mind_view_t* v, int k) {
    uint32_t h = HZ_SEED;
    int az = v->z_x10[k] < 0 ? -v->z_x10[k] : v->z_x10[k];
    h = hz(h, (uint32_t)(az >= 40 ? 2 : az >= 25 ? 1 : 0)); h = hz(h, v->learned[k] < 60);
    h = hz(h, (uint32_t)v->cur_x100[k]); h = hhist(h, v->hist[k], v->hpos);
    return h;
}
#define SIGROWG(n) static uint32_t g_m0_sig##n(const mind_view_t* v) { return g_m0_sig_row(v, n); }
SIGROWG(0) SIGROWG(1) SIGROWG(2) SIGROWG(3) SIGROWG(4) SIGROWG(5) SIGROWG(6) SIGROWG(7)
static uint32_t g_m0_counters(const mind_view_t* v) {
    uint32_t h = HZ_SEED; char age[32];
    fmt_age(age, v->lifetime_s); h = hs(h, age);
    h = hz(h, v->decisions); h = hz(h, v->anomalies); h = hz(h, v->tune_wins); h = hz(h, v->tune_trials); h = hz(h, v->kills);
    return h;
}
static uint32_t g_m0_journal(const mind_view_t* v) {
    uint32_t h = HZ_SEED, n = v->nj > MIND_VIEW_J ? MIND_VIEW_J : v->nj;
    h = hz(h, n);
    for (uint32_t i = 0; i < n; i++) { h = hz(h, v->j[i].t); h = hz(h, v->j[i].kind); h = hz(h, v->j[i].applied != 0); h = hs(h, v->j[i].text); }
    return h;
}
static uint32_t g_m0_status(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    if (v->nj) { h = hs(h, v->j[0].text); h = hz(h, v->j[0].kind); } else h = hz(h, 0xFFFFu);
    return h;
}
static uint32_t g_const(const mind_view_t* v) { (void)v; return 1; }
static uint32_t g_health(const mind_view_t* v) { return hz(HZ_SEED, v->health); }
static uint32_t g_m1_risk(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    h = hz(h, v->risk > 100 ? 100 : v->risk); h = hz(h, v->net_trusted != 0);
    if (v->net_trusted) h = hz(h, (uint32_t)(v->risk_why >= 0 && v->risk_why < MNET_F ? v->risk_why : -1));
    return h;
}
static uint32_t g_m1_history(const mind_view_t* v) { return hz(hz(hz(HZ_SEED, v->forecasts), v->net_n), v->net_pos); }
static uint32_t g_m1_quality(const mind_view_t* v) { return hz(hz(hz(HZ_SEED, v->net_prec), v->net_rec), v->net_trusted != 0); }
static uint32_t g_m2_state(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    h = hz(h, v->net_n); h = hz(h, v->net_pos); h = hz(h, v->net_prec); h = hz(h, v->net_rec); h = hz(h, v->net_loss_x1e4); h = hz(h, v->net_trusted != 0);
    return h;
}
static uint32_t g_m2_weights(const mind_view_t* v) { uint32_t h = HZ_SEED; for (int i = 0; i < MNET_F; i++) h = hz(h, (uint32_t)v->net_w[i]); return h; }
static uint32_t g_m2_status(const mind_view_t* v) { return hz(HZ_SEED, v->lv_seed != 0); }
static uint32_t g_m3_summary(const mind_view_t* v) {
    uint32_t h = HZ_SEED, backs = 0;
    for (int i = 0; i < ML_N; i++) backs += v->lv[i].backs;
    h = hz(h, v->decisions); h = hz(h, v->anomalies); h = hz(h, v->kills); h = hz(h, v->lv_changes); h = hz(h, backs); h = hz(h, v->lv_general);
    return h;
}
static uint32_t g_m3_levers(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    for (int i = 0; i < ML_N; i++) {
        h = hz(h, (uint32_t)v->lv[i].val); h = hz(h, v->lv[i].state == 2 ? 2 : v->lv[i].state == 1 ? 1 : v->lv[i].state == 3 ? 3 : 0);
        h = hz(h, v->lv[i].trials); h = hz(h, v->lv[i].wins);
    }
    return h;
}
static uint32_t g_m4_list(const mind_view_t* v) {
    uint32_t h = HZ_SEED, n = v->nproc > 16 ? 16 : v->nproc;
    h = hz(h, n);
    for (uint32_t i = 0; i < n; i++) {
        h = hz(h, v->p[i].pid); h = hs(h, v->p[i].name); h = hz(h, v->p[i].flagged != 0); h = hz(h, v->p[i].focus != 0);
        h = hz(h, v->p[i].threads); h = hz(h, v->p[i].cpu_x10); h = hz(h, v->p[i].heap_kb); h = hz(h, v->p[i].weight);
        h = hz(h, v->p[i].limit && v->p[i].limit < 100 ? v->p[i].limit : 0);
    }
    return h;
}
static uint32_t g_m5_cpu(const mind_view_t* v) {
    uint32_t h = HZ_SEED, n = v->cores > 64 ? 64 : v->cores;
    h = hz(h, n);
    for (uint32_t i = 0; i < n; i++) h = hz(h, v->busy[i] > 100 ? 100 : v->busy[i]);
    return h;
}
static uint32_t g_m5_mem(const mind_view_t* v) { return hz(hz(HZ_SEED, v->mem_used), v->runnable); }
static uint32_t g_m5_screen(const mind_view_t* v) { return hz(hz(hz(HZ_SEED, v->frames_per_s), v->late_per_s), v->comp_par != 0); }
static uint32_t g_m5_policy(const mind_view_t* v) {
    uint32_t h = HZ_SEED;
    h = hz(h, v->core_cap); h = hz(h, v->comp_ns_kpx_par); h = hz(h, v->comp_ns_kpx_ser);
    h = hz(h, v->dev_errors); h = hs(h, v->dev_name);
    h = hz(h, v->hid_suspect); h = hs(h, v->hid_name); h = hz(h, v->hid_rate);
    h = hz(h, v->denied); h = hs(h, v->denied_name);
    return h;
}

typedef struct { int x, y, w, h; uint32_t (*sig)(const mind_view_t*); void (*draw)(surface_t*, const mind_view_t*); int inner; } mcard_t;   /* inner: часть другой карточки, стирается её цветом */
#define MC_STATUS(sg, dr) { 0, -TH_BAR_H, 0, TH_BAR_H, sg, dr, 0 }            /* полоса состояния: прижата к низу поверхности страницы, здоровье в её подписи у всех разделов */
#define SR(n) { 224, 38 + (n) * 36 - 1, 362, 34, g_m0_sig##n, m0_sig##n, 1 }
static const mcard_t mc_p0[] = {
    { 10, 10, 200, 320, g_m0_health, m0_health, 0 }, { 220, 10, 370, 320, g_const, m0_sigframe, 0 },
    SR(0), SR(1), SR(2), SR(3), SR(4), SR(5), SR(6), SR(7),
    { 10, 340, 580, 40, g_m0_counters, m0_counters, 0 },
    { 10, 390, 580, 140, g_m0_journal, m0_journal, 0 }, MC_STATUS(g_m0_status, m0_status) };
static const mcard_t mc_p1[] = {
    { 10, 10, 580, 145, g_m1_risk, m1_risk, 0 }, { 10, 165, 280, 160, g_m1_history, m1_history, 0 }, { 300, 165, 290, 160, g_m1_quality, m1_quality, 0 },
    { 10, 335, 580, 190, g_const, m1_limits, 0 }, MC_STATUS(g_const, m1_status) };
static const mcard_t mc_p2[] = {
    { 10, 10, 580, 125, g_m2_state, m2_state, 0 }, { 10, 145, 580, 380, g_m2_weights, m2_weights, 0 }, MC_STATUS(g_m2_status, m2_status) };
static const mcard_t mc_p3[] = {
    { 10, 10, 580, 90, g_m3_summary, m3_summary, 0 }, { 10, 110, 580, 415, g_m3_levers, m3_levers, 0 }, MC_STATUS(g_const, m3_status) };
static const mcard_t mc_p4[] = { { 10, 10, 580, 515, g_m4_list, m4_list, 0 }, MC_STATUS(g_const, m4_status) };
static const mcard_t mc_p5[] = {
    { 10, 10, 580, 145, g_m5_cpu, m5_cpu, 0 }, { 10, 165, 280, 145, g_m5_mem, m5_mem, 0 }, { 300, 165, 290, 145, g_m5_screen, m5_screen, 0 },
    { 10, 320, 580, 205, g_m5_policy, m5_policy, 0 }, MC_STATUS(g_const, m5_status) };
#define MC_MAX 16

/* Что нарисовано в поверхности окна. Другая поверхность, размер или раздел - рисунок с нуля. */
static struct { const void* px; int w, h; int pitch; unsigned page; int valid; uint32_t sig[MC_MAX]; uint8_t drawn[MC_MAX]; } mcache;

void app_mind_invalidate(void) { mcache.valid = 0; }

unsigned app_mind_draw_step(surface_t* s, const mind_view_t* v, unsigned page, uint64_t budget_ns, uint64_t (*now_ns)(void)) {
    if (page >= 6) page = 0;
    surface_t sub = { s->px + (size_t)MIND_NAV_H * (size_t)s->pitch, s->w, s->h - MIND_NAV_H, s->pitch };
    static const mcard_t* const pages[6] = { mc_p0, mc_p1, mc_p2, mc_p3, mc_p4, mc_p5 };
    static const unsigned counts[6] = { sizeof mc_p0 / sizeof mc_p0[0], sizeof mc_p1 / sizeof mc_p1[0], sizeof mc_p2 / sizeof mc_p2[0],
                                        sizeof mc_p3 / sizeof mc_p3[0], sizeof mc_p4 / sizeof mc_p4[0], sizeof mc_p5 / sizeof mc_p5[0] };
    const mcard_t* cs = pages[page]; unsigned n = counts[page];
    unsigned res = 0;
    int full = !mcache.valid || mcache.px != s->px || mcache.w != s->w || mcache.h != s->h || mcache.pitch != s->pitch || mcache.page != page;
    if (full) {                                           /* первый рисунок, другой раздел или другая поверхность: всё сразу, без бюджета */
        gfx_fill(&sub, TH_CONTENT);
        mind_tabs_draw(s, page);
        mcache.valid = 1; mcache.px = s->px; mcache.w = s->w; mcache.h = s->h; mcache.pitch = s->pitch; mcache.page = page;
        for (unsigned i = 0; i < MC_MAX; i++) mcache.drawn[i] = 0;
        res |= APP_MIND_DREW;
    }
    uint64_t t0 = now_ns ? now_ns() : 0;
    for (unsigned i = 0; i < n; i++) {
        const mcard_t* c = &cs[i];
        uint32_t sg = c->sig(v);
        if (c->y < 0) sg = hz(sg, g_health(v));
        if (mcache.drawn[i] && mcache.sig[i] == sg) continue;
        if (!full && (res & APP_MIND_DREW) && now_ns && now_ns() - t0 >= budget_ns) { res |= APP_MIND_MORE; continue; }   /* бюджет кадра вышел: остальное - в следующих кадрах */
        if (mcache.drawn[i]) {                            /* прежнее содержимое карточки стирается фоном страницы, как на пустой странице */
            int y = c->y < 0 ? sub.h + c->y : c->y, w = c->w ? c->w : sub.w;
            gfx_rect(&sub, c->x, y, w, c->h, c->inner ? TH_CARD : TH_CONTENT);
        }
        c->draw(&sub, v);
        mcache.sig[i] = sg; mcache.drawn[i] = 1;
        res |= APP_MIND_DREW;
    }
    return res;
}

void app_mind_draw(surface_t* s, const mind_view_t* v, unsigned page) {
    app_mind_invalidate();
    (void)app_mind_draw_step(s, v, page, 0, 0);
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

/* ---------------- Проводник ---------------- */
/* Сверху вниз: панель инструментов; слева боковая панель (места, ветка, устройства); две панели со строкой пути
   (кликабельные части), шапкой колонок (сортировка щелчком) и списком; внизу строка состояния. Геометрия общая для
   рисования и проверки попадания мыши. Стиль и обоснование - docs/UI_DESIGN.md. */

#define TB_Y     6
#define TB_H     TH_BAR_H
#define SIDE_X   TH_S2
#define SIDE_W   148
#define PY       36
#define STAT_Y   (APP_FILES_H - TH_BAR_H)
#define PH       (STAT_Y - TH_S2 - PY)
#define PX0      (SIDE_X + SIDE_W + TH_S2)
#define PW       ((APP_FILES_W - PX0 - TH_S2 - TH_S2) / 2)
#define PX(p)    (PX0 + (p) * (PW + TH_S2))
#define CRUMB_H  24                      /* строка пути */
#define HEAD_H   22                      /* шапка колонок */
#define ROW0     (CRUMB_H + HEAD_H)      /* первая строка списка от верха панели */
#define ROW_H    TH_ROW_H
#define COL_T    68                      /* ширина колонки «Тип» */
#define COL_S    76                      /* ширина колонки «Размер» */
#define SB_W     8                       /* дорожка полосы прокрутки */

typedef struct { const char* label; int w; int cmd; } tool_t;   /* cmd 0 - разделитель */
static const tool_t tools[] = {
    { "\xE2\x86\x90", 28, FM_CMD_BACK }, { "\xE2\x86\x92", 28, FM_CMD_FWD }, { "\xE2\x86\x91", 28, FM_CMD_UP }, { 0, 0, 0 },
    { "Обновить", 80, FM_CMD_REFRESH }, { "Новая папка", 104, FM_CMD_MKDIR }, { 0, 0, 0 },
    { "Копировать", 96, FM_CMD_COPY }, { "Вырезать", 80, FM_CMD_CUT }, { "Вставить", 80, FM_CMD_PASTE }, { "Удалить", 72, FM_CMD_DELETE }, { 0, 0, 0 },
    { "Служебные", 88, FM_CMD_HIDDEN },
};
#define NTOOLS ((int)(sizeof tools / sizeof tools[0]))

static int tool_x(int i) {
    int x = TH_S2;
    for (int k = 0; k < i; k++) x += tools[k].label ? tools[k].w + TH_S1 : TH_S2;
    return x;
}

static int tool_state(const exp_view_t* e, int cmd) {          /* UI_ST_DISABLED, если команда сейчас бессмысленна */
    const fm_panel_t* P = &e->fm.p[e->fm.active];
    int real = P->n && P->cur < (int)P->n && P->e[P->cur].kind != FM_KIND_UP;
    switch (cmd) {
    case FM_CMD_BACK: return P->nback ? UI_ST_NORMAL : UI_ST_DISABLED;
    case FM_CMD_FWD: return P->nfwd ? UI_ST_NORMAL : UI_ST_DISABLED;
    case FM_CMD_UP: return P->path[0] == '/' && !P->path[1] ? UI_ST_DISABLED : UI_ST_NORMAL;
    case FM_CMD_COPY: case FM_CMD_CUT: case FM_CMD_DELETE: return real ? UI_ST_NORMAL : UI_ST_DISABLED;
    case FM_CMD_PASTE: return e->fm.nclip ? UI_ST_NORMAL : UI_ST_DISABLED;
    case FM_CMD_HIDDEN: return e->fm.show_hidden ? UI_ST_SELECTED : UI_ST_NORMAL;
    default: return UI_ST_NORMAL;
    }
}

/* ---- боковая панель ---- */
enum { SI_HEAD, SI_PLACE, SI_TREE, SI_DEV };
typedef struct { char label[24]; char path[FM_PATH]; int level, kind; } side_item_t;
#define SIDE_MAX 40

static void side_add(side_item_t* it, int* n, const char* label, const char* path, int level, int kind) {
    if (*n >= SIDE_MAX) return;
    side_item_t* x = &it[(*n)++];
    int p = 0; while (label[p] && p < 23) { x->label[p] = label[p]; p++; } x->label[p] = 0;
    p = 0; while (path[p] && p < FM_PATH - 1) { x->path[p] = path[p]; p++; } x->path[p] = 0;
    x->level = level; x->kind = kind;
}

static int side_build(const exp_view_t* e, side_item_t* it) {
    int n = 0;
    const fm_panel_t* P = &e->fm.p[e->fm.active];
    side_add(it, &n, "Места", "", 0, SI_HEAD);
    side_add(it, &n, "Корень", "/", 0, SI_PLACE);
    side_add(it, &n, "Данные", "/data", 0, SI_PLACE);
    side_add(it, &n, "Программы", "/bin", 0, SI_PLACE);
    side_add(it, &n, "Дерево", "", 0, SI_HEAD);
    int depth = fm_path_depth(P->path);                          /* путь от корня: каждый уровень на шаг правее */
    for (int k = 0; k <= depth && n < SIDE_MAX - 8; k++) {
        char pre[FM_PATH]; if (fm_path_prefix(P->path, k, pre, sizeof pre)) break;
        const char* base = pre; for (const char* q = pre; *q; q++) if (*q == '/' && q[1]) base = q + 1;
        side_add(it, &n, k == 0 ? "/" : base, pre, k > 5 ? 5 : k, SI_TREE);
    }
    int kids = 0;                                                 /* подпапки текущего каталога - из уже прочитанного списка */
    for (uint32_t i = 0; i < P->n && kids < 8 && n < SIDE_MAX - 6; i++) {
        const fm_ent_t* r = &P->e[i];
        if (!r->dir || r->kind == FM_KIND_UP) continue;
        char full[FM_PATH]; int q = cat(full, 0, P->path); if (q > 1) q = cat(full, q, "/"); cat(full, q, r->name);
        side_add(it, &n, r->name, full, (depth + 1 > 5 ? 5 : depth + 1), SI_TREE);
        kids++;
    }
    if (e->nvol) {
        side_add(it, &n, "Устройства", "", 0, SI_HEAD);
        for (uint32_t k = 0; k < e->nvol && k < EXP_VOLS; k++) {
            char full[16]; int q = cat(full, 0, "/"); cat(full, q, e->vol[k].name);
            side_add(it, &n, e->vol[k].label[0] ? e->vol[k].label : e->vol[k].name, full, 0, SI_DEV);
        }
    }
    return n;
}

int app_files_side_path(const exp_view_t* e, int row, char* out, int cap) {
    side_item_t it[SIDE_MAX]; int n = side_build(e, it);
    if (row < 0 || row >= n || it[row].kind == SI_HEAD || !it[row].path[0]) return -1;
    int p = 0; while (it[row].path[p] && p + 1 < cap) { out[p] = it[row].path[p]; p++; } out[p] = 0;
    return 0;
}

static int same_path(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
#define SIDE_ROW_Y(i) (PY + TH_S1 + (i) * TH_ROW_H)

/* ---- строка пути: части между «/» ---- */
static int crumb_layout(const char* path, int maxw, int* xs, int* ws, int cap) {
    /* части: «/», затем каждый каталог; ширина части = текст + отступ; если не помещаются - левые отбрасываются */
    int depth = fm_path_depth(path), n = depth + 1, w[40], tot = 0, first = 0;
    if (n > 40) return 0;
    for (int k = 0; k < n; k++) {
        char pre[FM_PATH]; fm_path_prefix(path, k, pre, sizeof pre);
        const char* base = pre; for (const char* q = pre; *q; q++) if (*q == '/' && q[1]) base = q + 1;
        w[k] = (k == 0 ? 8 : gfx_text_width(base)) + 2 * TH_S1 + (k >= 2 ? 8 : 0);   /* между частями «/» */
        tot += w[k];
    }
    while (first < n - 1 && tot > maxw) { tot -= w[first]; first++; }
    int x = 0, m = 0;
    for (int k = first; k < n && m < cap; k++, m++) { xs[m] = x; ws[m] = w[k]; x += w[k]; }
    return m ? (first << 8) | m : 0;                              /* первая видимая часть и их число */
}

int app_files_crumb_path(const exp_view_t* e, int panel, int k, char* out, int cap) {
    return fm_path_prefix(e->fm.p[panel].path, k, out, cap);
}

int app_files_hit(const exp_view_t* e, int x, int y, app_hit_t* h) {
    h->kind = APP_HIT_NONE; h->panel = 0; h->row = 0;
    if (e->fm.mode == FM_MODE_MENU) {                            /* меню перехватывает мышь: пункт или «мимо» */
        int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
        int n = fm_menu_build(&e->fm, id, lb, &off);
        int mx, my; app_files_menu_pos(e, &mx, &my);
        int i = ui_menu_hit(mx, my, APP_MENU_W, lb, n, x, y);
        if (i >= 0 && id[i] != FM_CMD_NONE && !((off >> i) & 1)) { h->kind = APP_HIT_MENU; h->row = i; }
        return h->kind;
    }
    if (e->fm.mode != FM_MODE_NORMAL) return h->kind;
    if (y >= TB_Y && y < TB_Y + TB_H) {
        for (int i = 0; i < NTOOLS; i++) {
            if (!tools[i].label) continue;
            int tx = tool_x(i);
            if (x >= tx && x < tx + tools[i].w) { if (tool_state(e, tools[i].cmd) != UI_ST_DISABLED) { h->kind = APP_HIT_TOOL; h->row = tools[i].cmd; } return h->kind; }
        }
        return h->kind;
    }
    if (x >= SIDE_X && x < SIDE_X + SIDE_W && y >= PY && y < PY + PH) {
        side_item_t it[SIDE_MAX]; int n = side_build(e, it);
        int i = (y - PY - TH_S1) / TH_ROW_H;
        if (y >= PY + TH_S1 && i >= 0 && i < n && it[i].kind != SI_HEAD) { h->kind = APP_HIT_SIDE; h->row = i; }
        return h->kind;
    }
    for (int p = 0; p < 2; p++) {
        int px = PX(p);
        if (x < px || x >= px + PW || y < PY || y >= PY + PH) continue;
        const fm_panel_t* P = &e->fm.p[p];
        h->panel = p; h->kind = APP_HIT_PANEL;
        if (y < PY + CRUMB_H) {                                  /* строка пути: щелчок по части */
            int xs[40], ws[40]; int lay = crumb_layout(P->path, PW - 2 * TH_S2, xs, ws, 40);
            int first = lay >> 8, m = lay & 255;
            for (int j = 0; j < m; j++) if (x >= px + TH_S2 + xs[j] && x < px + TH_S2 + xs[j] + ws[j]) { h->kind = APP_HIT_CRUMB; h->row = first + j; }
            return h->kind;
        }
        if (y < PY + ROW0) {                                     /* шапка колонок */
            int tx = px + PW - SB_W - COL_T, sx = tx - COL_S;
            h->kind = APP_HIT_HEAD; h->row = x >= tx ? FM_SORT_TYPE : x >= sx ? FM_SORT_SIZE : FM_SORT_NAME;
            return h->kind;
        }
        if (x >= px + PW - SB_W && (int)P->n > FM_ROWS) {        /* полоса прокрутки */
            int th = FM_ROWS * ROW_H * FM_ROWS / (int)P->n; if (th < 16) th = 16;
            int ty = PY + ROW0 + (FM_ROWS * ROW_H - th) * P->top / ((int)P->n - FM_ROWS);
            h->kind = APP_HIT_SCROLL; h->row = y < ty ? -1 : y >= ty + th ? 1 : 0;
            return h->kind;
        }
        int i = P->top + (y - PY - ROW0) / ROW_H;
        if ((y - PY - ROW0) / ROW_H < FM_ROWS && i < (int)P->n) { h->kind = APP_HIT_ROW; h->row = i; }
        return h->kind;
    }
    return h->kind;
}

/* Где рисовать меню: у места щелчка, но целиком в окне; открытое с клавиатуры - под строкой курсора активной панели. */
void app_files_menu_pos(const exp_view_t* e, int* x, int* y) {
    int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
    int n = fm_menu_build(&e->fm, id, lb, &off), mh = ui_menu_height(lb, n);
    const fm_panel_t* P = &e->fm.p[e->fm.active];
    int mx = e->fm.menu_x, my = e->fm.menu_y;
    if (mx < 0) { mx = PX(e->fm.active) + 40; my = PY + ROW0 + (P->cur - P->top + 1) * ROW_H; }
    if (mx + APP_MENU_W > APP_FILES_W - TH_S1) mx = APP_FILES_W - TH_S1 - APP_MENU_W;
    if (my + mh > APP_FILES_H - TH_BAR_H) my = APP_FILES_H - TH_BAR_H - mh;
    if (mx < TH_S1) mx = TH_S1;
    if (my < TH_S1) my = TH_S1;
    *x = mx; *y = my;
}

static int hit_eq(const app_hit_t* a, int kind, int panel, int row) { return a && a->kind == kind && a->panel == panel && a->row == row; }

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

/* Значки монохромные (единый набор); цвет значка - цвет текста. Программа /bin сохраняет свой значок, но без цвета. */
static void kind_icon(int k, int vol, const char* name, int* icon, uint32_t* col) {
    (void)vol;
    *icon = ICON_FILE; *col = TH_FG_DIM;
    switch (k) {
    case FM_KIND_DIR: *icon = ICON_FOLDER; *col = TH_FG; break;
    case FM_KIND_UP: *icon = ICON_FOLDER; *col = TH_FG_FAINT; break;
    case FM_KIND_PROG:
        *icon = ICON_TERMINAL;
        if (name && app_info(name)) *icon = dock_icon(name);
        break;
    case FM_KIND_IMG: *icon = ICON_BRUSH; break;
    case FM_KIND_WASM: *icon = ICON_CUBE; break;
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
    gfx_rect(s, px, PY, PW, PH, TH_SURF_CONTENT);
    /* строка пути: части кликабельны, последняя (текущий каталог) выделена */
    gfx_rect(s, px, PY, PW, CRUMB_H, TH_SURF_HEAD);
    int xs[40], ws[40]; int lay = crumb_layout(P->path, PW - 2 * TH_S2, xs, ws, 40);
    int first = lay >> 8, m = lay & 255, depth = fm_path_depth(P->path);
    for (int j = 0; j < m; j++) {
        int k = first + j, cx = px + TH_S2 + xs[j];
        char pre[FM_PATH]; fm_path_prefix(P->path, k, pre, sizeof pre);
        const char* base = pre; for (const char* q = pre; *q; q++) if (*q == '/' && q[1]) base = q + 1;
        int hov = hit_eq(hover, APP_HIT_CRUMB, p, k);
        if (hov) gfx_rect(s, cx, PY + 2, ws[j] - 2, CRUMB_H - 4, ui_state_bg(UI_ST_HOVER, TH_SURF_HEAD));
        if (k >= 2) gfx_text(s, cx, PY + 4, "/", TH_FG_FAINT, -1);                        /* разделитель частей */
        gfx_text(s, cx + (k >= 2 ? 8 : 0) + TH_S1, PY + 4, k == 0 ? "/" : base, k == depth ? TH_FG : TH_FG_DIM, -1);
    }
    /* шапка колонок */
    int tx = px + PW - SB_W - COL_T, sx = tx - COL_S;
    int sc = P->sort_col, sd = P->sort_desc ? -1 : 1;
    ui_col_header(s, px, PY + CRUMB_H, sx - px, HEAD_H, "Имя", sc == FM_SORT_NAME ? sd : 0, hit_eq(hover, APP_HIT_HEAD, p, FM_SORT_NAME));
    ui_col_header(s, sx, PY + CRUMB_H, COL_S, HEAD_H, "Размер", sc == FM_SORT_SIZE ? sd : 0, hit_eq(hover, APP_HIT_HEAD, p, FM_SORT_SIZE));
    ui_col_header(s, tx, PY + CRUMB_H, COL_T + SB_W, HEAD_H, "Тип", sc == FM_SORT_TYPE ? sd : 0, hit_eq(hover, APP_HIT_HEAD, p, FM_SORT_TYPE));
    if (!P->n) gfx_text(s, px + TH_S2, PY + ROW0 + 4, e->mounted ? "пусто" : "диска нет", TH_FG_FAINT, -1);
    for (int i = 0; i < FM_ROWS && P->top + i < (int)P->n; i++) {
        int idx = P->top + i;
        const fm_ent_t* r = &P->e[idx];
        int y = PY + ROW0 + i * ROW_H;
        int vol = vol_of(e, P, r);
        int cur = idx == P->cur;
        ui_list_row(s, px, y, PW - SB_W, ROW_H, cur || r->mark, act && cur, hit_eq(hover, APP_HIT_ROW, p, idx), i & 1);
        int icon; uint32_t ic; kind_icon(r->kind, vol >= 0, r->name, &icon, &ic);
        icon_draw(s, px + 16, y + ROW_H / 2, 14, icon, ic, 0);
        uint32_t nc = r->kind == FM_KIND_UP ? TH_FG_DIM : cur || r->mark ? TH_FG : r->dir ? TH_FG : TH_FG_DIM;
        int namew = sx - px - 28 - TH_S1;
        const char* shown = r->kind == FM_KIND_PROG ? app_label(r->name) : r->name;   /* /bin: русское название, сортировка и запуск - по системному имени */
        int nl = gfx_text_width(shown); if (nl > namew) nl = namew;
        ui_text_fit(s, px + 28, y + 2, shown, namew, nc);
        if (r->kind == FM_KIND_PROG && shown != r->name) {                   /* системное имя тускло рядом, пока помещается */
            int lw = namew - nl - 8;
            if (lw > 0 && gfx_text_width(r->name) <= lw) gfx_text(s, px + 28 + nl + 8, y + 2, r->name, TH_FG_FAINT, -1);
        }
        if (vol >= 0) {                                                      /* метка флешки после имени */
            int lw = namew - nl - 8;
            if (lw > 24) ui_text_fit(s, px + 28 + nl + 8, y + 2, e->vol[vol].label, lw, TH_FG_DIM);
            fmt_size(t, (uint64_t)e->vol[vol].total_kb * 1024);
        } else if (r->dir) t[0] = 0;
        else fmt_size(t, r->size);
        if (t[0]) gfx_text(s, tx - TH_S2 - gfx_text_width(t), y + 2, t, TH_FG_DIM, -1);
        gfx_text(s, tx + TH_S2, y + 2, kind_name(r->kind, vol >= 0), TH_FG_FAINT, -1);
        if (r->mark) gfx_text(s, px + PW - SB_W - 12, y + 2, "\xE2\x97\x8F", TH_ACCENT, -1);     /* ● отмечено */
    }
    ui_scrollbar(s, px + PW - SB_W + 1, PY + ROW0, FM_ROWS * ROW_H, (int)P->n, FM_ROWS, P->top, act);
    gfx_rect(s, px, PY, PW, 1, TH_EDGE); gfx_rect(s, px, PY + PH - 1, PW, 1, TH_EDGE);
    gfx_rect(s, px, PY, 1, PH, TH_EDGE); gfx_rect(s, px + PW - 1, PY, 1, PH, TH_EDGE);
    if (act) ui_focus_ring(s, px, PY, PW, PH);                              /* у активной панели клавиатурный фокус виден рамкой */
    if (drop_panel == p) ui_focus_ring(s, px + 2, PY + 2, PW - 4, PH - 4);
}

static void draw_side(surface_t* s, const exp_view_t* e, const app_hit_t* hover) {
    side_item_t it[SIDE_MAX]; int n = side_build(e, it);
    const fm_panel_t* P = &e->fm.p[e->fm.active];
    gfx_rect(s, SIDE_X, PY, SIDE_W, PH, TH_SURF_BAR);
    gfx_rect(s, SIDE_X + SIDE_W - 1, PY, 1, PH, TH_EDGE);
    for (int i = 0; i < n; i++) {
        int y = SIDE_ROW_Y(i);
        if (y + TH_ROW_H > PY + PH) break;
        if (it[i].kind == SI_HEAD) { gfx_text(s, SIDE_X + TH_S2, y + 2, it[i].label, TH_FG_FAINT, -1); continue; }
        int cur = it[i].path[0] && same_path(it[i].path, P->path);
        int hov = hit_eq(hover, APP_HIT_SIDE, 0, i);
        if (cur) { gfx_rect(s, SIDE_X, y, SIDE_W - 1, TH_ROW_H, ui_state_bg(UI_ST_SELECTED, TH_SURF_BAR)); gfx_rect(s, SIDE_X, y, 3, TH_ROW_H, TH_ACCENT); }
        else if (hov) gfx_rect(s, SIDE_X, y, SIDE_W - 1, TH_ROW_H, ui_state_bg(UI_ST_HOVER, TH_SURF_BAR));
        int ind = TH_S2 + (it[i].kind == SI_TREE ? it[i].level * 6 : 0);
        icon_draw(s, SIDE_X + ind + 7, y + TH_ROW_H / 2, 12, it[i].kind == SI_DEV ? ICON_CHIP : ICON_FOLDER, cur ? TH_FG : TH_FG_DIM, 0);
        ui_text_fit(s, SIDE_X + ind + 18, y + 2, it[i].label, SIDE_W - ind - 22, cur ? TH_FG : TH_FG_DIM);
    }
}

static void draw_dialog(surface_t* s, const fm_t* f);

static void draw_menu(surface_t* s, const exp_view_t* e) {
    int id[FM_MENU_MAX]; const char* lb[FM_MENU_MAX]; unsigned off;
    int n = fm_menu_build(&e->fm, id, lb, &off);
    int mx, my; app_files_menu_pos(e, &mx, &my);
    ui_menu(s, mx, my, APP_MENU_W, lb, n, e->fm.menu_sel, off);
}

static void draw_help(surface_t* s) {
    static const char* const ln[] = {
        "Стрелки, PgUp/PgDn, Home/End - выбор строки", "Enter - открыть, Backspace или Alt+↑ - вверх",
        "Alt+← / Alt+→ - назад / вперёд по истории", "Tab - другая панель, пробел или Insert - отметить",
        "Ctrl+A - выделить всё, Esc - снять отметки", "Ctrl+C / Ctrl+X / Ctrl+V - копировать, вырезать, вставить",
        "F2 - переименовать, F7 - новая папка", "Del или F8 - удалить (с подтверждением)",
        "F5 / F6 - копия / перенос в другую панель", "F3 - просмотр, F4 - закрепить программу /bin",
        "F9 - служебные файлы, Shift+F10 - меню",
        "Мышь: щелчок - выбор, двойной - открыть,", "правая кнопка - меню, щелчок по шапке - сортировка,",
        "перетащить на другую панель - копия",
    };
    int n = (int)(sizeof ln / sizeof ln[0]);
    int w = 440, h = 56 + n * 18, x = (APP_FILES_W - w) / 2, y = 60;
    ui_dialog_frame(s, x, y, w, h, "Клавиши Проводника", 0);
    for (int i = 0; i < n; i++) gfx_text(s, x + 18, y + 42 + i * 18, ln[i], TH_FG_DIM, -1);
}

static void draw_dialog(surface_t* s, const fm_t* f) {
    int x, y, w = 440;
    if (f->mode == FM_MODE_HELP) { draw_help(s); return; }
    x = (APP_FILES_W - w) / 2; y = 130;
    if (f->mode == FM_MODE_INPUT) {
        ui_dialog_frame(s, x, y, w, 124, f->dlg_title, 0);
        gfx_text(s, x + 18, y + 40, f->dlg_text, TH_FG_DIM, -1);
        ui_input(s, x + 16, y + 62, w - 32, 28, f->input, 1);
        gfx_text(s, x + 18, y + 98, "Enter - принять · Esc - отмена", TH_FG_FAINT, -1);
    } else if (f->mode == FM_MODE_CONFIRM) {
        ui_dialog_frame(s, x, y, w, 112, f->dlg_title, 1);                         /* опасное действие: красная кромка */
        text_wrap(s, x + 18, y + 40, f->dlg_text, 404, 2, 18, TH_FG);
        ui_button(s, x + 18, y + 72, 176, 26, "Enter - да, удалить", TH_ALARM, 0, 1);
        ui_button(s, x + 204, y + 72, 120, 26, "Esc - нет", TH_ACCENT, 0, 0);
    } else if (f->mode == FM_MODE_BUSY) {
        ui_dialog_frame(s, x, y, w, 128, f->dlg_title, 0);
        char t[128]; int p = cat(t, 0, f->prog_name); (void)p;
        ui_text_fit(s, x + 18, y + 40, t, 404, TH_FG);
        int pct = -1;
        if (f->prog_total) pct = (int)(f->prog_done * 100 / f->prog_total);
        else if (f->prog_files_total) pct = (int)((uint64_t)f->prog_files * 100 / f->prog_files_total);
        gfx_rect(s, x + 18, y + 64, 404, 14, TH_SURF_TRACK);
        if (pct >= 0) gfx_rect(s, x + 18, y + 64, 404 * (pct > 100 ? 100 : pct) / 100, 14, TH_ACCENT);
        char a[24], b[24];
        p = 0;
        if (f->prog_total) { fmt_size(a, f->prog_done); fmt_size(b, f->prog_total); p = cat(t, p, a); p = cat(t, p, " из "); p = cat(t, p, b); p = cat(t, p, " · "); }
        p = cat(t, p, "объектов "); p = cat_u(t, p, f->prog_files);
        if (f->prog_files_total) { p = cat(t, p, " из "); p = cat_u(t, p, f->prog_files_total); }
        if (pct >= 0) { p = cat(t, p, " · "); p = cat_u(t, p, (uint64_t)(pct > 100 ? 100 : pct)); cat(t, p, "%"); }
        gfx_text(s, x + 18, y + 86, t, TH_FG_DIM, -1);
        gfx_text(s, x + 18, y + 104, "Esc - прервать", TH_FG_FAINT, -1);
    }
}

void app_files_draw(surface_t* s, const exp_view_t* e, const app_hit_t* hover, int drop_panel, const char* status, int pinned) {
    (void)pinned;
    gfx_fill(s, TH_SURF_CONTENT);
    int p;
    /* панель инструментов */
    gfx_rect(s, 0, 0, s->w, PY - 2, TH_SURF_HEAD); gfx_rect(s, 0, PY - 3, s->w, 1, TH_EDGE);
    for (int i = 0; i < NTOOLS; i++) {
        int tx = tool_x(i);
        if (!tools[i].label) { gfx_rect(s, tx + 3, TB_Y + 3, 1, TB_H - 6, TH_EDGE); continue; }
        int st = tool_state(e, tools[i].cmd);
        if (st == UI_ST_NORMAL && hover && hover->kind == APP_HIT_TOOL && hover->row == tools[i].cmd) st = UI_ST_HOVER;
        ui_toolbtn(s, tx, TB_Y, tools[i].w, TB_H, tools[i].label, st, 0);
    }
    draw_side(s, e, hover);
    draw_panel(s, e, 0, hover, drop_panel);
    draw_panel(s, e, 1, hover, drop_panel);

    /* строка состояния: итог операции или сведения о строке; справа - число элементов, выделено, свободно */
    const fm_t* f = &e->fm;
    const fm_panel_t* P = &f->p[f->active];
    const char* left = ""; uint32_t lc = TH_FG_DIM; char info[256];
    if (f->msg[0]) { left = f->msg; lc = f->msg_err ? TH_ALARM : TH_OK; }
    else if (status && status[0]) { left = status; lc = TH_OK; }
    else if (!e->mounted) { left = "Диск с KiraFS не подключён: проводнику нечего показать"; lc = TH_WARN; }
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
    char right[160];
    uint32_t cnt = P->n > 0 && P->e[0].kind == FM_KIND_UP ? P->n - 1 : P->n;
    p = cat_u(right, 0, cnt); p = cat(right, p, cnt % 10 == 1 && cnt % 100 != 11 ? " элемент" : cnt % 10 >= 2 && cnt % 10 <= 4 && (cnt % 100 < 10 || cnt % 100 >= 20) ? " элемента" : " элементов");
    if (P->trunc) p = cat(right, p, "+");
    if (P->nmark) { char z[24]; fmt_size(z, P->mark_bytes); p = cat(right, p, " · выделено "); p = cat_u(right, p, P->nmark); p = cat(right, p, " ("); p = cat(right, p, z); p = cat(right, p, ")"); }
    if (!f->show_hidden && P->hidden) { p = cat(right, p, " · скрыто "); p = cat_u(right, p, P->hidden); }
    if (e->mounted) { char z[24]; app_fmt_kb(z, e->free_kb); p = cat(right, p, " · свободно "); cat(right, p, z); }
    ui_statusbar(s, STAT_Y, left, lc, right);
    if (f->mode == FM_MODE_MENU) draw_menu(s, e);
    else if (f->mode != FM_MODE_NORMAL) draw_dialog(s, f);
}

/* ---------------- Настройки ---------------- */

typedef struct { int row; const char* label; int code; } seg_t;
static const seg_t segs[] = {
    { 0, "наблюдает", 10 }, { 0, "действует", 11 },
    { 1, "авто", 24 }, { 1, "0", 20 }, { 1, "1", 21 }, { 1, "2", 22 }, { 1, "3", 23 },
    { 2, "UTC", 32 }, { 2, "местн", 33 }, { 2, "-", 30 }, { 2, 0, -1 }, { 2, "+", 31 },
    { 3, "выкл", 40 }, { 3, "вкл", 41 },
    { 4, "выкл", 50 }, { 4, "важные", 51 }, { 4, "все", 52 },
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
    if (code >= 50 && code <= 52) return st->notify == code - 50;
    return 0;
}

void app_settings_draw(surface_t* s, const app_settings_t* st, int hover) {
    gfx_fill(s, TH_CONTENT);
    static const char* const title[5] = { "Разум", "Анимация фона", "Часовой пояс", "Телеметрия", "Уведомления Разума" };
    static const char* const hint[5] = { "наблюдать или управлять системой", "авто - решает Разум",
                                         "часы ПК: UTC / местное", "замеры Разума в журнал раз в секунду",
                                         "важные - угрозы и остановки; всё остаётся в журнале" };
    static const uint32_t acc[5] = { TH_PURPLE, TH_CYAN, TH_GREEN, TH_AMBER, TH_BLUE };
    for (int r = 0; r < 5; r++) {
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
    int y = SET_ROW_Y(5);
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

