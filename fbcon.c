#include "fbcon.h"
#include "klog.h"
#include "font.h"
#include "theme.h"

/* Терминал: журнал ядра с подсветкой.
   - метка в начале строки ([KIRA …], [USER …], [TEST …], [FB] …) - своим цветом, текст после неё - обычным;
   - приглашение «кира>» - зелёным с голубой стрелкой, ввод после него - ярким;
   - слова PASS/OK - зелёным, FAIL/ERROR/PANIC/FATAL - красным, WARN - янтарным.
   Цвет решается по мере прихода символов: метка перекрашивается, когда закрылась скобка, слово - когда кончилось. */

/* Сетка знаков: по умолчанию 240x68 (статическая), а с fbcon_reserve - под весь экран любого размера. */
#define COLS_DEF 240
#define ROWS_DEF 68
#define FG FBCON_FG
#define BG FBCON_BG
#define CHUNK 256
#define MAX_PER_UPDATE 4096

#define C_TAG_KIRA TH_CYAN
#define C_TAG_USER TH_GREEN
#define C_TAG_TEST TH_PURPLE
#define C_TAG_MIND 0x00C084FCu
#define C_TAG_OTHER 0x0060A5FAu
#define C_PROMPT TH_GREEN
#define C_INPUT  TH_TEXT

static surface_t* scr;
static uint32_t cell_def[ROWS_DEF * COLS_DEF], attr_def[ROWS_DEF * COLS_DEF];
static uint32_t* cellp = cell_def;
static uint32_t* attrp = attr_def;
static int COLS_MAX = COLS_DEF, ROWS_MAX = ROWS_DEF;
#define cell(r, c) cellp[(r) * COLS_MAX + (c)]
#define attr(r, c) attrp[(r) * COLS_MAX + (c)]

void fbcon_reserve(int max_w_px, int max_h_px, void* (*alloc)(uint64_t bytes)) {
    int c = max_w_px / FONT_W, r = max_h_px / FONT_H;
    if (c <= COLS_DEF && r <= ROWS_DEF) return;
    if (c < COLS_DEF) c = COLS_DEF;
    if (r < ROWS_DEF) r = ROWS_DEF;
    uint32_t* a = (uint32_t*)alloc((uint64_t)c * r * 4);
    uint32_t* b = (uint32_t*)alloc((uint64_t)c * r * 4);
    if (!a || !b) return;                                   /* нет памяти: остаётся сетка по умолчанию */
    cellp = a; attrp = b; COLS_MAX = c; ROWS_MAX = r;
    for (int i = 0; i < c * r; i++) { cellp[i] = ' '; attrp[i] = FG; }
}
static int cols, rows, cx, cy;
static uint64_t klog_cursor;
static int dy0, dy1;                    // накопленная область изменений
static uint32_t u_cp; static int u_need; // разбор UTF-8 по байтам (последовательность может оборваться на границе порции)

/* разбор строки */
enum { LS_START, LS_TAG, LS_BODY, LS_PROMPT };
static int ls;                         /* состояние строки */
static int ls_n;                       /* символов в строке (логической) */
static uint32_t line_color;            /* цвет обычного текста строки */
static int tag_r, tag_c, tag_len;      /* где началась метка */
static char tag_txt[8]; static int tag_tn;
static int w_r, w_c, w_len;            /* текущее слово из латинских букв */
static char w_txt[8];
static const uint32_t prompt_cp[5] = { 0x043A, 0x0438, 0x0440, 0x0430, '>' };   /* «кира>» */

static void mark(int y0, int y1) {
    if (y0 < dy0) dy0 = y0;
    if (y1 > dy1) dy1 = y1;
}

static void line_reset(void) { ls = LS_START; ls_n = 0; line_color = FG; tag_len = 0; tag_tn = 0; w_len = 0; }

/* Размер поверхности изменился (панель растянули): пересчитать сетку, сохранив текст.
   Если курсор оказался ниже новой нижней строки, содержимое сдвигается вверх. */
void fbcon_resize(void) {
    if (!scr) return;
    cols = scr->w / FONT_W; if (cols > COLS_MAX) cols = COLS_MAX;
    rows = scr->h / FONT_H; if (rows > ROWS_MAX) rows = ROWS_MAX;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cy >= rows) {
        int shift = cy - rows + 1;
        for (int r = 0; r + shift < ROWS_MAX; r++)
            for (int c = 0; c < COLS_MAX; c++) { cell(r, c) = cell(r + shift, c); attr(r, c) = attr(r + shift, c); }
        for (int r = ROWS_MAX - shift; r < ROWS_MAX; r++)
            for (int c = 0; c < COLS_MAX; c++) { cell(r, c) = ' '; attr(r, c) = FG; }
        cy -= shift; tag_r -= shift; w_r -= shift;
    }
    if (cx > cols) cx = cols;
    fbcon_repaint();
}

