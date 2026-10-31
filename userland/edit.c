/* Текстовый редактор (программа Ring 3 / EL0). Файл - в песочнице программы (/data/edit/<имя>), текст в UTF-8
   (кириллица из файлов показывается, набор - латиница, цифры, знаки). Клавиши: стрелки, Home/End, PgUp/PgDn,
   Backspace, Delete, Enter, Tab; F2 или Ctrl+S - сохранить; F3 - следующий файл песочницы; F4 - новый файл;
   мышь - курсор в точку. Файл до 32 КБ. */
#include "kira.h"
#include "../font.h"
KIRA_ENTRY()

#define W 640
#define H 400
#define CW FONT_W
#define CH FONT_H
#define TOP 20                       /* строка состояния */
#define COLS (W / CW)
#define ROWS ((H - TOP) / CH)
#define MAX 32768

enum { KEY_ESC = 0x100, KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT,
       KEY_RALT, KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11,
       KEY_F12, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE };
#define MOD_CTRL 2

static char text[MAX];
static u32 len, cur, top_line;               /* длина, курсор (байт), первая видимая строка */
static char name[32] = "note.txt";
static char status[96];
static int changed;
static u32* px;

static u32 slen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scpy(char* d, const char* s, u32 cap) { u32 i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static void scat(char* d, const char* s, u32 cap) { scpy(d + slen(d), s, cap - slen(d)); }
static void sdec(char* d, u64 v, u32 cap) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); scat(d, b + i, cap); }

