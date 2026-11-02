// Хост-прогон потока дисплея целиком: журнал ядра -> терминал, приложения ядра (Разум, Монитор, Проводник,
// Настройки), окна Ring 3, разделы и боковая панель, живой фон -> present.
// Заглушки: fb (экран в памяти), sched (захват точки входа), clock (виртуальные часы), pmm (malloc), снимки Разума и
// файловой системы (правдоподобные данные). Проводник - настоящая логика панелей fm.c над файловой системой в памяти.
// Результат - картинки того, что увидит пользователь по VNC: /tmp/kira_gfx/*.ppm
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/stat.h>
#include "display.h"
#include "fb.h"
#include "klog.h"
#include "sched.h"
#include "clock.h"
#include "timer.h"
#include "proc.h"
#include "progs.h"
#include "input.h"
#include "wm.h"
#include "fbcon.h"
#include "mind_sys.h"
#include "explorer.h"
#include "fm.h"
#include "rtc.h"
#include "apps.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int fails;

static uint64_t fake_ns;
uint64_t clock_ns(void) { return fake_ns += 1500; }       // каждый вызов "занимает" 1.5 мкс
int clock_ready(void) { return 1; }

static surface_t screen;
static int presents_full, presents_rect;
int fb_init(unsigned long m) { (void)m; return 0; }
int fb_available(void) { return 1; }
surface_t* fb_surface(void) { return &screen; }
static fb_info_t finfo = { 0xFD000000ULL, 1024, 768, 4096, 32, 1, 0x1000000, 3145728 };
const fb_info_t* fb_get_info(void) { return &finfo; }
void fb_present(void) { presents_full++; }
void fb_present_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; presents_rect++; }
void fb_frame_done(void) {}
int virtio_gpu_probe(void) { return -1; }
int platform_fb_probe(void) { return -1; }

uint64_t pmm_alloc_contiguous(uint32_t pages) { return (uint64_t)(uintptr_t)aligned_alloc(4096, (size_t)pages * 4096); }
int pmm_free_contiguous(uint64_t p, uint32_t pages) { (void)pages; free((void*)(uintptr_t)p); return 0; }
uint32_t pmm_get_total_memory_kb(void) { return 2048 * 1024; }

void uart_print(const char* s) { while (*s) klog_putc(*s++); }
void uart_print_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); uart_print(b); }
void uart_print_hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); uart_print(b); }

static void (*entry)(void*);
static jmp_buf out;
static int frames_left;
thread_t* sched_create(const char* n, void (*e)(void*), void* a, thread_prio_t p) { (void)a; (void)p; if (!strcmp(n, "display")) entry = e; static thread_t t; return &t; }
void sched_sleep_ticks(uint32_t t) { fake_ns += (uint64_t)t * (1000000000ULL / TIMER_HZ); if (--frames_left <= 0) longjmp(out, 1); }
unsigned sched_snapshot(thread_info_t* o, unsigned max) { (void)o; return max < 23 ? max : 23; }
unsigned sched_cpu_count(void) { return 4; }

int experience_enabled(void) { return 1; }
const char* platform_irq_name(void) { return "IOAPIC"; }
void gfx_server_run(void) {}
static int gin_n;
void gfx_on_input(int w, int t, int x, int y, unsigned b) { (void)w; (void)t; (void)x; (void)y; (void)b; gin_n++; }
void gfx_on_close(int w) { (void)w; }
static char spawned[16];
void prog_request(const char* n) { snprintf(spawned, sizeof spawned, "%s", n); }
void prog_request_file(const char* n, const char* f) { (void)f; snprintf(spawned, sizeof spawned, "%s", n); }
static char con_cmd[64]; static int con_n;
int console_request(const char* l) { snprintf(con_cmd, sizeof con_cmd, "%s", l); con_n++; return 0; }

