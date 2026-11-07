#include "display.h"
#include "kpar.h"
#ifndef KIRA_HOST_TEST
#include "scale.h"
#endif
#include "fb.h"
#include "fbcon.h"
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "pmm.h"
#include "uart.h"
#include "wm.h"
#include "input.h"
#include "experience.h"
#include "gfxsrv.h"
#include "proc.h"
#include "progs.h"
#include "platform.h"
#include "virtio_gpu.h"
#include "mind_sys.h"
#include "explorer.h"
#include "apps.h"
#include "icons.h"
#include "theme.h"
#include "rtc.h"
#include "console.h"
#include "dock.h"
#include "vfs.h"
#include "spinlock.h"
#include "cpu.h"

/* Поток дисплея: единственный, кто выводит кадры. Оболочка «ОС Кира»: верхняя панель с разделами, часами и
   индикаторами, боковая панель приложений, окна; приложения ядра (Терминал, Разум, Монитор, Проводник, Настройки)
   рисуются здесь же из готовых снимков, программы Ring 3 (плазма, холст, 3D) - через gfxsrv. */

#define FRAME_NS   16666667ULL       // 60 кадров/с: бюджет цикла; в покое цикл почти ничего не делает
#define TICK_NS    (1000000000ULL / TIMER_HZ)
#define CON_W 680
#define CON_H 432

// Страницы под поверхности окон: непрерывная физическая память (первый ГБ отображён 1:1).
void* gfx_alloc_pages(uint64_t bytes) {
    uint64_t p = pmm_alloc_contiguous((uint32_t)((bytes + 4095) / 4096));
    return (void*)(uintptr_t)p;
}

static int ready;
static int in_flush;
static int w_con = -1, w_mind = -1, w_mon = -1, w_files = -1, w_set = -1;
static volatile int fx_req = 3;

static struct {
    uint64_t frames;
    uint64_t present_ns;
    uint64_t busy_ns;
    uint64_t start_ns;
    uint64_t mouse_events;
    uint64_t composed;        // кадры, в которых что-то перерисовывалось
    uint64_t late;            // циклы, начавшиеся позже чем через 1,5 бюджета кадра
    uint64_t worst_gap_ns;    // самый большой промежуток между началами циклов
    uint64_t worst_busy_ns;   // самый тяжёлый цикл
    uint64_t last_t0;
    uint64_t pixels;          // собрано точек (все полосы всех кадров)
} st;

/* снимки данных для приложений */
static mind_view_t mv;
static exp_view_t ev;
static uint32_t mv_ver = ~0u, ev_ver = ~0u;
static app_hit_t files_hover;
static int set_hover = -1, files_drop = -1;
static unsigned mind_page, mind_btn;
static unsigned files_btn, set_btn;
static char files_status[96];
static uint64_t files_click_ns; static int files_click_panel = -1, files_click_row = -1;
static struct { int on, panel, row, x, y; } drag;       /* файл тащат мышью на другую панель */
static uint32_t act_seen;                                /* последняя обработанная просьба менеджера (открыть, закрепить) */
static volatile int files_focus;                         /* клавиатура у Проводника (читает консоль) */
static spinlock_t rq_lk = SPINLOCK_INIT;                 /* просьба консоли «files» открыть Проводник на путях */
static char rq_l[FM_PATH], rq_r[FM_PATH]; static int rq_pending;

/* ---------------- приложения ядра ---------------- */

/* Приложения ядра открываются разделами верхней панели. Левая панель - быстрый доступ: программы из /bin,
   которые пользователь закрепил сам (Проводник: «закрепить»), ниже черты - запущенные, но не закреплённые
   (их свёрнутые окна иначе было бы не вернуть), внизу «+» - открыть /bin в Проводнике. */
enum { APP_TERM, APP_MIND, APP_FILES, APP_MON, APP_SET, APP_N };
static const char* const app_tag[APP_N] = { "Console", "Mind", "Files", "Monitor", "Settings" };

static const wm_tab_t tabs[] = {
    { "ГЛАВНАЯ", "" }, { "ТЕРМИНАЛ", "Console" }, { "РАЗУМ", "Mind" }, { "ПРОВОДНИК", "Files" }, { "МОНИТОР", "Monitor" },
    { "НАСТРОЙКИ", "Settings" },
};
static const int app_of_tab[] = { -1, APP_TERM, APP_MIND, APP_FILES, APP_MON, APP_SET };

#define SIDE_N WM_SIDE_MAX
static dock_t dock;
static int dock_ready;                                    /* список прочитан с диска (или принят по умолчанию) */
static wm_side_item_t side[SIDE_N];
static char side_names[SIDE_N][16];
static int side_n;
static char side_sig[SIDE_N * 17];                         /* что показано сейчас: пересобирать только при изменении */

static void draw_mind(void) { if (wm_visible(w_mind)) { app_mind_draw(wm_content(w_mind), &mv, mind_page); wm_invalidate(w_mind); } }

