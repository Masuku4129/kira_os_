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
#include "prog_catalog.h"
#include "platform.h"
#include "virtio_gpu.h"
#include "mind_sys.h"
#include "explorer.h"
#include "apps.h"
#include "gfxui.h"
#include "icons.h"
#include "theme.h"
#include "rtc.h"
#include "console.h"
#include "dock.h"
#include "vfs.h"
#include "spinlock.h"
#include "cpu.h"
#ifndef KIRA_HOST_TEST
#include "net.h"
#include "audio.h"
#endif

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
static int w_con = -1, w_mind = -1, w_mon = -1, w_files = -1, w_set = -1, w_launcher = -1;
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
static int mind_health_band = -1;
static uint32_t mv_ver = ~0u, ev_ver = ~0u;
static app_hit_t files_hover;
static int set_hover = -1, files_drop = -1;
static unsigned files_btn, set_btn;
static char files_status[96];
static uint32_t files_launch_id;
static uint64_t files_click_ns; static int files_click_panel = -1, files_click_row = -1;
static struct { int on, panel, row, x, y; } drag;       /* файл тащат мышью на другую панель */
static uint32_t act_seen;                                /* последняя обработанная просьба менеджера (открыть, закрепить) */
static volatile int files_focus;
static volatile int launcher_focus;
static char launcher_query[64], launcher_status[96];
static unsigned launcher_matches[16], launcher_count, launcher_selected;
static unsigned launcher_btn;
static uint32_t launcher_request;
static spinlock_t rq_lk = SPINLOCK_INIT;                 /* просьба консоли «files» открыть Проводник на путях */
#define DISPLAY_NOTIFY_QUEUE 16
typedef struct { char title[40]; char message[112]; int priority; } display_notification_t;
static display_notification_t notify_queue[DISPLAY_NOTIFY_QUEUE];
static uint32_t notify_head, notify_tail;
static spinlock_t notify_lk = SPINLOCK_INIT;
static void notify_copy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0; if (!src) src = "";
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
int display_notify(const char* title, const char* message, int priority) {
    if (!title || !message || !__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || wm_get_layout_mode() != WM_LAYOUT_SPATIAL_MINIMAL) return -1;
    uint64_t flags = spin_lock_irqsave(&notify_lk);
    if (notify_head - notify_tail >= DISPLAY_NOTIFY_QUEUE) { spin_unlock_irqrestore(&notify_lk, flags); return -2; }
    display_notification_t* n = &notify_queue[notify_head % DISPLAY_NOTIFY_QUEUE];
    notify_copy(n->title, title, sizeof n->title);
    notify_copy(n->message, message, sizeof n->message);
    n->priority = priority;
    notify_head++;
    spin_unlock_irqrestore(&notify_lk, flags);
    return 0;
}
static void notification_poll(void) {
    for (;;) {
        display_notification_t n;
        uint64_t flags = spin_lock_irqsave(&notify_lk);
        if (notify_tail == notify_head) { spin_unlock_irqrestore(&notify_lk, flags); break; }
        n = notify_queue[notify_tail % DISPLAY_NOTIFY_QUEUE];
        notify_tail++;
        spin_unlock_irqrestore(&notify_lk, flags);
        (void)wm_notification_post(n.title, n.message, n.priority);
    }
}
static char rq_l[FM_PATH], rq_r[FM_PATH]; static int rq_pending;

/* ---------------- приложения ядра ---------------- */

/* Приложения ядра открываются разделами верхней панели. Левая панель - быстрый доступ: программы из /bin,
   которые пользователь закрепил сам (Проводник: «закрепить»), ниже черты - запущенные, но не закреплённые
   (их свёрнутые окна иначе было бы не вернуть), внизу «+» - открыть /bin в Проводнике. */
enum { APP_TERM, APP_MIND, APP_FILES, APP_MON, APP_SET, APP_LAUNCHER, APP_N };
static const char* const app_tag[APP_N] = { "Console", "Mind", "Files", "Monitor", "Settings", "Launcher" };

static const wm_tab_t tabs[] = {
    { "ГЛАВНАЯ", "" }, { "ПРИЛОЖЕНИЯ", "Launcher" }, { "ПРОВОДНИК", "Files" },
    { "KIRA AI", "Mind" }, { "МОНИТОР", "Monitor" }, { "НАСТРОЙКИ", "Settings" }, { "ТЕРМИНАЛ", "Console" },
};
static const int app_of_tab[] = { -1, APP_LAUNCHER, APP_FILES, APP_MIND, APP_MON, APP_SET, APP_TERM };