/* ---- часы: пятница, 30 октября 2026, 13:05 ---- */
int rtc_now(int32_t tz, rtc_time_t* t) { (void)tz; *t = (rtc_time_t){ 2026, 10, 30, 13, 5, 0 }; return 0; }

/* ---- Разум: правдоподобный снимок ---- */
static mind_view_t MV;
static uint32_t mver = 1;
static int req_mode = -1, req_fx = -1, tele; static int32_t req_tz = 99999;
void mind_sys_view(mind_view_t* v) { *v = MV; }
uint32_t mind_sys_version(void) { return mver; }
int32_t mind_sys_tz(void) { return MV.tz_min; }
int mind_sys_rtc_local(void) { return 0; }
void mind_sys_request_rtc_local(int on) { (void)on; }
int32_t mind_sys_local_offset(void) { return MV.tz_min; }
int mind_sys_telemetry(void) { return tele; }
void mind_sys_set_telemetry(int on) { tele = on; }
void mind_sys_request_mode(int m) { req_mode = m; }
void mind_sys_request_fx(int f) { req_fx = f; }
void mind_sys_request_tz(int32_t m) { req_tz = m; }

static void fill_mind(void) {
    mind_view_t* v = &MV;
    memset(v, 0, sizeof *v);
    v->t = 2711; v->lifetime_s = 3 * 86400 + 5 * 3600 + 14 * 60;
    v->health = 86; v->c_resp = 94; v->c_room = 92; v->c_bal = 63; v->c_stab = 100;
    strcpy(v->mood, "сосредоточена");
    v->mode = 1; v->fx = 3; v->fx_user = MIND_FX_AUTO; v->quantum_ms = 30;
    v->cores = 4; v->load = 41; v->mem_used = 23; v->runnable = 3;
    uint8_t b[4] = { 72, 38, 21, 33 }; memcpy(v->busy, b, 4);
    v->frames_per_s = 60; v->late_per_s = 1;
    static const int32_t cur[8] = { 4100, 75, 2300, 100, 41000, 120000, 0, 1830000 };
    static const int32_t mean[8] = { 2600, 60, 2200, 80, 39000, 90000, 10, 1500000 };
    static const int32_t z[8] = { 12, 8, 3, 6, 4, 9, -2, 31 };
    for (int k = 0; k < MIND_SIG_N; k++) {
        v->cur_x100[k] = cur[k]; v->mean_x100[k] = mean[k]; v->z_x10[k] = z[k]; v->learned[k] = k == 6 ? 40 : 9000;
        for (int i = 0; i < MIND_HIST; i++) {
            int base = 20 + k * 7, wave = (int)(18.0 * __builtin_sin((i + k * 9) * 0.21) + 9.0 * __builtin_sin(i * 0.77 + k));
            int val = base + wave + (k == 7 && i > 50 ? 30 : 0);
            v->hist[k][i] = (uint8_t)(val < 0 ? 0 : val > 100 ? 100 : val);
        }
    }
    for (int i = 0; i < MIND_HIST; i++) v->health_hist[i] = (uint8_t)(84 + (int)(5 * __builtin_sin(i * 0.3)) - (i > 40 && i < 46 ? 14 : 0));
    v->hpos = 0;
    v->decisions = 47; v->anomalies = 9; v->kills = 0; v->tune_trials = 6; v->tune_wins = 2; v->activity = 1;
    static const struct { uint32_t t; uint8_t kind, applied; const char* text; } J[] = {
        { 2709, MJ_ANOMALY, 1, "необычно: вызовы 18300 /с при норме 15000 (отклонение в 3 раза больше обычного разброса)" },
        { 2702, MJ_FOCUS, 1, "фокус: cube3d - квант x1,5, пока окно впереди" },
        { 2650, MJ_TUNE_KEEP, 1, "квант 30 мс лучше прежнего 40 мс на 12% - оставляю и запоминаю для 4 ядер" },
        { 2610, MJ_TUNE_TRY, 1, "пробую квант 30 мс вместо 40 мс: сравню переключения и ожидание потоков" },
        { 2433, MJ_LIMIT, 1, "экран опаздывает (7% кадров): ограничиваю фоновую burn (98% ядра) до 50%" },
        { 2440, MJ_RESTORE, 1, "экран успевает 10 с: burn снова без ограничения" },
        { 1800, MJ_FX, 1, "у экрана есть запас времени: анимация фона - уровень 3" },
        { 2, MJ_LEARN, 1, "опыт загружен: 7 из 8 норм выучены, квант 40 мс, решений за жизнь 41" },
    };
    v->nj = 8;
    for (unsigned i = 0; i < 8; i++) { v->j[i].t = J[i].t; v->j[i].kind = J[i].kind; v->j[i].applied = J[i].applied; snprintf(v->j[i].text, MIND_TEXT, "%s", J[i].text); }
    v->tz_min = 180;
    static const struct { uint32_t pid, th, heap, cpu, sysc, w, lim; const char* n; uint8_t fl, fo; } P[] = {
        { 7, 1, 2048, 612, 18300, 6, 100, "cube3d", 0, 1 }, { 5, 1, 512, 488, 120, 4, 50, "burn", 0, 0 },
        { 9, 4, 1024, 133, 2400, 4, 100, "mt", 0, 0 }, { 3, 1, 1536, 41, 60, 4, 100, "paint", 0, 0 },
        { 11, 1, 256, 12, 9, 4, 100, "ipc", 0, 0 },
    };
    v->nproc = 5;
    for (unsigned i = 0; i < 5; i++) {
        v->p[i].pid = P[i].pid; v->p[i].threads = P[i].th; v->p[i].heap_kb = P[i].heap; v->p[i].cpu_x10 = P[i].cpu; v->p[i].sysc = P[i].sysc;
        v->p[i].weight = P[i].w; v->p[i].limit = P[i].lim; snprintf(v->p[i].name, 16, "%s", P[i].n); v->p[i].flagged = P[i].fl; v->p[i].focus = P[i].fo;
    }
}