static void draw_monitor(void) {
    if (!wm_visible(w_mon)) return;
    static thread_info_t ti[64];
    unsigned n = sched_snapshot(ti, 64);
    app_monitor_draw(wm_content(w_mon), &mv, n);
    wm_invalidate(w_mon);
}

static int cur_prog_pinned(void);

static void draw_files(void) {
    if (!wm_visible(w_files)) return;
    app_files_draw(wm_content(w_files), &ev, &files_hover, files_drop, files_status, cur_prog_pinned());
    wm_invalidate(w_files);
}

static void settings_state(app_settings_t* s) {
    surface_t* scr = fb_surface();
    s->mode = mv.mode; s->fx_user = mv.fx_user; s->fx = mv.fx; s->tz_min = mv.t ? mv.tz_min : mind_sys_tz(); s->rtc_local = mind_sys_rtc_local();
    s->telemetry = mind_sys_telemetry(); s->experience = experience_enabled(); s->irqc = platform_irq_name();
    s->cores = (int)sched_cpu_count(); s->ram_mb = (uint32_t)(pmm_get_total_memory_kb() / 1024);
    s->scr_w = scr->w; s->scr_h = scr->h;
}

static void draw_settings(void) {
    if (!wm_visible(w_set)) return;
    app_settings_t s; settings_state(&s);
    app_settings_draw(wm_content(w_set), &s, set_hover);
    wm_invalidate(w_set);
}

static void redraw_app(int id) {
    if (id == w_mind) draw_mind();
    else if (id == w_mon) draw_monitor();
    else if (id == w_files) draw_files();
    else if (id == w_set) draw_settings();
}

static int open_app(int app) {
    int* slot; const char* title; int w, h;
    switch (app) {
    case APP_MIND:  slot = &w_mind;  title = "Разум";     w = APP_MIND_W;  h = APP_MIND_H;  break;
    case APP_MON:   slot = &w_mon;   title = "Монитор";   w = APP_MON_W;   h = APP_MON_H;   break;
    case APP_FILES: slot = &w_files; title = "Проводник"; w = APP_FILES_W; h = APP_FILES_H; break;
    case APP_SET:   slot = &w_set;   title = "Настройки"; w = APP_SET_W;   h = APP_SET_H;   break;
    case APP_TERM:  slot = &w_con;   title = "Терминал";  w = CON_W;       h = CON_H;       break;
    default: return -1;
    }
    if (*slot < 0) {
        int flags = WM_INPUT;
        if (app != APP_TERM) flags |= WM_ZOOMABLE; /* встроенные панели подстраиваются под маленький экран */
        *slot = wm_create(title, WM_AUTO, WM_AUTO, w, h, flags);
        if (*slot < 0) { uart_print("[KIRA DISPLAY] not enough memory for a window.\n"); return -1; }
        wm_set_tag(*slot, app_tag[app]);
        if (app == APP_MIND) wm_set_subtitle(*slot, "ядро");
        if (app == APP_FILES) { wm_set_subtitle(*slot, "KiraFS"); explorer_refresh(); }
    } else {
        wm_set_visible(*slot, 1);
        wm_raise(*slot);
    }
    redraw_app(*slot);
    return *slot;
}

/* ---------------- события оболочки ---------------- */

static void on_visibility(int id, int visible) {
    if (visible == -1) { gfx_on_close(id); return; }   // красная кнопка на окне приложения
    if (visible == 1) redraw_app(id);                  // свёрнутое окно вернулось: содержимое могло устареть
}

static int scpy(char* d, int p, const char* s, int cap) { while (*s && p < cap - 1) d[p++] = *s++; d[p] = 0; return p; }

/* Запуск программы из Проводника: команда уходит в терминал (основной поток ядра читает диск и запускает),
   поэтому видно эхо и весь вывод. Программа ipc - половина пары: запускается проверка целиком. */
static void files_run(const char* prog) {
    char cmd[64]; int graphic = 0;
    if (!prog) return;
    static const char* const gfx_progs[] = { "plasma", "paint", "cube3d", "drvman" };
    for (unsigned i = 0; i < 4; i++) { const char* a = gfx_progs[i]; const char* b = prog; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) graphic = 1; }
    const char* b = prog; const char* a = "ipc"; while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) scpy(cmd, 0, "test ipc", sizeof cmd);
    else scpy(cmd, 0, prog, sizeof cmd);
    int p;
    if (console_request(cmd) != 0) p = scpy(files_status, 0, "терминал занят прошлыми командами, повторите", sizeof files_status);
    else {
        p = scpy(files_status, 0, "запущено: ", sizeof files_status);
        p = scpy(files_status, p, cmd, sizeof files_status);
        p = scpy(files_status, p, graphic ? " - окно откроется рядом" : " - вывод в Терминале", sizeof files_status);
    }
    (void)p;
}

static void dock_toggle(const char* name);

static const char* prog_name_of(const char* path) {          /* файл прямо в /bin: имя программы, иначе 0 */
    if (path[0] != '/' || path[1] != 'b' || path[2] != 'i' || path[3] != 'n' || path[4] != '/' || !path[5]) return 0;
    for (const char* q = path + 5; *q; q++) if (*q == '/') return 0;
    return path + 5;
}