void fbcon_get_size(int* c, int* r) { *c = cols; *r = rows; }
int fbcon_text_height(void) { return rows * FONT_H; }

static void clear_row_pixels(int row) {
    gfx_rect(scr, 0, row * FONT_H, scr->w, FONT_H, BG);
}

void fbcon_repaint(void) {
    if (!scr) return;
    gfx_fill(scr, BG);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (cell(r, c) != ' ' && cell(r, c) != 0) gfx_glyph(scr, c * FONT_W, r * FONT_H, cell(r, c), attr(r, c), -1);
    mark(0, scr->h);
}

void fbcon_init(surface_t* screen) {
    scr = screen;
    cols = screen->w / FONT_W; if (cols > COLS_MAX) cols = COLS_MAX;
    rows = screen->h / FONT_H; if (rows > ROWS_MAX) rows = ROWS_MAX;
    for (int r = 0; r < ROWS_MAX; r++) for (int c = 0; c < COLS_MAX; c++) { cell(r, c) = ' '; attr(r, c) = FG; }
    cx = cy = 0; klog_cursor = 0; u_need = 0;
    line_reset();
    dy0 = scr->h; dy1 = 0;
    fbcon_repaint();
}

static void scroll_up(void) {
    for (int r = 1; r < rows; r++)
        for (int c = 0; c < COLS_MAX; c++) { cell(r - 1, c) = cell(r, c); attr(r - 1, c) = attr(r, c); }
    for (int c = 0; c < COLS_MAX; c++) { cell(rows - 1, c) = ' '; attr(rows - 1, c) = FG; }
    // пиксели: сдвиг на строку текста одним копированием (pitch чётный: ширина экрана чётная)
    uint64_t qwords = (uint64_t)(rows - 1) * FONT_H * (uint64_t)scr->pitch / 2;
    gfx_copy64(scr->px, scr->px + (long)FONT_H * scr->pitch, qwords);
    clear_row_pixels(rows - 1);
    mark(0, rows * FONT_H);
    tag_r--; w_r--;
}

static void advance_row(void) {
    cx = 0;
    if (++cy >= rows) { cy = rows - 1; scroll_up(); }
}

static void clear_grid(void) {
    for (int r = 0; r < ROWS_MAX; r++) for (int c = 0; c < COLS_MAX; c++) { cell(r, c) = ' '; attr(r, c) = FG; }
    cx = cy = 0;
    line_reset();
    fbcon_repaint();
}

/* Перекрасить n клеток, начиная с (r,c), с переносом по строкам. */
static void recolor(int r, int c, int n, uint32_t color) {
    for (int i = 0; i < n; i++) {
        if (c >= cols) { c = 0; r++; }
        if (r >= 0 && r < rows) {
            attr(r, c) = color;
            gfx_glyph(scr, c * FONT_W, r * FONT_H, cell(r, c), color, BG);
            mark(r * FONT_H, (r + 1) * FONT_H);
        }
        c++;
    }
}