/* ---- Проводник: настоящая логика панелей (fm.c) над файловой системой в памяти ---- */
typedef struct { char path[FM_PATH]; int dir; uint8_t* data; uint64_t size; int used; } mf_t;
static mf_t MFS[256];
static mf_t* mfind(const char* p) { for (int i = 0; i < 256; i++) if (MFS[i].used && !strcmp(MFS[i].path, p)) return &MFS[i]; return 0; }
static mf_t* mnew(const char* p) { for (int i = 0; i < 256; i++) if (!MFS[i].used) { memset(&MFS[i], 0, sizeof MFS[i]); MFS[i].used = 1; snprintf(MFS[i].path, FM_PATH, "%s", p); return &MFS[i]; } return 0; }
static void mput(const char* p, int dir, uint64_t size, const char* text) {
    mf_t* m = mnew(p); m->dir = dir; m->size = size; if (dir) return;
    m->data = calloc(size + 1, 1); if (text) memcpy(m->data, text, strlen(text) < size ? strlen(text) : size);
}
static int m_list(const char* path, void (*emit)(void*, const char*, int, uint64_t), void* ctx) {
    if (strcmp(path, "/") && !mfind(path)) return -2;
    size_t pl = strlen(path); if (pl == 1) pl = 0;
    for (int i = 0; i < 256; i++) {
        const char* q = MFS[i].path;
        if (!MFS[i].used || strncmp(q, path, pl) || q[pl] != '/' || strchr(q + pl + 1, '/')) continue;
        emit(ctx, q + pl + 1, MFS[i].dir, MFS[i].size);
    }
    return 0;
}
static int m_stat(const char* q, int* dir, uint64_t* size) { if (!strcmp(q, "/")) { *dir = 1; *size = 0; return 0; } mf_t* m = mfind(q); if (!m) return -2; *dir = m->dir; *size = m->size; return 0; }
static int64_t m_read(const char* q, uint64_t off, void* b, size_t n) { mf_t* m = mfind(q); if (!m || m->dir || off > m->size) return -2; if (off + n > m->size) n = (size_t)(m->size - off); memcpy(b, m->data + off, n); return (int64_t)n; }
static int m_write(const char* q, const void* d, size_t n) { mf_t* m = mfind(q); if (!m) m = mnew(q); if (!m) return -6; free(m->data); m->data = malloc(n + 1); memcpy(m->data, d, n); m->size = n; return 0; }
static int m_mkdir(const char* q) { if (mfind(q)) return -3; mf_t* m = mnew(q); m->dir = 1; return 0; }
static int m_unlink(const char* q) { mf_t* m = mfind(q); if (!m) return -2; free(m->data); m->used = 0; return 0; }
static void* m_alloc(size_t n) { return malloc(n); }
static void m_release(void* q) { free(q); }
static int m_tick(fm_t* f) { (void)f; return 0; }
static const char* m_err(int e) { return e == -3 ? "уже есть" : "ошибка"; }
static void m_log(const char* l) { (void)l; }
static const fm_io_t IO = { m_list, m_stat, m_read, m_write, m_mkdir, m_unlink, m_alloc, m_release, m_tick, m_err, m_log };