static int cur_prog_pinned(void) {
    const fm_panel_t* P = &ev.fm.p[ev.fm.active];
    if (!P->n || P->cur >= (int)P->n || P->e[P->cur].kind != FM_KIND_PROG) return 0;
    return dock_find(&dock, P->e[P->cur].name) >= 0;
}

/* Просьба менеджера: Enter/двойной щелчок по файлу (программа из /bin - запуск, иначе просмотр), F3, F4. */
static void files_act(void) {
    const char* path = ev.fm.act_path;
    const char* prog = prog_name_of(path);
    if (ev.fm.act_kind == FM_ACT_PIN) {
        if (prog) { char nm[16]; scpy(nm, 0, prog, 16); dock_toggle(nm); }
        return;
    }
    if (ev.fm.act_kind == FM_ACT_OPEN && prog) { files_run(prog); return; }
    unsigned pl = 0; while (path[pl]) pl++;
    int is_wasm = ev.fm.act_kind == FM_ACT_OPEN && pl > 5 && path[pl - 5] == '.' && path[pl - 4] == 'w' && path[pl - 3] == 'a' && path[pl - 2] == 's' && path[pl - 1] == 'm';
    prog_request_file(is_wasm ? "wasm" : "view", path);
    int p = scpy(files_status, 0, "открыто в просмотре: ", sizeof files_status);
    (void)scpy(files_status, p, path, sizeof files_status);
}

static void on_input(int id, int type, int x, int y, unsigned buttons) {
    if (id == w_files) {
        app_hit_t h; app_files_hit(&ev, x, y, &h);
        int press = type == 4 && (buttons & 1) && !(files_btn & 1);
        int release = type == 4 && !(buttons & 1) && (files_btn & 1);
        int old_drop = files_drop, drop = -1;
        if (type == 4) files_btn = buttons;
        if (type == 3 && drag.on && (buttons & 1)) {                    /* тащат: над другой панелью - подсветка цели */
            int dx = x - drag.x, dy = y - drag.y;
            if ((dx * dx + dy * dy > 64) && h.kind != APP_HIT_NONE && h.kind <= APP_HIT_SCROLL && h.panel != drag.panel) drop = h.panel;
            files_drop = drop;
        }
        if (press) {
            files_status[0] = 0;
            uint64_t now = clock_ns();
            if (h.kind == APP_HIT_ROW) {
                int dbl = h.panel == files_click_panel && h.row == files_click_row && now - files_click_ns < 450000000ULL;
                explorer_mouse(dbl ? FM_MOUSE_DOUBLE : FM_MOUSE_CLICK, h.panel, h.row);
                files_click_panel = dbl ? -1 : h.panel; files_click_row = h.row; files_click_ns = now;
                drag.on = !dbl; drag.panel = h.panel; drag.row = h.row; drag.x = x; drag.y = y;
            } else if (h.kind == APP_HIT_PANEL) explorer_mouse(FM_MOUSE_CLICK, h.panel, -1);
            else if (h.kind == APP_HIT_SCROLL) explorer_page(h.panel, h.row < 0 ? -1 : 1);
            else if (h.kind == APP_HIT_REFRESH) explorer_refresh();
            else if (h.kind == APP_HIT_HIDDEN) explorer_key(KEY_F9, 0);
            else if (h.kind == APP_HIT_KEY) explorer_key(KEY_F1 + h.row, 0);
        }
        if (release) {
            if (drag.on && files_drop >= 0) explorer_mouse(FM_MOUSE_DROP, drag.panel, drag.row);
            drag.on = 0; files_drop = -1;
        }
        if (h.kind != files_hover.kind || h.panel != files_hover.panel || h.row != files_hover.row || press || release || files_drop != old_drop) {
            files_hover = h; draw_files();
        }
        return;
    }
    if (id == w_mind) {
        int h = app_mind_hit(x, y);
        int press = type == 4 && (buttons & 1) && !(mind_btn & 1);
        if (type == 4) mind_btn = buttons;
        if (press && h >= 0 && (unsigned)h != mind_page) { mind_page = (unsigned)h; draw_mind(); }
        return;
    }
    if (id == w_set) {
        int h = app_settings_hit(x, y);
        int press = type == 4 && (buttons & 1) && !(set_btn & 1);
        if (type == 4) set_btn = buttons;
        if (press && h >= 0) {
            if (h == 10 || h == 11) { mind_sys_request_mode(h - 10); mv.mode = (uint8_t)(h - 10); }
            else if (h >= 20 && h <= 24) { mind_sys_request_fx(h - 20); mv.fx_user = (uint8_t)(h - 20); }
            else if (h == 30 || h == 31) {
                int32_t tz = (mv.t ? mv.tz_min : mind_sys_tz()) + (h == 30 ? -60 : 60);
                if (tz >= -720 && tz <= 840) { mind_sys_request_tz(tz); mv.tz_min = tz; }
            } else if (h == 32 || h == 33) mind_sys_request_rtc_local(h - 32);
            else if (h == 40 || h == 41) mind_sys_set_telemetry(h - 40);
        }
        if (h != set_hover || press) { set_hover = h; draw_settings(); }
        return;
    }
    if (id == w_mind || id == w_mon || id == w_con) return;
    gfx_on_input(id, type, x, y, buttons);
}