static int prefix(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static uint32_t tag_color(void) {
    tag_txt[tag_tn < 7 ? tag_tn : 7] = 0;
    if (prefix(tag_txt, "KIRA M")) return C_TAG_MIND;              /* [KIRA MIND] - решения Разума */
    if (prefix(tag_txt, "KIRA")) return C_TAG_KIRA;
    if (prefix(tag_txt, "USER")) return C_TAG_USER;
    if (prefix(tag_txt, "TEST")) return C_TAG_TEST;
    return C_TAG_OTHER;
}

static void word_end(void) {
    if (w_len >= 2 && w_len <= 6) {
        w_txt[w_len] = 0;
        uint32_t c = 0;
        if (prefix(w_txt, "PASS") && w_len == 4) c = TH_GREEN;
        else if (w_len == 2 && prefix(w_txt, "OK")) c = TH_GREEN;
        else if ((w_len == 4 && prefix(w_txt, "FAIL")) || (w_len == 6 && prefix(w_txt, "FAILED")) || (w_len == 5 && (prefix(w_txt, "ERROR") || prefix(w_txt, "PANIC") || prefix(w_txt, "FATAL")))) c = TH_RED;
        else if (w_len == 4 && prefix(w_txt, "WARN")) c = TH_AMBER;
        if (c) recolor(w_r, w_c, w_len, c);
    }
    w_len = 0;
}

static void put_glyph(uint32_t cp, uint32_t color) {
    if (cx >= cols) advance_row();
    cell(cy, cx) = cp;
    attr(cy, cx) = color;
    gfx_glyph(scr, cx * FONT_W, cy * FONT_H, cp, color, BG);
    mark(cy * FONT_H, (cy + 1) * FONT_H);
    cx++;
}

static void put_cp(uint32_t cp) {
    if (cp == '\f') { clear_grid(); return; }          // очистка экрана: управляющий код идёт в потоке журнала, порядок вывода сохраняется
    if (cp == '\n') { word_end(); advance_row(); line_reset(); return; }
    if (cp == '\r') { word_end(); cx = 0; line_reset(); return; }
    if (cp == '\t') { word_end(); int n = (cx + 8) & ~7; while (cx < n && cx < cols) { cell(cy, cx) = ' '; attr(cy, cx) = FG; cx++; } ls_n += 8; return; }
    if (cp == '\b') {
        if (cx > 0) {
            cx--; cell(cy, cx) = ' '; attr(cy, cx) = FG;
            gfx_rect(scr, cx * FONT_W, cy * FONT_H, FONT_W, FONT_H, BG); mark(cy * FONT_H, (cy + 1) * FONT_H);
            if (ls_n) ls_n--;
            if (w_len) w_len--;
        }
        return;
    }
    if (cp < 0x20) return;                           // прочие управляющие

    int letter = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    if (!letter) word_end();

    uint32_t color = line_color;
    if (ls == LS_START) {
        if (cp == '[') { ls = LS_TAG; tag_r = cy; tag_c = cx < cols ? cx : 0; if (cx >= cols) tag_r = cy + 1; tag_len = 0; tag_tn = 0; color = C_TAG_OTHER; }
        else if (ls_n < 5 && cp == prompt_cp[ls_n]) {
            color = C_PROMPT;
            if (ls_n == 4) { color = TH_CYAN; ls = LS_PROMPT; line_color = C_INPUT; }
        } else {
            ls = LS_BODY;
            if (ls_n && cx >= ls_n) recolor(cy, cx - ls_n, ls_n, FG);   /* начало было похоже на приглашение - обычная строка */
        }
    } else if (ls == LS_TAG) {
        color = C_TAG_OTHER;
        if (cp == ']') {
            ls = LS_BODY;
            put_glyph(cp, color); tag_len++; ls_n++;
            recolor(tag_r, tag_c, tag_len, tag_color());
            return;
        }
        if (tag_tn < 7 && cp < 0x80) tag_txt[tag_tn++] = (char)cp;
        if (tag_len > 24) ls = LS_BODY;               /* не метка: слишком длинно */
    }
    if (letter) {
        if (w_len == 0) { w_r = cy; w_c = cx; if (cx >= cols) { w_r = cy + 1; w_c = 0; } }
        if (w_len < 7) w_txt[w_len] = (char)(cp >= 'a' ? cp - 32 : cp);
        w_len++;
        if (cp >= 'a') w_len = 99;                    /* выделяем только слова заглавными буквами */
    }
    put_glyph(cp, color);
    if (ls == LS_TAG) tag_len++;
    ls_n++;
}

static void put_byte(uint8_t b) {
    if (u_need) {
        if ((b & 0xC0) == 0x80) {
            u_cp = (u_cp << 6) | (b & 0x3F);
            if (--u_need == 0) put_cp(u_cp);
            return;
        }
        u_need = 0; put_cp(0xFFFD);                   // оборванная последовательность, разбираем байт заново
    }
    if (b < 0x80) put_cp(b);
    else if ((b & 0xE0) == 0xC0) { u_cp = b & 0x1F; u_need = 1; }
    else if ((b & 0xF0) == 0xE0) { u_cp = b & 0x0F; u_need = 2; }
    else if ((b & 0xF8) == 0xF0) { u_cp = b & 0x07; u_need = 3; }
    else put_cp(0xFFFD);
}

unsigned fbcon_update(int* y0, int* y1) {
    if (!scr) return 0;
    char buf[CHUNK];
    unsigned total = 0, lost = 0;
    dy0 = scr->h; dy1 = 0;
    while (total < MAX_PER_UPDATE) {
        unsigned n = klog_read(&klog_cursor, buf, CHUNK, &lost);
        if (!n) break;
        for (unsigned i = 0; i < n; i++) put_byte((uint8_t)buf[i]);
        total += n;
    }
    if (total && dy1 > dy0) { *y0 = dy0; *y1 = dy1; }
    return total;
}