static exp_view_t EV;
static int clicks;
static void ex_publish(void) { EV.mounted = 1; EV.total_kb = 8192; EV.free_kb = 7790; EV.files = 18; EV.version++; }
static char written_path[64], written[256]; static int writes;
void explorer_goto(int panel, const char* path) { int pn = panel < 0 ? EV.fm.active : panel; fm_goto(&EV.fm, &IO, pn, path); EV.fm.active = pn; ex_publish(); }
void explorer_key(int key, int ch) { clicks++; fm_key(&EV.fm, &IO, key, ch); ex_publish(); }
void explorer_mouse(int kind, int panel, int row) { clicks++; fm_mouse(&EV.fm, &IO, kind, panel, row); ex_publish(); }
void explorer_page(int panel, int dir) { fm_page(&EV.fm, panel, dir); ex_publish(); }
int explorer_write(const char* path, const void* data, unsigned len) {
    snprintf(written_path, sizeof written_path, "%s", path); memcpy(written, data, len); written[len] = 0; writes++;
    memcpy(EV.dock_text, data, len); EV.dock_len = (int32_t)len;      /* как будто записалось на диск */
    return 0;
}
uint32_t vfs_generation(void) { return 0; }
void explorer_init(void) {
    static const struct { const char* path; int dir; uint32_t size; } S[] = {
        { "/bin", 1, 0 }, { "/data", 1, 0 }, { "/var", 1, 0 }, { "/readme.txt", 0, 412 }, { "/pic.png", 0, 3300 },
        { "/bin/plasma", 0, 9144 }, { "/bin/paint", 0, 17096 }, { "/bin/cube3d", 0, 22056 }, { "/bin/burn", 0, 8808 },
        { "/bin/mt", 0, 13248 }, { "/bin/ipc", 0, 9192 }, { "/bin/fpu", 0, 9120 }, { "/wasm", 1, 0 }, { "/wasm/wtest.wasm", 0, 5120 },
        { "/data/paint", 1, 0 }, { "/data/paint/canvas.raw", 0, 153600 }, { "/data/notes.txt", 0, 96 },
        { "/var/life.log", 0, 3810 }, { "/var/mind.dat", 0, 556 }, { "/var/mind", 1, 0 },
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) mput(S[i].path, S[i].dir, S[i].size, i == 3 ? "Kira OS" : 0);
    fm_init(&EV.fm, &IO, "/", "/data");
    EV.dock_loaded = 1; EV.dock_len = -1; ex_publish();
}
void explorer_refresh(void) { fm_reload(&EV.fm, &IO); ex_publish(); }
uint32_t explorer_version(void) { return EV.version; }
void explorer_view(exp_view_t* o) { *o = EV; }