static void on_resize(int id, int w, int h) {
    (void)w; (void)h;
    if (id == w_con) fbcon_resize();
}

/* ---------------- быстрый доступ ---------------- */

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Виды плиток боковой панели (side_kind): закреплённая, открытая незакреплённая, «ещё N», «+». */
enum { SK_PINNED, SK_RUN, SK_MORE, SK_ADD };
#define RUN_MAX WM_MAX_WIN                                /* просматриваем метки всех окон */
static unsigned char side_kind[SIDE_N];
static char side_hints[SIDE_N][64];
static char side_labels[SIDE_N][40];
static char run_prev[RUN_MAX][16];                         /* какие программы были открыты в прошлый раз (для журнала) */
static int run_prev_n;

/* Журнал появления и исчезновения значков: по нему стенд проверяет док без снимков. */
static void dock_log_changes(char (*run)[16], int nrun) {
    char line[160]; int p;
    for (int i = 0; i < nrun; i++) {
        int seen = 0; for (int k = 0; k < run_prev_n; k++) if (streq(run_prev[k], run[i])) seen = 1;
        if (seen) continue;
        p = scpy(line, 0, "[KIRA DOCK] открыта: ", sizeof line);
        p = scpy(line, p, run[i], sizeof line);
        p = scpy(line, p, " «", sizeof line);
        p = scpy(line, p, app_label(run[i]), sizeof line);
        scpy(line, p, dock_find(&dock, run[i]) >= 0 ? "» (закреплена)\n" : "» (временный значок)\n", sizeof line);
        uart_print(line); /* одна строка под uart_lock: пользовательский вывод не вклинивается */
    }
    for (int k = 0; k < run_prev_n; k++) {
        int seen = 0; for (int i = 0; i < nrun; i++) if (streq(run_prev[k], run[i])) seen = 1;
        if (seen) continue;
        p = scpy(line, 0, "[KIRA DOCK] закрыта: ", sizeof line);
        p = scpy(line, p, run_prev[k], sizeof line);
        p = scpy(line, p, " «", sizeof line);
        p = scpy(line, p, app_label(run_prev[k]), sizeof line);
        scpy(line, p, dock_find(&dock, run_prev[k]) >= 0 ? "» (значок остаётся: закреплена)\n" : "» (значок убран)\n", sizeof line);
        uart_print(line);
    }
    run_prev_n = nrun;
    for (int i = 0; i < nrun; i++) scpy(run_prev[i], 0, run[i], 16);
}

/* Заполнить плитку по системному имени программы: русское название, описание, значок, цвет. */
static void side_fill(int k, const char* name, int kind) {
    scpy(side_names[k], 0, name, 16);
    int p = scpy(side_hints[k], 0, name, (int)sizeof side_hints[k]);
    p = scpy(side_hints[k], p, kind == SK_PINNED ? " · правая кнопка - открепить" : " · правая кнопка - закрепить", (int)sizeof side_hints[k]);
    side_kind[k] = (unsigned char)kind;
    side[k] = (wm_side_item_t){ app_label(side_names[k]), side_names[k], dock_color(name), dock_icon(name), 0, kind == SK_RUN, side_hints[k], 0, app_desc(name) };
    if (side[k].icon == ICON_TERMINAL && name[0] >= 'a' && name[0] <= 'z') side[k].badge = (char)(name[0] - 32);
}

/* Собрать значки: закреплённые, затем открытые незакреплённые (динамические, на диск не пишутся), «ещё N» если не
   помещаются, внизу «+». Перестраивается, только если что-то изменилось. Окон приложений Ring 3 может быть много,
   плитки сжимаются (wm.c), а сверх предела остаётся одна плитка «ещё N» (щелчок открывает первую скрытую программу). */
