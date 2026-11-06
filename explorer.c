#include "explorer.h"
#include "input.h"
#include "kfs.h"
#include "vfs.h"
#include "sched.h"
#include "spinlock.h"
#include "cpu.h"
#include "uart.h"
#include "heap.h"
#include "clock.h"

/* Рабочий поток Проводника. Состояние панелей (fm_t) принадлежит только ему; оболочка шлёт события через очередь и
   забирает снимок V. Диск здесь можно ждать сколько нужно: поток дисплея в это время рисует кадры. */

#define QN 64
enum { EV_KEY, EV_MOUSE, EV_PAGE, EV_GOTO };
typedef struct { int type, a, b, c; char path[FM_PATH]; } ev_t;
typedef struct { char path[64]; char data[256]; unsigned len; int used; } wreq_t;

static spinlock_t lk = SPINLOCK_INIT;
static thread_t* worker;
static int waiting;
static int req_refresh = 1;
static ev_t q[QN];
static unsigned qh, qt;
static exp_view_t V;                 /* опубликованный снимок (под lk) */
static exp_view_t W;                 /* рабочая копия потока */
static wreq_t wq[4];
static int dock_read, started;
static uint64_t last_pub_ns;

static void scpy(char* d, const char* s, int cap) { int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

/* ---- слои файловых систем для fm.c ---- */

typedef struct { void (*emit)(void*, const char*, int, uint64_t); void* ctx; } kl_ctx_t;
static int kl_cb(void* c, const char* name, const kfs_stat_t* st) {
    kl_ctx_t* k = (kl_ctx_t*)c;
    k->emit(k->ctx, name, st->type == KFS_DIR, st->size);
    return 0;
}
static int kio_list(const char* path, void (*emit)(void*, const char*, int, uint64_t), void* ctx) {
    kl_ctx_t k = { emit, ctx };
    return vfs_list(path, kl_cb, &k);
}
static int kio_stat(const char* path, int* dir, uint64_t* size) {
    kfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r) return r;
    *dir = st.type == KFS_DIR; *size = st.size;
    return 0;
}
static int64_t kio_read(const char* p, uint64_t off, void* b, size_t n) { return vfs_read(p, off, b, n); }
static int kio_write(const char* p, const void* d, size_t n) { return vfs_write(p, d, n); }
static int kio_mkdir(const char* p) { return vfs_mkdir(p); }
static int kio_unlink(const char* p) { return vfs_unlink(p); }
static void* kio_alloc(size_t n) { return kmalloc(n); }
static void kio_release(void* p) { kfree(p); }
static void kio_log(const char* l) { uart_print(l); }

static void publish_locked(void) {
    W.version++;
    __builtin_memcpy(&V, &W, sizeof V);
}

/* Между порциями копирования: показать ход (не чаще 25 раз в секунду) и узнать, не нажали ли Esc. */
static int kio_tick(fm_t* f) {
    (void)f;
    uint64_t now = clock_ns();
    int cancel = 0;
    uint64_t fl = spin_lock_irqsave(&lk);
    unsigned w = qt;
    for (unsigned r = qt; r != qh; r++) {                           /* Esc из очереди забираем, остальное остаётся */
        if (q[r % QN].type == EV_KEY && q[r % QN].a == KEY_ESC) { cancel = 1; continue; }
        if (w != r) q[w % QN] = q[r % QN];
        w++;
    }
    qh = w;
    if (cancel || !now || now - last_pub_ns >= 40000000ULL) { last_pub_ns = now; publish_locked(); }
    spin_unlock_irqrestore(&lk, fl);
    return cancel;
}

static const fm_io_t KIO = { kio_list, kio_stat, kio_read, kio_write, kio_mkdir, kio_unlink, kio_alloc, kio_release, kio_tick, kfs_strerror, kio_log };

static void rebuild_info(void) {
    W.mounted = vfs_any();
    static vfs_info_t mi[VFS_MAX];
    int nm = vfs_mounts(mi, VFS_MAX);
    W.nvol = W.rovol = 0;
    for (int k = 0; k < nm && k < EXP_VOLS; k++) {
        scpy(W.vol[k].name, mi[k].name, sizeof W.vol[k].name);
        scpy(W.vol[k].label, mi[k].label[0] ? mi[k].label : mi[k].desc, sizeof W.vol[k].label);
        W.vol[k].total_kb = (uint32_t)(mi[k].total_kb > 0xFFFFFFFFu ? 0xFFFFFFFFu : mi[k].total_kb);
        W.vol[k].read_only = (uint8_t)(mi[k].read_only != 0);
        if (mi[k].read_only) W.rovol++;
        W.nvol++;
    }
    W.total_kb = W.free_kb = W.files = 0;
    if (W.mounted && kfs_mounted()) {
        kfs_info_t fi; kfs_info(&fi);
        W.total_kb = fi.total_blocks * (KFS_BLOCK / 1024); W.free_kb = fi.free_blocks * (KFS_BLOCK / 1024);
        W.files = fi.inodes - fi.free_inodes;
    }
}

static int pop_event(ev_t* e) {                                     /* под lk */
    if (qt == qh) return 0;
    *e = q[qt % QN]; qt++;
    return 1;
}