static void save_ppm(const char* path) {
    FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", screen.w, screen.h);
    for (int i = 0; i < screen.w * screen.h; i++) { uint32_t p = screen.px[i]; unsigned char c[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p}; fwrite(c, 1, 3, f); }
    fclose(f);
}
static void run(int n) { frames_left = n; if (setjmp(out) == 0) entry(0); }
static void klog_str(const char* s) { while (*s) klog_putc(*s++); }
static void move_to(int x, int y, unsigned b) { int cx, cy; wm_cursor(&cx, &cy); wm_mouse(x - cx, y - cy, b); }
static void click(int x, int y) { move_to(x, y, 0); wm_mouse(0, 0, 1); wm_mouse(0, 0, 0); }
static void click_tab(int i) { int x, y; wm_tab_center(i, &x, &y); click(x, y); }
static void click_side(int i) { int x, y; wm_side_center(i, &x, &y); click(x, y); }
static void click_in(int id, int x, int y) { int ox, oy, s; wm_content_origin(id, &ox, &oy, &s); click(ox + x, oy + y); }

static char outdir[64] = "/tmp/kira_gfx";
static void save_shot(const char* name) { char p[128]; snprintf(p, sizeof p, "%s/%s", outdir, name); save_ppm(p); }

int main(int argc, char** argv) {
    int W = argc > 2 ? atoi(argv[1]) : 1024, H = argc > 2 ? atoi(argv[2]) : 768;   /* разрешение: аргументы */
    if (argc > 2) snprintf(outdir, sizeof outdir, "/tmp/kira_gfx/%dx%d", W, H);
    mkdir("/tmp/kira_gfx", 0777); mkdir(outdir, 0777);
    uint32_t* px = calloc((size_t)W * H, 4);
    screen.px = px; screen.w = W; screen.h = H; screen.pitch = W;
    finfo.width = (uint32_t)W; finfo.height = (uint32_t)H; finfo.pitch_bytes = (uint32_t)W * 4;
    fill_mind();

    klog_str("[KIRA OS] Kernel core starting...\n"
             "[KIRA PMM] Usable RAM per memory map: 2047 MB; allocatable: 2040 MB.\n"
             "[KIRA SCHED] Preemptive scheduler online: quantum and per-process weight set by the mind\n"
             "[KIRA SMP] 4 cores online, all equal: a failing core is isolated, the system keeps running\n"
             "[KIRA FS] KiraFS mounted on virtio-blk: 8 MB, 18 objects\n"
             "[KIRA MIND] опыт загружен: 7 из 8 норм выучены, квант 40 мс, решений за жизнь 41\n"
             "[KIRA OS] Ready. Type 'help' and press Enter.\n"
             "кира> test ipc\n"
             "[TEST] ports: send/recv between processes ... PASS\n"
             "[TEST] shared memory + futex ... PASS\n"
             "[TEST] crash isolation: core 2 FAIL injected -> isolated, OK\n"
             "[USER cube3d] frame 1200, 60 fps\n"
             "[KIRA MIND] фокус: cube3d - квант x1,5, пока окно впереди\n"
             "WARN: heap 81% (cube3d)\n"
             "кира> ai why\n");

    if (display_init(0x10000) != 0) { printf("display_init failed\n"); return 1; }
    run(30);
    CHECK(presents_rect > 10);
    save_shot("terminal.ppm");

    /* левая панель: закреплённые по умолчанию (файла /var/dock нет) и «+» */
    { int x, y; CHECK(wm_side_center(0, &x, &y) == 0 && wm_side_center(5, &x, &y) == 0 && wm_side_center(6, &x, &y) != 0); }

    /* разделы: Разум */
    click_tab(2);
    run(4);
    int w_mind = wm_find("Mind");
    CHECK(w_mind >= 0 && wm_top() == w_mind);
    save_shot("mind.ppm");

    /* Монитор - разделом */
    click_tab(4);
    run(4);
    int w_mon = wm_find("Monitor");
    CHECK(w_mon >= 0 && wm_top() == w_mon);
    save_shot("monitor.ppm");

    /* Проводник: две панели, мышь и клавиши */
    click_tab(3);
    run(3);
    int w_files = wm_find("Files");
    CHECK(w_files >= 0 && wm_top() == w_files);
    exp_view_t e; explorer_view(&e);
    const int RX0 = 70, RX1 = 70 + 395 - 10, RY = 38 + 46 + 9, KY = 520 - 12;
    CHECK(!strcmp(e.fm.p[0].path, "/") && !strcmp(e.fm.p[1].path, "/data") && e.fm.p[0].n == 5 && e.fm.p[0].hidden == 1);   /* var скрыт, bin виден (список приложений) */
    CHECK(!strcmp(e.fm.p[0].e[0].name, "bin") && !strcmp(e.fm.p[0].e[1].name, "data") && e.fm.p[0].e[1].dir && !strcmp(e.fm.p[0].e[2].name, "wasm"));
    save_shot("files-open.ppm");
    click_in(w_files, RX0, RY + 18 * 2); run(1);                              /* щелчок: курсор на wasm */
    explorer_view(&e); CHECK(e.fm.active == 0 && e.fm.p[0].cur == 2);
    click_in(w_files, RX1, RY + 18 * 1); run(1);                              /* щелчок по правой панели: она активна */
    explorer_view(&e); CHECK(e.fm.active == 1 && e.fm.p[1].cur == 1);
    click_in(w_files, RX0, RY + 18); click_in(w_files, RX0, RY + 18); run(1);  /* двойной щелчок по data: левая панель входит */
    explorer_view(&e); CHECK(!strcmp(e.fm.p[0].path, "/data") && e.fm.active == 0);
    /* клавиши: Tab, F7 - каталог */
    input_push_scancode(0x0F); input_push_scancode(0x8F); run(2);
    explorer_view(&e); CHECK(e.fm.active == 1);
    input_push_scancode(0x41); input_push_scancode(0xC1); run(2);
    explorer_view(&e); CHECK(e.fm.mode == FM_MODE_INPUT);
    save_shot("files-dialog.ppm");
    input_push_scancode(0x31); input_push_scancode(0xB1); input_push_scancode(0x1C); input_push_scancode(0x9C); run(2);   /* "n" Enter */
    explorer_view(&e); CHECK(e.fm.mode == FM_MODE_NORMAL && mfind("/data/n") && !strcmp(e.fm.p[1].e[e.fm.p[1].cur].name, "n"));
    click_in(w_files, 4 * 800 / 9 + 30, KY); run(1);                         /* ячейка F5: обе панели в /data - отказ */
    explorer_view(&e); CHECK(e.fm.msg_err);
    explorer_goto(0, "/"); run(1);
    explorer_view(&e);
    int dd = -1; for (uint32_t i = 0; i < e.fm.p[1].n; i++) if (!strcmp(e.fm.p[1].e[i].name, "notes.txt")) dd = (int)i;
    CHECK(dd > 0);
    {
        int ox, oy, sc; wm_content_origin(w_files, &ox, &oy, &sc);
        move_to(ox + RX1, oy + RY + 18 * dd, 0); wm_mouse(0, 0, 1); wm_mouse(-120, 4, 1); wm_mouse(-60, 4, 1); save_shot("files-drag.ppm"); wm_mouse(0, 0, 0);
    }
    run(1);
    CHECK(mfind("/notes.txt") && mfind("/data/notes.txt"));
    explorer_view(&e); CHECK(!e.fm.msg_err && strstr(e.fm.msg, "Скопировано"));
    /* F8: подтверждение и удаление (в правой панели - notes.txt) */
    input_push_scancode(0x42); input_push_scancode(0xC2); run(2);
    explorer_view(&e); CHECK(e.fm.mode == FM_MODE_CONFIRM);
    save_shot("files-confirm.ppm");
    input_push_scancode(0x1C); input_push_scancode(0x9C); run(2);
    explorer_view(&e); CHECK(e.fm.mode == FM_MODE_NORMAL && !mfind("/data/notes.txt"));
    click_in(w_files, 8 * 800 / 9 + 30, KY); run(1);                         /* F9: служебные файлы видны */
    explorer_view(&e); CHECK(e.fm.show_hidden && e.fm.p[0].n == 7);
    save_shot("files.ppm");
    click_in(w_files, 8 * 800 / 9 + 30, KY); run(1);
    /* программы /bin: двойной щелчок запускает, F4 закрепляет */
    explorer_goto(0, "/bin"); run(2);
    explorer_view(&e);
    int burn = -1, ipc = -1;
    for (uint32_t i = 0; i < e.fm.p[0].n; i++) { if (!strcmp(e.fm.p[0].e[i].name, "burn")) burn = (int)i; if (!strcmp(e.fm.p[0].e[i].name, "ipc")) ipc = (int)i; }
    CHECK(burn > 0 && ipc > 0 && e.fm.p[0].e[burn].kind == FM_KIND_PROG);
    con_n = 0; click_in(w_files, RX0, RY + 18 * burn); run(1); click_in(w_files, RX0, RY + 18 * burn); run(2);
    CHECK(con_n == 1 && !strcmp(con_cmd, "burn"));
    run(2); con_n = 0; click_in(w_files, RX0, RY + 18 * ipc); run(1); click_in(w_files, RX0, RY + 18 * ipc); run(2);
    CHECK(con_n == 1 && !strcmp(con_cmd, "test ipc"));
    run(2); click_in(w_files, RX0, RY + 18 * burn); run(1);
    move_to(200, 600, 0); run(2);
    save_shot("files_run.ppm");
    writes = 0; click_in(w_files, 3 * 800 / 9 + 30, KY); run(2);              /* F4: закрепить burn */
    CHECK(writes == 1 && !strcmp(written_path, "/var/dock") && !strcmp(written, "plasma\npaint\ncube3d\nedit\ndrvman\nburn\n"));
    { int x, y; CHECK(wm_side_center(5, &x, &y) == 0 && wm_side_center(6, &x, &y) == 0 && wm_side_center(7, &x, &y) != 0); }
    con_n = 0; click_side(5); CHECK(con_n == 1 && !strcmp(con_cmd, "burn"));
    run(1);
    { int x, y; wm_side_center(0, &x, &y); move_to(x, y, 0); wm_mouse(0, 0, 2); wm_mouse(0, 0, 0); }   /* plasma - открепить */
    run(1);
    CHECK(writes == 2 && !strcmp(written, "paint\ncube3d\nedit\ndrvman\nburn\n"));
    /* «+» открывает Проводник с /bin */
    click_side(5);
    run(2);
    CHECK(wm_top() == w_files);
    explorer_view(&e); CHECK(!strcmp(e.fm.p[e.fm.active].path, "/bin"));
    move_to(200, 600, 0); run(1);

    /* Настройки: щелчки доходят до Разума */
    click_tab(5);
    run(3);
    int w_set = wm_find("Settings");
    CHECK(w_set >= 0 && wm_top() == w_set);
    click_in(w_set, 500 - 20, 12 + 15 + 10);                         /* «действует» уже выбран; щёлкнем «наблюдает» */
    int ox, oy, s; wm_content_origin(w_set, &ox, &oy, &s);
    for (int x = 200; x < 500 && req_mode < 0; x += 10) click(ox + x, oy + 12 + 15 + 10);
    CHECK(req_mode == 0 || req_mode == 1);
    req_fx = -1; for (int x = 300; x < 500 && req_fx != 2; x += 6) click(ox + x, oy + 12 + 64 + 15 + 10);
    CHECK(req_fx == 2);
    req_tz = 99999; for (int x = 380; x < 500 && req_tz == 99999; x += 6) click(ox + x, oy + 12 + 128 + 15 + 10);
    CHECK(req_tz == 120 || req_tz == 240);
    MV.mode = 1; MV.fx_user = MIND_FX_AUTO; MV.tz_min = 180; mver++;
    run(3);
    save_shot("settings.ppm");

    /* «Главная»: все окна убраны, виден живой мозг; подсказка у значка */
    click_tab(0);
    CHECK(wm_top() == -1);
    MV.activity = 3; mver++;
    { int x, y; wm_side_center(1, &x, &y); move_to(x, y, 0); }
    run(40);
    save_shot("home.ppm");

    /* программы Ring 3: значок запускает (команда в терминал), окно приложения рядом с Разумом */
    con_n = 0; click_side(1);
    CHECK(con_n == 1 && !strcmp(con_cmd, "cube3d"));
    static uint32_t pa[320 * 240];
    for (int j = 0; j < 240; j++) for (int i = 0; i < 320; i++) pa[j * 320 + i] = ((i * 160 / 320) << 16) | ((j * 120 / 240 + 40) << 8) | 0xA0;
    click_tab(2);
    int uf = wm_create_user("cube3d", WM_AUTO, WM_AUTO, 320, 240, pa);
    wm_set_tag(uf, "cube3d");
    CHECK(uf >= 0 && wm_top() == uf);
    gin_n = 0; { int x, y, sc; wm_content_origin(uf, &x, &y, &sc); move_to(x + 30, y + 30, 0); }
    CHECK(gin_n > 0);
    /* незакреплённая идущая программа появляется ниже черты; правая кнопка - закрепить */
    static uint32_t pm[200 * 120];
    int um = wm_create_user("mt", WM_AUTO, WM_AUTO, 200, 120, pm);
    run(2);
    { int x, y; CHECK(wm_side_center(5, &x, &y) == 0); move_to(x, y, 0); wm_mouse(0, 0, 2); wm_mouse(0, 0, 0); }
    run(1);
    CHECK(!strcmp(written, "paint\ncube3d\nedit\ndrvman\nburn\nmt\n"));
    wm_destroy(um);
    run(4);
    save_shot("apps.ppm");
    input_push_scancode(0x57); input_push_scancode(0xD7);          // F11: на весь экран
    run(3);
    CHECK(wm_fullscreen(wm_top()));
    input_push_scancode(0x57); input_push_scancode(0xD7);
    run(3);
    CHECK(!wm_fullscreen(wm_top()));
    wm_destroy(uf);

    /* терминал растягивается уголком: больше колонок и строк */
    {
        int con = wm_find("Console");
        wm_set_visible(con, 1); wm_raise(con); run(2);
        int cols0, rows0, cols1, rows1, wx, wy, w0, h0, cx_, cy_;
        fbcon_get_size(&cols0, &rows0);
        wm_window_pos(con, &wx, &wy); wm_content_size(con, &w0, &h0);
        wm_cursor(&cx_, &cy_);
        wm_mouse(wx + w0 + 2 * WM_PAD - 8 - cx_, wy + h0 + WM_HEAD + WM_PAD - 8 - cy_, 0);
        wm_mouse(0, 0, 1); wm_mouse(-160, -80, 1); wm_mouse(0, 0, 0);
        fbcon_get_size(&cols1, &rows1);
        CHECK(cols1 < cols0 && rows1 < rows0);
        klog_putc('\f'); klog_str("кира> ready\n");
        run(3);
    }
    display_print_info();
    printf("spawn %s, explorer clicks %d\n", spawned, clicks);
    printf("%s\n", fails ? "DISPLAY PREVIEW FAILED" : "display preview: terminal, mind, monitor, files, settings, home, apps, resize passed");
    return fails != 0;
}