static void side_rebuild(int force) {
    static char run[RUN_MAX][16];
    int raw = wm_user_tags(run, RUN_MAX), nrun = 0;
    for (int i = 0; i < raw; i++) {                          /* одна плитка на программу, порядок - по окнам */
        if (!run[i][0]) continue;
        int dup = 0; for (int k = 0; k < nrun; k++) if (streq(run[k], run[i])) { dup = 1; break; }
        if (!dup) { if (nrun != i) scpy(run[nrun], 0, run[i], 16); nrun++; }
    }
    dock_log_changes(run, nrun);
    int cap = wm_side_capacity() - dock.n - 1;                /* сколько плиток открытых программ влезет */
    if (cap < 0) cap = 0;
    int unp = 0; for (int i = 0; i < nrun; i++) if (dock_find(&dock, run[i]) < 0) unp++;
    int show = unp > cap ? (cap > 0 ? cap - 1 : 0) : unp, more = unp - show;
    if (cap == 0) more = 0;                                   /* места нет совсем: «ещё» тоже не поместится */
    char sig[SIDE_N * 17 + 16]; int sp = 0;
    for (int i = 0; i < dock.n; i++) { sp = scpy(sig, sp, dock.name[i], (int)sizeof sig); sig[sp++] = '|'; sig[sp] = 0; }
    sig[sp++] = '#'; sig[sp] = 0;
    int shown = 0;
    for (int i = 0; i < nrun && shown < show; i++) if (dock_find(&dock, run[i]) < 0) { sp = scpy(sig, sp, run[i], (int)sizeof sig); sig[sp++] = '|'; sig[sp] = 0; shown++; }
    if (more > 0) { sig[sp++] = '+'; sig[sp++] = (char)('A' + (more > 25 ? 25 : more)); sig[sp] = 0; }
    if (!force && streq(sig, side_sig)) return;
    scpy(side_sig, 0, sig, (int)sizeof side_sig);
    side_n = 0;
    for (int i = 0; i < dock.n && side_n < SIDE_N - 1; i++) { side_fill(side_n, dock.name[i], SK_PINNED); side_n++; }
    shown = 0;
    const char* first_hidden = 0;
    for (int i = 0; i < nrun && side_n < SIDE_N - 2; i++) {
        if (dock_find(&dock, run[i]) >= 0) continue;
        if (shown >= show) { if (!first_hidden) first_hidden = run[i]; continue; }
        side_fill(side_n, run[i], SK_RUN); side_n++; shown++;
    }
    if (more > 0 && first_hidden && side_n < SIDE_N - 1) {
        int k = side_n++;
        scpy(side_names[k], 0, first_hidden, 16);
        int p = scpy(side_labels[k], 0, "Ещё программ: ", (int)sizeof side_labels[k]);
        { char dg[12]; int q = 0, tn = 0, v = more; char t2[12]; do { t2[tn++] = (char)('0' + v % 10); v /= 10; } while (v); while (tn) dg[q++] = t2[--tn]; dg[q] = 0; scpy(side_labels[k], p, dg, (int)sizeof side_labels[k]); }
        p = scpy(side_hints[k], 0, "щелчок - открыть: ", (int)sizeof side_hints[k]);
        scpy(side_hints[k], p, app_label(first_hidden), (int)sizeof side_hints[k]);
        side_kind[k] = SK_MORE;
        side[k] = (wm_side_item_t){ side_labels[k], side_names[k], TH_TEXT_DIM, ICON_MORE, 0, 1, side_hints[k], 0, "Окон открыто больше, чем помещается в панели" };
    }
    side_kind[side_n] = SK_ADD;
    side[side_n++] = (wm_side_item_t){ "Добавить программу", "", TH_TEXT_DIM, ICON_FOLDER, 1, 0, "откроет /bin в Проводнике", 0, "" };
    wm_side_set(side, side_n);
}

static void dock_save(void) {
    char text[256];
    unsigned n = dock_format(&dock, text, sizeof text);
    if (explorer_write(DOCK_PATH, text, n) != 0) uart_print("[KIRA DISPLAY] quick-launch list not saved: disk queue is busy.\n");
}

static void dock_toggle(const char* name) {
    if (dock_find(&dock, name) >= 0) dock_remove(&dock, name);
    else if (!dock_valid_name(name)) { uart_print("[KIRA DISPLAY] quick-launch: this name cannot be pinned.\n"); return; }
    else if (dock_add(&dock, name) < 0) { uart_print("[KIRA DISPLAY] quick-launch panel is full (10 programs).\n"); return; }
    dock_save();
    side_rebuild(1);
    draw_files();
}

/* Запуск программы из быстрого доступа: так же, как из Проводника - команда в терминал. */
static void launch_program(const char* name) {
    static char last_name[16]; static uint64_t last_ns;
    uint64_t now = clock_ns();
    if (streq(last_name, name) && now - last_ns < 1500000000ULL) return;   /* программа ещё создаёт окно */
    scpy(last_name, 0, name, 16); last_ns = now;
    files_run(name);
}

static void on_launch(int index) {
    if (index < 0 || index >= side_n) return;
    if (side[index].bottom) {                                       /* «+»: Проводник с раскрытым /bin */
        open_app(APP_FILES);
        explorer_goto(-1, "/bin");
        return;
    }
    launch_program(side_names[index]);
}

static void on_side_alt(int index) {
    if (index < 0 || index >= side_n || side[index].bottom || side_kind[index] == SK_MORE) return;
    char name[16]; scpy(name, 0, side_names[index], 16);
    dock_toggle(name);
}

static void on_tab(int i) {
    if (i == 0) { wm_minimize_all(); return; }
    if (i < (int)(sizeof app_of_tab / sizeof app_of_tab[0])) open_app(app_of_tab[i]);
}

/* ---------------- верхняя панель ---------------- */