/* UTF-8: длина символа по первому байту и его код. */
static u32 utf8(const char* s, u32 n, u32* cp) {
    u8 c = (u8)s[0];
    if (c < 0x80 || n < 2) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { *cp = (u32)(c & 0x1F) << 6 | ((u8)s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && n >= 3) { *cp = (u32)(c & 0x0F) << 12 | (u32)((u8)s[1] & 0x3F) << 6 | ((u8)s[2] & 0x3F); return 3; }
    *cp = '?'; return 1;
}
static u32 char_len_at(u32 i) { u32 cp; return i < len ? utf8(text + i, len - i, &cp) : 0; }
static u32 prev_char(u32 i) { if (!i) return 0; i--; while (i && ((u8)text[i] & 0xC0) == 0x80) i--; return i; }

static u32 line_start(u32 i) { while (i && text[i - 1] != '\n') i--; return i; }
static u32 line_end(u32 i) { while (i < len && text[i] != '\n') i++; return i; }
static u32 col_of(u32 i) { u32 c = 0; for (u32 p = line_start(i); p < i; p += char_len_at(p)) c++; return c; }
static u32 at_col(u32 ls, u32 col) { u32 p = ls, e = line_end(ls); while (col-- && p < e) p += char_len_at(p); return p; }
static u32 line_no(u32 i) { u32 n = 0; for (u32 p = 0; p < i; p++) if (text[p] == '\n') n++; return n; }
static u32 line_pos(u32 n) { u32 p = 0; while (n && p < len) { if (text[p++] == '\n') n--; } return p; }

static void glyph(int x, int y, u32 cp, u32 fg, u32 bg) {
    const u8* g = font_glyph(cp);
    for (int r = 0; r < CH; r++) {
        u32* row = px + (y + r) * W + x;
        for (int c = 0; c < CW; c++) row[c] = (g[r] & (0x80 >> c)) ? fg : bg;
    }
}
static void text_at(int x, int y, const char* s, u32 fg, u32 bg) {
    u32 n = slen(s);
    for (u32 i = 0; i < n && x + CW <= W; ) { u32 cp; i += utf8(s + i, n - i, &cp); glyph(x, y, cp, fg, bg); x += CW; }
}

static void draw(void) {
    for (int i = 0; i < W * TOP; i++) px[i] = 0x00203040;
    char st[128]; st[0] = 0;
    scat(st, name, sizeof st); scat(st, changed ? " *  " : "    ", sizeof st);
    scat(st, "строка ", sizeof st); sdec(st, line_no(cur) + 1, sizeof st); scat(st, ", столбец ", sizeof st); sdec(st, col_of(cur) + 1, sizeof st);
    scat(st, "   ", sizeof st); scat(st, status, sizeof st);
    text_at(4, 2, st, 0x00E0E8F0, 0x00203040);
    u32 cl = line_no(cur);
    if (cl < top_line) top_line = cl;
    if (cl >= top_line + ROWS) top_line = cl - ROWS + 1;
    u32 p = line_pos(top_line);
    int drawn = 0;                                                  /* курсор рисуется один раз */
    for (int r = 0; r < ROWS; r++) {
        int y = TOP + r * CH;
        int x = 0;
        while (p < len && text[p] != '\n' && x + CW <= W) {
            u32 cp, k = utf8(text + p, len - p, &cp);
            if (cp == '\t') cp = ' ';
            int on = p == cur && !drawn;
            if (on) drawn = 1;
            glyph(x, y, cp < 32 ? '?' : cp, on ? 0x00101820 : 0x00D0D8E0, on ? 0x00F0C040 : 0x00101820);
            x += CW; p += k;
        }
        int cur_here = !drawn && p == cur && (p >= len || text[p] == '\n');
        if (cur_here) drawn = 1;
        for (; x < W; x += CW) {
            glyph(x, y, ' ', 0, cur_here ? 0x00F0C040 : 0x00101820);
            cur_here = 0;
        }
        while (p < len && text[p] != '\n') p++;                     /* за правым краем - не видно */
        if (p < len) p++;
    }
    k_gfx_present();
}

static void insert(const char* s, u32 n) {
    if (len + n > MAX) { scpy(status, "файл больше 32 КБ", sizeof status); return; }
    for (u32 i = len; i > cur; i--) text[i - 1 + n] = text[i - 1];
    for (u32 i = 0; i < n; i++) text[cur + i] = s[i];
    len += n; cur += n; changed = 1;
}
static void erase(u32 at, u32 n) {
    if (at + n > len) return;
    for (u32 i = at; i + n < len; i++) text[i] = text[i + n];
    len -= n; changed = 1;
}

static void load(void) {
    i64 n = k_file_read(name, text, MAX, 0);
    len = n > 0 ? (u32)n : 0; cur = 0; top_line = 0; changed = 0;
    status[0] = 0;
    scpy(status, n >= 0 ? "открыт" : "новый файл", sizeof status);
}
static void save(void) {
    i64 r = k_file_write(name, text, len);
    status[0] = 0;
    if (r >= 0) { changed = 0; scpy(status, "сохранено: ", sizeof status); sdec(status, len, sizeof status); scat(status, " байт", sizeof status); }
    else { scpy(status, "не сохранено (код ", sizeof status); sdec(status, (u64)-r, sizeof status); scat(status, ")", sizeof status); }
    char m[128]; scpy(m, "edit: ", sizeof m); scat(m, name, sizeof m); scat(m, " - ", sizeof m); scat(m, status, sizeof m); scat(m, "\n", sizeof m);
    k_write(m, slen(m));
}

/* F3: следующий файл песочницы по списку (имена через перевод строки). */
static void next_file(void) {
    static char list[2048];
    i64 n = k_file_list(list, sizeof list - 1);
    if (n <= 0) { scpy(status, "других файлов нет", sizeof status); return; }
    list[n] = 0;
    char* first = 0; char* pick = 0; int seen = 0;
    for (char* p = list; *p && !pick; ) {
        char* e = p; while (*e && *e != '\n') e++;
        char c = *e; *e = 0;
        if (!first) first = p;
        if (seen) pick = p;
        else if (same(p, name)) seen = 1;
        if (!c) break;
        p = e + 1;
    }
    if (!pick) pick = first;
    if (pick && slen(pick) < sizeof name) { scpy(name, pick, sizeof name); load(); }
}

static void new_file(void) {
    for (u32 k = 1; k < 100; k++) {
        char nm[32] = "note"; sdec(nm, k, sizeof nm); scat(nm, ".txt", sizeof nm);
        if (k_file_size(nm) < 0) { scpy(name, nm, sizeof name); len = cur = top_line = 0; changed = 0; scpy(status, "новый файл", sizeof status); return; }
    }
}

int umain(void) {
    px = k_gfx_create(W, H);
    if (!px) { k_write("edit: cannot create window\n", 27); return 1; }
    load();
    draw();
    for (;;) {
        kevent_t ev; int dirty = 0;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) { if (changed) save(); return 0; }
            if (ev.type == KEV_MOUSE_BUTTON && (ev.c & 1) && (int)ev.b >= TOP) {
                u32 row = ((u32)ev.b - TOP) / CH, col = ev.a / CW;
                cur = at_col(line_pos(top_line + row), col); dirty = 1;
                continue;
            }
            if (ev.type != KEV_KEY_DOWN) continue;
            u32 k = ev.a, ch = ev.b; dirty = 1;
            if (k == KEY_F2 || ((ev.c & MOD_CTRL) && (ch == 's' || ch == 19 || k == 's'))) save();
            else if (k == KEY_F3) next_file();
            else if (k == KEY_F4) new_file();
            else if (k == KEY_LEFT) cur = prev_char(cur);
            else if (k == KEY_RIGHT) cur += char_len_at(cur);
            else if (k == KEY_HOME) cur = line_start(cur);
            else if (k == KEY_END) cur = line_end(cur);
            else if (k == KEY_UP || k == KEY_PGUP) {
                u32 col = col_of(cur), n = k == KEY_UP ? 1 : ROWS;
                u32 ls = line_start(cur);
                while (n-- && ls) ls = line_start(ls - 1);
                cur = at_col(ls, col);
            } else if (k == KEY_DOWN || k == KEY_PGDN) {
                u32 col = col_of(cur), n = k == KEY_DOWN ? 1 : ROWS, le = line_end(cur);
                while (n-- && le < len) { cur = le + 1; le = line_end(cur); }
                cur = at_col(line_start(cur), col);
            } else if (k == KEY_BACKSPACE) { if (cur) { u32 p = prev_char(cur); erase(p, cur - p); cur = p; } }
            else if (k == KEY_DELETE) { if (cur < len) erase(cur, char_len_at(cur)); }
            else if (k == KEY_ENTER) insert("\n", 1);
            else if (k == KEY_TAB) insert("    ", 4);
            else if (ch >= 32 && ch < 127 && !(ev.c & MOD_CTRL)) { char c = (char)ch; insert(&c, 1); }
            else if (ch >= 0x80 && ch < 0x800) { char u[2] = { (char)(0xC0 | ch >> 6), (char)(0x80 | (ch & 0x3F)) }; insert(u, 2); }
            else dirty = 0;
        }
        if (dirty) draw();
        k_sleep(2);
    }
}