#define SIDE_N (DOCK_MAX + 8 + 1)
static dock_t dock;
static int dock_ready;                                    /* список прочитан с диска (или принят по умолчанию) */
static wm_side_item_t side[SIDE_N];
static char side_names[SIDE_N][16];
static int side_n;
static char side_sig[SIDE_N * 17];                         /* что показано сейчас: пересобирать только при изменении */

static void draw_mind(void) { if (wm_visible(w_mind)) { app_mind_draw(wm_content(w_mind), &mv); wm_invalidate(w_mind); } }

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

static int scpy(char* d, int p, const char* s, int cap);
static void files_launch_poll(void) {
    if (!files_launch_id) return;
    int result = 0;
    prog_result_state_t state = prog_request_take_result(files_launch_id, &result);
    if (state == PROG_RESULT_PENDING) return;
    files_launch_id = 0;
    if (state != PROG_RESULT_COMPLETE) (void)scpy(files_status, 0, "результат запуска недоступен", sizeof files_status);
    else if (result > 0) (void)scpy(files_status, 0, "программа успешно запущена", sizeof files_status);
    else if (result == -2) (void)scpy(files_status, 0, "запуск отклонён: проверка доверия", sizeof files_status);
    else if (result == -3) (void)scpy(files_status, 0, "ошибка формата исполняемого файла", sizeof files_status);
    else if (result == -4) (void)scpy(files_status, 0, "лимит запуска KIRA Mind", sizeof files_status);
    else (void)scpy(files_status, 0, "не удалось открыть файл/запустить программу", sizeof files_status);
    wm_notification_post("Проводник", files_status, state == PROG_RESULT_COMPLETE && result > 0 ? WM_NOTIFY_INFO : WM_NOTIFY_WARNING);
    draw_files();
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

static int scpy(char* d, int p, const char* s, int cap);

static void launcher_refresh(void) {
    launcher_count = prog_catalog_search(launcher_query, launcher_matches, 7);
    if (launcher_selected >= launcher_count) launcher_selected = launcher_count ? launcher_count - 1 : 0;
}
static void draw_launcher(void) {
    if (w_launcher < 0 || !wm_visible(w_launcher)) return;
    surface_t* s = wm_content(w_launcher);
    gfx_fill(s, TH_CONTENT);
    ui_card(s, 12, 10, s->w - 24, 58, "ПОИСК ПРИЛОЖЕНИЙ", TH_CYAN);
    ui_round_rect(s, 24, 36, s->w - 48, 26, 7, TH_TILE, 255);
    ui_round_frame(s, 24, 36, s->w - 48, 26, 7, TH_BORDER, 255);
    ui_text_fit(s, 34, 43, launcher_query[0] ? launcher_query : "Начните вводить название или назначение…", s->w - 68, launcher_query[0] ? TH_TEXT : TH_TEXT_FAINT);
    if (!launcher_count) gfx_text(s, 28, 92, "Ничего не найдено. Измените запрос.", TH_TEXT_DIM, -1);
    for (unsigned i = 0; i < launcher_count && i < 7; i++) {
        const prog_catalog_entry_t* e = prog_catalog_get(launcher_matches[i]);
        if (!e) continue;
        int y = 82 + (int)i * 38;
        if (i == launcher_selected) ui_round_rect(s, 20, y - 3, s->w - 40, 34, 7, TH_CYAN, 34);
        gfx_text(s, 30, y, e->title, i == launcher_selected ? TH_CYAN : TH_TEXT, -1);
        ui_text_fit(s, 220, y, e->description, s->w - 250, TH_TEXT_DIM);
        gfx_text(s, 30, y + 16, e->category, TH_TEXT_FAINT, -1);
    }
    ui_statusbar(s, s->h - TH_BAR_H, launcher_status[0] ? launcher_status : "Enter — запуск · ↑/↓ — выбор · Esc — закрыть", TH_TEXT_DIM, "Проверка доверия — в ядре");
    wm_invalidate(w_launcher);
}
static void launcher_search_update(void) { launcher_selected = 0; launcher_refresh(); draw_launcher(); }
static void launcher_launch_selected(void) {
    if (launcher_selected >= launcher_count) return;
    const prog_catalog_entry_t* e = prog_catalog_get(launcher_matches[launcher_selected]);
    if (!e) return;
    uint32_t id = 0;
    prog_request_status_t st = prog_request(e->name, &id);
    if (st == PROG_REQUEST_ACCEPTED) {
        launcher_request = id;
        (void)scpy(launcher_status, 0, "Запрос принят: ожидание запуска…", sizeof launcher_status);
    } else if (st == PROG_REQUEST_BUSY) {
        (void)scpy(launcher_status, 0, "Очередь запуска занята. Повторите позже.", sizeof launcher_status);
    } else {
        (void)scpy(launcher_status, 0, "Некорректное имя приложения.", sizeof launcher_status);
    }
    draw_launcher();
}
static void launcher_poll(void) {
    if (!launcher_request) return;
    int result = 0;
    prog_result_state_t state = prog_request_take_result(launcher_request, &result);
    if (state == PROG_RESULT_PENDING) return;
    launcher_request = 0;
    if (state != PROG_RESULT_COMPLETE) (void)scpy(launcher_status, 0, "Результат запуска недоступен.", sizeof launcher_status);
    else if (result > 0) (void)scpy(launcher_status, 0, "Приложение успешно запущено.", sizeof launcher_status);
    else if (result == -2) (void)scpy(launcher_status, 0, "Запуск отклонён проверкой доверия.", sizeof launcher_status);
    else if (result == -3) (void)scpy(launcher_status, 0, "Неверный формат исполняемого файла.", sizeof launcher_status);
    else (void)scpy(launcher_status, 0, "Не удалось запустить приложение.", sizeof launcher_status);
    wm_notification_post("Приложения", launcher_status, state == PROG_RESULT_COMPLETE && result > 0 ? WM_NOTIFY_INFO : WM_NOTIFY_WARNING);
    draw_launcher();
}
static void launcher_key(const input_event_t* ke) {
    if (ke->type != INPUT_KEY_DOWN) return;
    if (ke->key == KEY_ESC) { wm_set_visible(w_launcher, 0); launcher_focus = 0; return; }
    if (ke->key == KEY_ENTER) { launcher_launch_selected(); return; }
    if (ke->key == KEY_UP) { if (launcher_selected) launcher_selected--; draw_launcher(); return; }
    if (ke->key == KEY_DOWN) { if (launcher_selected + 1 < launcher_count) launcher_selected++; draw_launcher(); return; }
    if (ke->key == KEY_BACKSPACE) {
        unsigned n = 0; while (launcher_query[n]) n++;
        if (n) { do { n--; } while (n && (((unsigned char)launcher_query[n] & 0xC0) == 0x80)); launcher_query[n] = 0; launcher_search_update(); }
        return;
    }
    uint16_t c = input_cyr(ke->key, ke->ch, ke->mods);
    if (c >= 32 && c != 127) {
        unsigned n = 0; while (launcher_query[n]) n++;
        if (c < 0x80 && n + 1 < sizeof launcher_query) launcher_query[n++] = (char)c;
        else if (c < 0x800 && n + 2 < sizeof launcher_query) { launcher_query[n++] = (char)(0xC0 | (c >> 6)); launcher_query[n++] = (char)(0x80 | (c & 63)); }
        else if (n + 3 < sizeof launcher_query) { launcher_query[n++] = (char)(0xE0 | (c >> 12)); launcher_query[n++] = (char)(0x80 | ((c >> 6) & 63)); launcher_query[n++] = (char)(0x80 | (c & 63)); }
        launcher_query[n] = 0; launcher_search_update();
    }
}

static void redraw_app(int id) {
    if (id == w_mind) draw_mind();
    else if (id == w_mon) draw_monitor();
    else if (id == w_files) draw_files();
    else if (id == w_set) draw_settings();
    else if (id == w_launcher) draw_launcher();
}

static int open_app(int app) {
    int* slot; const char* title; int w, h;
    switch (app) {
    case APP_MIND:  slot = &w_mind;  title = "Разум";     w = APP_MIND_W;  h = APP_MIND_H;  break;
    case APP_MON:   slot = &w_mon;   title = "Монитор";   w = APP_MON_W;   h = APP_MON_H;   break;
    case APP_FILES: slot = &w_files; title = "Проводник"; w = APP_FILES_W; h = APP_FILES_H; break;
    case APP_SET:   slot = &w_set;   title = "Настройки"; w = APP_SET_W;   h = APP_SET_H;   break;
    case APP_LAUNCHER: slot = &w_launcher; title = "Приложения"; w = 640; h = 390; break;
    case APP_TERM:  slot = &w_con;   title = "Терминал";  w = CON_W;       h = CON_H;       break;
    default: return -1;
    }
    if (*slot < 0) {
        *slot = wm_create(title, WM_AUTO, WM_AUTO, w, h, WM_INPUT);
        if (*slot < 0) { uart_print("[KIRA DISPLAY] not enough memory for a window.\n"); return -1; }
        wm_set_tag(*slot, app_tag[app]);
        if (app == APP_MIND) wm_set_subtitle(*slot, "ядро");
        if (app == APP_FILES) { wm_set_subtitle(*slot, "KiraFS"); explorer_refresh(); }
        if (app == APP_LAUNCHER) wm_set_subtitle(*slot, "поиск и запуск");
    } else {
        wm_set_visible(*slot, 1);
        wm_raise(*slot);
    }
    redraw_app(*slot);
    return *slot;
}

/* ---------------- события оболочки ---------------- */

static void on_visibility(int id, int visible) {
    if (visible == -1) { if (id == w_launcher) { w_launcher = -1; launcher_focus = 0; } gfx_on_close(id); return; }   // красная кнопка на окне приложения
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
    uint32_t request_id = 0;
    prog_request_status_t accepted = prog_request_file(is_wasm ? "wasm" : "view", path, &request_id);
    if (accepted == PROG_REQUEST_ACCEPTED) {
        files_launch_id = request_id;
        (void)scpy(files_status, 0, "запрос просмотра принят", sizeof files_status);
    } else if (accepted == PROG_REQUEST_BUSY) {
        (void)scpy(files_status, 0, "запуск занят: дождитесь результата", sizeof files_status);
    } else {
        (void)scpy(files_status, 0, "путь или имя файла некорректны/слишком длинны", sizeof files_status);
    }
}

static void on_input(int id, int type, int x, int y, unsigned buttons) {
    if (id == w_launcher) {
        int press = type == 4 && (buttons & 1) && !(launcher_btn & 1);
        if (type == 4) launcher_btn = buttons;
        if (press && y >= 78) { unsigned row = (unsigned)(y - 78) / 38; if (row < launcher_count && row < 7) { launcher_selected = row; launcher_launch_selected(); } }
        return;
    }
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

/* Собрать только закреплённые значки Dock. Перестраивается при изменении списка закреплений. */
static void side_rebuild(int force) {
    /* Spatial Minimal Dock is intentionally pin-only: running windows are
       reached through the workspace overview, never appended as task buttons.
       Legacy keeps its historical running-app launcher and the final "+" item. */
    int spatial = wm_get_layout_mode() == WM_LAYOUT_SPATIAL_MINIMAL;
    char run[8][16];
    int nrun = spatial ? 0 : wm_user_tags(run, 8);
    char sig[SIDE_N * 17]; int sp = 0;
    for (int i = 0; i < dock.n; i++) {
        sp = scpy(sig, sp, dock.name[i], (int)sizeof sig);
        sig[sp++] = '|'; sig[sp] = 0;
    }
    if (!spatial) {
        sig[sp++] = '#'; sig[sp] = 0;
        for (int i = 0; i < nrun; i++) if (dock_find(&dock, run[i]) < 0) {
            sp = scpy(sig, sp, run[i], (int)sizeof sig);
            sig[sp++] = '|'; sig[sp] = 0;
        }
    }
    if (!force && streq(sig, side_sig)) return;
    scpy(side_sig, 0, sig, (int)sizeof side_sig);
    side_n = 0;
    for (int i = 0; i < dock.n && side_n < SIDE_N - (spatial ? 0 : 1); i++) {
        scpy(side_names[side_n], 0, dock.name[i], 16);
        side[side_n] = (wm_side_item_t){ dock_title(dock.name[i]) ? dock_title(dock.name[i]) : side_names[side_n], side_names[side_n], dock_color(dock.name[i]), dock_icon(dock.name[i]), 0, 0,
                                         "правая кнопка - открепить", 0 };
        if (side[side_n].icon == ICON_TERMINAL && dock.name[i][0] >= 'a' && dock.name[i][0] <= 'z') side[side_n].badge = (char)(dock.name[i][0] - 32);
        side_n++;
    }
    if (!spatial) {
        for (int i = 0; i < nrun && side_n < SIDE_N - 1; i++) {
            if (dock_find(&dock, run[i]) >= 0 || !dock_valid_name(run[i])) continue;
            int dup = 0;
            for (int k = 0; k < side_n; k++) if (streq(side_names[k], run[i])) dup = 1;
            if (dup) continue;
            scpy(side_names[side_n], 0, run[i], 16);
            side[side_n] = (wm_side_item_t){ dock_title(run[i]) ? dock_title(run[i]) : side_names[side_n], side_names[side_n], dock_color(run[i]), dock_icon(run[i]), 0, 1,
                                             "правая кнопка - закрепить", 0 };
            if (side[side_n].icon == ICON_TERMINAL && run[i][0] >= 'a' && run[i][0] <= 'z') side[side_n].badge = (char)(run[i][0] - 32);
            side_n++;
        }
        side[side_n++] = (wm_side_item_t){ "Добавить программу", "", TH_TEXT_DIM, ICON_FOLDER, 1, 0, "откроет /bin в Проводнике", 0 };
    }
    wm_side_set(side, side_n);
}

static void dock_save(void) {
    char text[256];
    unsigned n = dock_format(&dock, text, sizeof text);
    if (explorer_write(DOCK_PATH, text, n) != 0) uart_print("[KIRA DISPLAY] quick-launch list not saved: disk queue is busy.\n");
}

static void dock_toggle(const char* name) {
    if (dock_find(&dock, name) >= 0) dock_remove(&dock, name);
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

static void on_launcher(void) {
    launcher_query[0] = 0; launcher_status[0] = 0; launcher_selected = 0; launcher_refresh();
    open_app(APP_LAUNCHER);
}

static void on_side_alt(int index) {
    if (index < 0 || index >= side_n || side[index].bottom) return;
    char name[16]; scpy(name, 0, side_names[index], 16);
    dock_toggle(name);
}

static void on_tab(int i) {
    if (i == 0) { wm_minimize_all(); return; }
    if (i == 1) { on_launcher(); return; }
    if (i > 0 && i < (int)(sizeof app_of_tab / sizeof app_of_tab[0])) open_app(app_of_tab[i]);
}

static void on_workspace_changed(int workspace) {
    (void)workspace;
    if (w_con >= 0) wm_invalidate(w_con);
    draw_mind(); draw_monitor(); draw_files(); draw_settings(); draw_launcher();
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

#ifndef KIRA_HOST_TEST
static int system_link_state(void) {
    netdev_t* dev = net_dev();
    if (!dev) return -2;
    if (!dev->link) return -1;
    int state = dev->link(dev);
    return state > 0 ? 1 : state == 0 ? 0 : -1;
}
#endif

static void update_system_io(void) {
#ifndef KIRA_HOST_TEST
    int volume = audio_running() ? audio_master(-1) : -1;
    wm_set_system_io(system_link_state(), volume);
#else
    wm_set_system_io(-1, -1);
#endif
}

static void on_system_volume(int percent) {
#ifndef KIRA_HOST_TEST
    if (audio_running()) wm_set_system_io(system_link_state(), audio_master(percent));
#else
    (void)percent;
#endif
}

static void update_mind(void) {
    uint32_t v = mind_sys_version();
    if (v == mv_ver) return;
    mv_ver = v;
    mind_sys_view(&mv);
    if (mv.t) {
        wm_set_meters(mv.busy, (int)(mv.cores > 64 ? 64 : mv.cores), (int)mv.mem_used, mv.health);
        int band = mv.health < 25 ? 2 : mv.health < 50 ? 1 : 0;
        if (mind_health_band >= 0 && band > mind_health_band)
            (void)display_notify("KIRA Mind", band == 2 ? "Критическое снижение здоровья системы" : "Здоровье системы ниже 50%", band == 2 ? WM_NOTIFY_CRITICAL : WM_NOTIFY_WARNING);
        else if (mind_health_band > 0 && band == 0)
            (void)display_notify("KIRA Mind", "Показатель здоровья системы восстановился", WM_NOTIFY_INFO);
        mind_health_band = band;
    }
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
static struct { uint32_t par; uint64_t cost[2]; uint32_t n[2]; uint32_t probe; uint64_t next_ns; uint64_t gap_ns; uint32_t confirm; } cs;   /* gap_ns: пауза между пробами, растёт вдвое, пока выбор подтверждается */

void display_compositor_stats(uint32_t* par, uint32_t* ns_par, uint32_t* ns_ser) {
    *par = cs.par; *ns_par = (uint32_t)cs.cost[1]; *ns_ser = (uint32_t)cs.cost[0];
}

static int comp_mode(uint64_t now) {
    if (!kpar_helpers()) return 0;
    if (!cs.gap_ns) cs.gap_ns = 5000000000ULL;
    if (!cs.probe && now >= cs.next_ns) { cs.probe = 16; cs.next_ns = now + cs.gap_ns; }
    return cs.probe ? !cs.par : (int)cs.par;
}

static void comp_note(int mode, uint64_t ns, uint64_t px) {
    if (px < 20000) return;                                   /* мелкие кадры не показательны: накладные расходы */
    uint64_t c = ns * 1000 / px;
    cs.cost[mode] = cs.n[mode] ? (cs.cost[mode] * 7 + c) / 8 : c;
    cs.n[mode]++;
    if (cs.probe && --cs.probe == 0 && cs.n[0] >= 8 && cs.n[1] >= 8) {
        uint32_t other = !cs.par;
        if (cs.cost[other] * 100 < cs.cost[cs.par] * 80) {       /* запас 20%: шум замера не качает выбор туда-сюда */
            if (++cs.confirm >= 2) {                               /* другой способ должен выиграть две пробы подряд */
                cs.confirm = 0; cs.par = other; cs.gap_ns = 60000000000ULL;
                uart_print(other ? "[KIRA DISPLAY] compositor: all cores are cheaper here (" : "[KIRA DISPLAY] compositor: one core is cheaper here (");
                uart_print_dec(cs.cost[other]); uart_print(" vs "); uart_print_dec(cs.cost[!other]);
                uart_print(" ns per 1000 pixels) - switching.\n");
            } else cs.next_ns = 0;                                 /* повторить пробу сразу */
        } else {                                                   /* выбор подтверждён: проб становится вдвое реже (до 10 минут), каждая проба стоит кадров в худшем способе */
            cs.confirm = 0;
            if (cs.gap_ns < 600000000000ULL) cs.gap_ns *= 2;
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
        notification_poll();              // bounded cross-thread notification queue -> WM on display owner thread
        files_launch_poll();
        launcher_poll();               // результат ранее принятого запроса

        {                                 // Клавиатура принадлежит верхнему окну ввода.
            input_event_t ke;
            for (;;) {
                /* Re-evaluate ownership after every key: Escape may close Launcher. */
                int lown = w_launcher >= 0 && wm_visible(w_launcher) && wm_top() == w_launcher;
                int fown = !lown && w_files >= 0 && wm_visible(w_files) && wm_top() == w_files;
                __atomic_store_n(&launcher_focus, lown, __ATOMIC_RELEASE);
                __atomic_store_n(&files_focus, fown, __ATOMIC_RELEASE);
                if (!lown && !fown) break;
                if (!input_pop(&ke)) break;
                if (lown) launcher_key(&ke);
                else if (ke.type == INPUT_KEY_DOWN && !(ke.key >= KEY_LSHIFT && ke.key <= KEY_CAPS)) explorer_key(ke.key, ke.ch);
            }
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
            else if (hk == KEY_WORKSPACE_PREV) wm_workspace_switch((wm_workspace_current() + wm_workspace_count() - 1) % wm_workspace_count());
            else if (hk == KEY_WORKSPACE_NEXT) wm_workspace_switch((wm_workspace_current() + 1) % wm_workspace_count());
            else if (hk == KEY_WORKSPACE_MOVE_PREV) wm_window_move_workspace(top, (wm_workspace_current() + wm_workspace_count() - 1) % wm_workspace_count());
            else if (hk == KEY_WORKSPACE_MOVE_NEXT) wm_window_move_workspace(top, (wm_workspace_current() + 1) % wm_workspace_count());
            else if (hk == KEY_NOTIFICATION_TOGGLE) wm_notification_center_toggle();
        }

        mouse_event_t me;
        while (input_pop_mouse(&me)) {
            st.mouse_events++;
            if (me.abs) {                                  /* планшет: точка экрана, а не сдвиг */
                surface_t* fs = fb_surface();
                int cx, cy; wm_cursor(&cx, &cy);
                int tx = (int)((uint32_t)me.ax * (uint32_t)(fs->w - 1) / 32767u), ty = (int)((uint32_t)me.ay * (uint32_t)(fs->h - 1) / 32767u);
                wm_mouse(tx - cx, ty - cy, me.buttons);
            } else wm_mouse(me.dx, me.dy, me.buttons);
        }

        int y0 = 0, y1 = 0;
        unsigned n = fbcon_update(&y0, &y1);
        if (n && y1 > y0) wm_invalidate_rows(w_con, y0, y1);

        update_mind();
        update_files();

        uint64_t now0 = clock_ns();
        if (now0 - clock_t0 >= 500000000ULL) { clock_t0 = now0; update_clock(); update_system_io(); }
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

int display_init(unsigned long mbi) {
    /* кадровый буфер от загрузчика (multiboot), иначе - virtio-gpu по PCI (QEMU virt на ARM64, машины без VBE) */
    int rc = mbi ? fb_init(mbi) : -1;
    if (rc != 0 && virtio_gpu_probe() == 0) rc = 0;
    if (rc != 0 && platform_fb_probe() == 0) rc = 0;            /* ARM: ramfb, simple-framebuffer из дерева устройств */
    if (rc != 0) return rc;
    notify_head = notify_tail = 0;
    wm_ops_t ops = { wm_alloc, on_visibility, on_input, on_launch, on_resize, on_tab, on_side_alt, on_launcher, on_workspace_changed, on_system_volume };
/* Spatial Minimal is the production desktop, not a preview-only code path. */
    wm_init_mode(fb_surface(), &ops, WM_LAYOUT_SPATIAL_MINIMAL);
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
#ifdef KIRA_SPATIAL_PREVIEW
    /* Keep the initial Spatial canvas quiet; the terminal remains reachable from the navigation rail. */
    wm_set_visible(w_con, 0);
#else
    wm_raise(w_con);
#endif
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
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    uart_print("[KIRA DISPLAY] Shell online: Spatial Minimal desktop, glass panels, Dock, workspaces and contextual system controls.\n");
    return 0;
}

void display_set_fx(int level) { fx_req = level < 0 ? 0 : level > 3 ? 3 : level; }

void display_get_stats(uint64_t* loops, uint64_t* late, uint64_t* worst_gap_ns) {
    *loops = st.frames; *late = st.late; *worst_gap_ns = st.worst_gap_ns;
    st.worst_gap_ns = 0;                       /* худший промежуток - за период между вызовами */
}

void display_get_render_stats(uint64_t* frames, uint64_t* composed, uint64_t* pixels, uint64_t* busy_ns, uint64_t* worst_busy_ns, uint64_t* last_present_ns) {
    if (frames) *frames = st.frames;
    if (composed) *composed = st.composed;
    if (pixels) *pixels = st.pixels;
    if (busy_ns) *busy_ns = st.busy_ns;
    if (worst_busy_ns) *worst_busy_ns = st.worst_busy_ns;
    if (last_present_ns) *last_present_ns = st.present_ns;
}

void display_request_files(const char* left, const char* right) {
    uint64_t fl = spin_lock_irqsave(&rq_lk);
    scpy(rq_l, 0, left ? left : "", FM_PATH); scpy(rq_r, 0, right ? right : "", FM_PATH);
    __atomic_store_n(&rq_pending, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&rq_lk, fl);
}

int display_kbd_owner(void) { return __atomic_load_n(&files_focus, __ATOMIC_ACQUIRE) || __atomic_load_n(&launcher_focus, __ATOMIC_ACQUIRE); }

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
    uart_print(" per second on average), avg dirty pixels/composed frame "); uart_print_dec(st.composed ? st.pixels / st.composed : 0);
    uart_print(", late cycles "); uart_print_dec(st.late);
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