static void update_clock(void) {
    rtc_time_t t;
    char date[48], hm[8];
    if (rtc_now(mind_sys_local_offset(), &t) != 0) { wm_set_clock("", ""); return; }
    rtc_format_date(&t, date, sizeof date);
    rtc_format_hm(&t, hm, sizeof hm);
    {   /* раскладка клавиатуры (Alt+Shift) - рядом с датой */
        unsigned n = 0; while (date[n]) n++;
        const char* l = input_layout() ? "  ·  RU" : "  ·  EN";
        for (unsigned i = 0; l[i] && n + 1 < sizeof date; i++) date[n++] = l[i];
        date[n] = 0;
    }
    wm_set_clock(date, hm);
}

static void update_mind(void) {
    uint32_t v = mind_sys_version();
    if (v == mv_ver) return;
    mv_ver = v;
    mind_sys_view(&mv);
    if (mv.t) wm_set_meters(mv.busy, (int)(mv.cores > 64 ? 64 : mv.cores), (int)mv.mem_used, mv.health);
    draw_mind();
    draw_monitor();
    draw_settings();
}

static void update_files(void) {
    uint32_t v = explorer_version();
    if (v == ev_ver) return;
    ev_ver = v;
    explorer_view(&ev);
    if (!dock_ready && ev.dock_loaded) {                    /* список быстрого доступа с диска (нет файла - как по умолчанию) */
        dock_ready = 1;
        if (ev.dock_len < 0 || dock_parse(&dock, ev.dock_text, (unsigned)ev.dock_len) == 0) {
            if (ev.dock_len < 0) dock_default(&dock); else dock.n = 0;
        }
        side_rebuild(1);
    }
    if (ev.fm.act_seq != act_seen) { act_seen = ev.fm.act_seq; files_act(); }
    draw_files();
}

/* ---------------- сборка кадра: на всех ядрах или на одном ----------------
   Параллельная сборка выгодна, только если ядра настоящие: на эмуляторе, который исполняет все виртуальные ядра одним
   потоком телефона (Limbo), помощники лишь добавляют переключения. Поэтому цена 1000 точек меряется в обоих режимах
   (скользящее среднее по крупным кадрам), раз в 5 с 16 кадров собираются другим способом, и выбирается дешёвый
   (если дешевле хотя бы на 15%). */
static struct { uint32_t par; uint64_t cost[2]; uint32_t n[2]; uint32_t probe; uint64_t next_ns; uint64_t gap_ns; uint32_t confirm; uint32_t init; } cs;   /* gap_ns: пауза между пробами, растёт вдвое, пока выбор подтверждается */

void display_compositor_stats(uint32_t* par, uint32_t* ns_par, uint32_t* ns_ser) {
    *par = cs.par; *ns_par = (uint32_t)cs.cost[1]; *ns_ser = (uint32_t)cs.cost[0];
}

static int comp_mode(uint64_t now) {
    if (!kpar_helpers()) return 0;
    if (!cs.init) { cs.init = 1; cs.par = 1; }                  /* начало - как всегда было: на всех ядрах; на эмуляторе в один поток проба это поправит */
    if (!cs.gap_ns) cs.gap_ns = 5000000000ULL;
    if (!cs.probe && now >= cs.next_ns) { cs.probe = 16; cs.next_ns = now + cs.gap_ns; }
    return cs.probe ? !cs.par : (int)cs.par;
}

/* Параллельная сборка - по умолчанию и при малейшей выгоде (она же разгружает поток экрана); на одно ядро уходим, только если оно
   дешевле на 25% в двух пробах подряд: шум замера не должен сгонять сборку с ядер. Пауза между пробами растёт вдвое до минуты. */
static void comp_note(int mode, uint64_t ns, uint64_t px) {
    if (px < 20000) return;                                   /* мелкие кадры не показательны: накладные расходы */
    uint64_t c = ns * 1000 / px;
    cs.cost[mode] = cs.n[mode] ? (cs.cost[mode] * 7 + c) / 8 : c;
    cs.n[mode]++;
    if (cs.probe && --cs.probe == 0 && cs.n[0] >= 8 && cs.n[1] >= 8) {
        uint32_t other = !cs.par;
        int better = other ? (cs.cost[1] * 100 < cs.cost[0] * 95) : (cs.cost[0] * 100 < cs.cost[1] * 75);
        if (better) {
            if (!other && ++cs.confirm < 2) { cs.next_ns = 0; return; }   /* уход на одно ядро - после повторной пробы */
            cs.confirm = 0; cs.par = other; cs.gap_ns = 30000000000ULL;
            uart_print(other ? "[KIRA DISPLAY] compositor: all cores are cheaper here (" : "[KIRA DISPLAY] compositor: one core is cheaper here (");
            uart_print_dec(cs.cost[other]); uart_print(" vs "); uart_print_dec(cs.cost[!other]);
            uart_print(" ns per 1000 pixels) - switching.\n");
        } else {
            cs.confirm = 0;
            if (cs.gap_ns < 60000000000ULL) cs.gap_ns *= 2;
        }
    }
}

/* ---------------- цикл ---------------- */