static void worker_main(void* arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&lk);
        while (qt == qh && !req_refresh && !wq[0].used && !wq[1].used && !wq[2].used && !wq[3].used && dock_read) {
            waiting = 1;
            sched_wait_and_unlock(&lk, ~0ULL);
            irq_restore(f);
            f = spin_lock_irqsave(&lk);
        }
        waiting = 0;
        static ev_t ev;
        int have = pop_event(&ev), refresh = req_refresh;
        req_refresh = 0;
        wreq_t w[4];
        for (int i = 0; i < 4; i++) { w[i] = wq[i]; wq[i].used = 0; }
        spin_unlock_irqrestore(&lk, f);

        for (int i = 0; i < 4; i++)                                  /* записи файлов - по порядку прихода */
            if (w[i].used && vfs_any()) {
                int rc = vfs_write(w[i].path, w[i].data, w[i].len);
                if (rc == KFS_ERR_NOENT) { vfs_mkdir("/var"); rc = vfs_write(w[i].path, w[i].data, w[i].len); }
                if (rc) { uart_print("[KIRA FILES] cannot write "); uart_print(w[i].path); uart_print("\n"); }
                refresh = 1;
            }
        if (!dock_read) {                                            /* список быстрого доступа */
            dock_read = 1;
            W.dock_loaded = 1; W.dock_len = -1;
            if (kfs_mounted()) {
                int64_t n = kfs_read("/var/dock", 0, W.dock_text, sizeof W.dock_text - 1);
                if (n >= 0) { W.dock_len = (int32_t)n; W.dock_text[n] = 0; }
            }
            refresh = 1;
        }
        if (!started) { started = 1; fm_init(&W.fm, &KIO, "/", "/"); refresh = 1; }
        int info = refresh;
        if (have) {
            if (ev.type == EV_KEY) {
                fm_key(&W.fm, &KIO, ev.a, ev.b);
                if (ev.a == KEY_ENTER || ev.a == KEY_F5 || ev.a == KEY_F6 || ev.a == KEY_F7 || ev.a == KEY_DELETE) info = 1;
            } else if (ev.type == EV_MOUSE) { fm_mouse(&W.fm, &KIO, ev.a, ev.b, ev.c); info = 1; }
            else if (ev.type == EV_PAGE) fm_page(&W.fm, ev.b, ev.c);
            else if (ev.type == EV_GOTO) {
                int pn = ev.b < 0 ? W.fm.active : ev.b;
                int r = fm_goto(&W.fm, &KIO, pn, ev.path);
                if (r < 0) { scpy(W.fm.msg, "Не открыть каталог ", sizeof W.fm.msg); W.fm.msg_err = 1; }
                else W.fm.active = pn;
            }
        }
        if (refresh) fm_reload(&W.fm, &KIO);
        if (info) rebuild_info();
        f = spin_lock_irqsave(&lk);
        publish_locked();
        spin_unlock_irqrestore(&lk, f);
    }
}

static void kick(void) {
    if (waiting && worker) { waiting = 0; sched_wake_waiter(worker); }
}

void explorer_init(void) {
    worker = sched_create("explorer", worker_main, 0, PRIO_NORMAL);
    if (!worker) uart_print("[KIRA FILES] cannot start explorer thread.\n");
}

static void push(int type, int a, int b, int c, const char* path) {
    uint64_t f = spin_lock_irqsave(&lk);
    if (qh - qt < QN) {
        ev_t* e = &q[qh % QN];
        e->type = type; e->a = a; e->b = b; e->c = c;
        scpy(e->path, path ? path : "", FM_PATH);
        qh++;
    }
    kick();
    spin_unlock_irqrestore(&lk, f);
}

void explorer_key(int key, int ch) { push(EV_KEY, key, ch, 0, 0); }
void explorer_mouse(int kind, int panel, int row) { push(EV_MOUSE, kind, panel, row, 0); }
void explorer_page(int panel, int dir) { push(EV_PAGE, 0, panel, dir, 0); }
void explorer_goto(int panel, const char* path) { push(EV_GOTO, 0, panel, 0, path); }

int explorer_write(const char* path, const void* data, unsigned len) {
    if (len > sizeof wq[0].data) return -1;
    int rc = -1;
    uint64_t f = spin_lock_irqsave(&lk);
    for (int i = 0; i < 4; i++) if (!wq[i].used) {
        scpy(wq[i].path, path, sizeof wq[i].path);
        for (unsigned k = 0; k < len; k++) wq[i].data[k] = ((const char*)data)[k];
        wq[i].len = len; wq[i].used = 1; rc = 0;
        break;
    }
    kick();
    spin_unlock_irqrestore(&lk, f);
    return rc;
}

void explorer_refresh(void) {
    uint64_t f = spin_lock_irqsave(&lk);
    req_refresh = 1;
    kick();
    spin_unlock_irqrestore(&lk, f);
}

uint32_t explorer_version(void) { return __atomic_load_n(&V.version, __ATOMIC_ACQUIRE); }

void explorer_view(exp_view_t* out) {
    uint64_t f = spin_lock_irqsave(&lk);
    __builtin_memcpy(out, &V, sizeof V);
    spin_unlock_irqrestore(&lk, f);
}