static void display_main(void* arg) {
    (void)arg;
    uint64_t clock_t0 = 0;

    for (;;) {
        uint64_t t0 = clock_ns();
        if (t0 && !st.start_ns) {                      /* часы TSC включились: статистика с нуля, калибровка в неё не входит */
            st.start_ns = t0; st.frames = st.composed = st.busy_ns = st.late = st.worst_gap_ns = st.worst_busy_ns = 0;
        }
        if (st.last_t0 && t0) {
            uint64_t gap = t0 - st.last_t0;
            if (gap > st.worst_gap_ns) st.worst_gap_ns = gap;
            if (gap > FRAME_NS + FRAME_NS / 2) st.late++;
        }
        st.last_t0 = t0;
        int timed = t0 != 0;

        gfx_server_run();                 // окна приложений Ring 3: создание, перерисовка, закрытие

        {                                 // Проводник сверху: клавиатура у него (консоль в это время не читает)
            int own = w_files >= 0 && wm_visible(w_files) && wm_top() == w_files;
            __atomic_store_n(&files_focus, own, __ATOMIC_RELEASE);
            input_event_t ke;
            while (own && input_pop(&ke)) if (ke.type == INPUT_KEY_DOWN && !(ke.key >= KEY_LSHIFT && ke.key <= KEY_CAPS)) explorer_key(ke.key, ke.ch);   // модификаторы - не команды
        }
        if (__atomic_load_n(&rq_pending, __ATOMIC_ACQUIRE)) {      // команда «files»: открыть Проводник на этих путях
            char l[FM_PATH], r[FM_PATH];
            uint64_t fl = spin_lock_irqsave(&rq_lk);
            scpy(l, 0, rq_l, FM_PATH); scpy(r, 0, rq_r, FM_PATH); rq_pending = 0;
            spin_unlock_irqrestore(&rq_lk, fl);
            open_app(APP_FILES);
            if (r[0]) explorer_goto(1, r);                           // левая последней: она становится активной
            if (l[0]) explorer_goto(0, l);
        }

        int hk = input_take_hotkey();     // F11 - на весь экран, F10 - свернуть на фон
        if (hk) {
            int top = wm_top();
            if (hk == KEY_F11) wm_set_fullscreen(top, !wm_fullscreen(top));
            else if (hk == KEY_F10) wm_minimize(top);
        }

        mouse_event_t me;
        while (input_pop_mouse(&me)) {
            st.mouse_events++;
            if (me.abs) wm_mouse_abs(me.ax, me.ay, me.buttons); /* планшет/сенсор: абсолютная точка экрана */
            else wm_mouse(me.dx, me.dy, me.buttons);
        }

        int y0 = 0, y1 = 0;
        unsigned n = fbcon_update(&y0, &y1);
        if (n && y1 > y0) wm_invalidate_rows(w_con, y0, y1);

        update_mind();
        update_files();

        uint64_t now0 = clock_ns();
        if (now0 - clock_t0 >= 500000000ULL) { clock_t0 = now0; update_clock(); }
        side_rebuild(0);                                   /* появились/закрылись окна программ - левая панель следует */
        {                                                  /* подключили или вынули флешку - Проводник перечитывает дерево */
            static uint32_t vgen;
            uint32_t g = vfs_generation();
            if (g != vgen) { vgen = g; explorer_refresh(); }
        }

        wm_set_fx(fx_req);
        wm_anim((uint32_t)(now0 / 1000000ULL), mv.activity);

        uint64_t tp0 = clock_ns();
        uint64_t px = 0;
        int cmode = comp_mode(tp0);
        wm_set_runner(cmode ? kpar_run : 0);
        if (wm_compose_par(fb_present_rect, &px)) { st.composed++; st.pixels += px; }
        st.present_ns = clock_ns() - tp0;
        comp_note(cmode, st.present_ns, px);
        fb_frame_done();                                   /* virtio-gpu: изменённое - устройству (без кадрового буфера в памяти устройства) */
        st.frames++;

        uint64_t now = clock_ns();
        uint64_t elapsed = now - t0;
        if (timed) { st.busy_ns += elapsed; if (elapsed > st.worst_busy_ns) st.worst_busy_ns = elapsed; }
        uint32_t ticks = 1;
        if (elapsed < FRAME_NS) { uint64_t rest = FRAME_NS - elapsed; ticks = (uint32_t)((rest + TICK_NS - 1) / TICK_NS); if (ticks < 1) ticks = 1; }
        sched_sleep_ticks(ticks);
    }
}

static void* wm_alloc(uint64_t bytes) { return gfx_alloc_pages(bytes); }

static int display_init_finish(int rc) {
    if (rc != 0 && virtio_gpu_probe() == 0) rc = 0;
    if (rc != 0 && platform_fb_probe() == 0) rc = 0;
    if (rc != 0) return rc;
    wm_ops_t ops = { wm_alloc, on_visibility, on_input, on_launch, on_resize, on_tab, on_side_alt };
    wm_init(fb_surface(), &ops);
    surface_t* fbs = fb_surface();
    w_con = wm_create_ex("Терминал", WM_AUTO, WM_AUTO, CON_W, CON_H, WM_RESIZABLE, fbs->w, fbs->h);   // можно растянуть до размера экрана
    if (w_con < 0) { uart_print("[KIRA DISPLAY] cannot allocate window surfaces.\n"); return -7; }
    wm_set_resize_limits(w_con, 320, 160, 8, 16);       // кратно размеру знакоместа
    wm_set_tag(w_con, "Console");
    wm_set_subtitle(w_con, "журнал ядра");
    dock_default(&dock);                                   /* до чтения /var/dock: обычный набор */
    side_rebuild(1);
    wm_tabs_set(tabs, (int)(sizeof tabs / sizeof tabs[0]));
    fbcon_reserve(fbs->w, fbs->h, wm_alloc);              /* сетка знаков - под любой экран */
    fbcon_init(wm_content(w_con));
    fbcon_repaint();
    wm_raise(w_con);
    explorer_init();
    st.start_ns = clock_ns();
#ifndef KIRA_HOST_TEST
    {   /* композитор - на всех ядрах: помощников по числу ядер сверх одного (поток дисплея работает сам) */
        unsigned cpus = scale_cpus(platform_cpus());
        kpar_init(cpus > 1 ? cpus - 1 : 0);
        wm_set_runner(kpar_run);
        cs.par = kpar_helpers() ? 1 : 0;
    }
#endif
    if (!sched_create("display", display_main, 0, PRIO_CRITICAL)) {
        uart_print("[KIRA DISPLAY] cannot create display thread.\n");
        return -6;
    }
    ready = 1;
    uart_print("[KIRA DISPLAY] Shell online: top bar (sections, clock, cores, memory, health), side launcher, "
               "apps Terminal / Mind / Files / Monitor / Settings, Ring 3 programs from /bin, neural background, "
               "fullscreen (F11), minimize (F10), damage tiles composed in parallel on all cores.\n");
    return 0;}

int display_init(unsigned long mbi) {
    return display_init_finish(mbi ? fb_init(mbi) : -1);
}

int display_init_bootinfo(const kbi_header_t *bootinfo) {
    return display_init_finish(bootinfo ? fb_init_bootinfo(bootinfo) : -1);
}

void display_set_fx(int level) { fx_req = level < 0 ? 0 : level > 3 ? 3 : level; }

void display_get_stats(uint64_t* loops, uint64_t* late, uint64_t* worst_gap_ns) {
    *loops = st.frames; *late = st.late; *worst_gap_ns = st.worst_gap_ns;
    st.worst_gap_ns = 0;                       /* худший промежуток - за период между вызовами */
}

void display_request_files(const char* left, const char* right) {
    uint64_t fl = spin_lock_irqsave(&rq_lk);
    scpy(rq_l, 0, left ? left : "", FM_PATH); scpy(rq_r, 0, right ? right : "", FM_PATH);
    __atomic_store_n(&rq_pending, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&rq_lk, fl);
}

int display_kbd_owner(void) { return __atomic_load_n(&files_focus, __ATOMIC_ACQUIRE); }

void display_print_info(void) {
    const fb_info_t* i = fb_get_info();
    if (!fb_available()) {
        uart_print("\n[FB] no framebuffer (the bootloader gave text mode): serial console only.\n\n");
        return;
    }
    uart_print("\n[FB] "); uart_print_dec(i->width); uart_print("x"); uart_print_dec(i->height);
    uart_print("x32 at "); uart_print_hex(i->phys);
    uart_print(", pitch "); uart_print_dec(i->pitch_bytes);
    uart_print(i->write_combining ? ", write-combining" : ", NOT write-combining");
    uart_print("\n[FB] back buffer "); uart_print_hex(i->back_phys);
    uart_print(" ("); uart_print_dec(i->back_bytes >> 10); uart_print(" KB)\n[DISPLAY] frames "); uart_print_dec(st.frames);
    uart_print(", loop busy avg ");
    uart_print_dec(st.frames ? st.busy_ns / st.frames / 1000 : 0);
    uart_print(" us, last present "); uart_print_dec(st.present_ns / 1000);
    uart_print(" us\n[DISPLAY] drawn frames "); uart_print_dec(st.composed);
    uint64_t span = clock_ns() - st.start_ns;
    uart_print(" ("); uart_print_dec(span ? st.composed * 1000000000ULL / span : 0);
    uart_print(" per second on average), late cycles "); uart_print_dec(st.late);
    uart_print(", worst gap "); uart_print_dec(st.worst_gap_ns / 1000);
    uart_print(" us, worst cycle "); uart_print_dec(st.worst_busy_ns / 1000);
    uart_print(" us, budget "); uart_print_dec(FRAME_NS / 1000);
    uart_print(" us\n\n");
}

void display_panic_flush(void) {
    if (!fb_available() || in_flush || w_con < 0) return;
    in_flush = 1;
    int y0, y1;
    for (int guard = 0; guard < 64 && fbcon_update(&y0, &y1); guard++) { }
    wm_set_visible(w_con, 1);
    wm_raise(w_con);
    wm_invalidate_all();
    int rx, ry, rw, rh;
    if (wm_compose(&rx, &ry, &rw, &rh)) fb_present();
}
