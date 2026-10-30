#include "explorer.h"

/* ---------------- чистая часть: дерево ---------------- */

#define DIR_MAX   EXP_DIR_MAX
#define DEPTH_MAX 10
#define OPEN_MAX  64

typedef struct { char name[EXP_NAME]; uint8_t dir; uint64_t size; } ent_t;
typedef struct { ent_t e[DIR_MAX]; int n, over; } dirbuf_t;

static dirbuf_t lvl[DEPTH_MAX];        /* по буферу на уровень: обход рекурсивный, но без больших кадров стека */

static void emit_ent(void* ctx, const char* name, int dir, uint64_t size) {
    dirbuf_t* b = (dirbuf_t*)ctx;
    if (b->n >= DIR_MAX) { b->over = 1; return; }      /* больше DIR_MAX имён: показываем часть, отмечаем усечение */
    ent_t* e = &b->e[b->n++];
    int i = 0; while (name[i] && i < EXP_NAME - 1) { e->name[i] = name[i]; i++; } e->name[i] = 0;
    e->dir = (uint8_t)(dir != 0); e->size = size;
}

static int name_cmp(const ent_t* a, const ent_t* b) {
    if (a->dir != b->dir) return a->dir ? -1 : 1;                   /* каталоги первыми */
    const unsigned char* x = (const unsigned char*)a->name; const unsigned char* y = (const unsigned char*)b->name;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}

static int join(char* out, const char* dir, const char* name) {
    int n = 0;
    while (dir[n] && n < 127) { out[n] = dir[n]; n++; }
    if (n == 0 || out[n - 1] != '/') { if (n >= 127) return -1; out[n++] = '/'; }
    for (int i = 0; name[i]; i++) { if (n >= 127) return -1; out[n++] = name[i]; }
    out[n] = 0;
    return 0;
}

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

typedef struct {
    exp_list_fn list; char (*open)[128]; int nopen;
    exp_row_t* rows; char (*paths)[128]; uint32_t max, n, trunc;
} build_t;

static void walk(build_t* B, const char* path, int depth) {
    if (depth >= DEPTH_MAX) return;
    dirbuf_t* b = &lvl[depth];
    b->n = 0; b->over = 0;
    if (B->list(path, emit_ent, b) < 0) return;
    if (b->over) B->trunc = 1;
    for (int i = 1; i < b->n; i++) {                                  /* вставками: в каталоге не больше 64 имён */
        ent_t t = b->e[i]; int j = i - 1;
        while (j >= 0 && name_cmp(&b->e[j], &t) > 0) { b->e[j + 1] = b->e[j]; j--; }
        b->e[j + 1] = t;
    }
    for (int i = 0; i < b->n; i++) {
        if (B->n >= B->max) { B->trunc = 1; return; }
        const ent_t* e = &b->e[i];
        exp_row_t* r = &B->rows[B->n];
        char full[128];
        if (join(full, path, e->name) < 0) continue;
        r->depth = (uint8_t)depth; r->dir = e->dir; r->size = e->size; r->open = 0; r->sel = 0;
        for (int k = 0; k < EXP_NAME; k++) r->name[k] = e->name[k];
        for (int k = 0; k < 128; k++) B->paths[B->n][k] = full[k];
        B->n++;
        if (e->dir) {
            for (int k = 0; k < B->nopen; k++) if (same(B->open[k], full)) { r->open = 1; break; }
            if (r->open) {
                walk(B, full, depth + 1);
                b = &lvl[depth];                                    /* буфер уровня цел: вложенный обход пишет в свой */
            }
        }
    }
}

uint32_t exp_build(exp_list_fn list, char open[][128], int nopen, exp_row_t* rows, char (*paths)[128], uint32_t max, uint32_t* truncated) {
    build_t B = { list, open, nopen, rows, paths, max, 0, 0 };
    walk(&B, "/", 0);
    if (truncated) *truncated = B.trunc;
    return B.n;
}

#ifndef KIRA_HOST_TEST
/* ---------------- ядро: рабочий поток ---------------- */
#include "kfs.h"
#include "vfs.h"
#include "sched.h"
#include "spinlock.h"
#include "cpu.h"
#include "uart.h"

static spinlock_t lk = SPINLOCK_INIT;
static thread_t* worker;
static int waiting;
static int req_refresh = 1, req_row = -1;
static uint32_t req_ver;
static exp_view_t V;                 /* опубликованный снимок (под lk) */
static exp_view_t W;                 /* рабочая копия потока */
static char paths[EXP_ROWS][128];    /* полные пути строк рабочей копии */
static char open_set[OPEN_MAX][128];
static int nopen;
static char sel[128];
static char req_expand[128];
typedef struct { char path[64]; char data[256]; unsigned len; int used; } wreq_t;
static wreq_t wq[4];
static int dock_read;

typedef struct { void (*emit)(void*, const char*, int, uint64_t); void* ctx; } kl_ctx_t;
static int kl_cb(void* c, const char* name, const kfs_stat_t* st) {
    kl_ctx_t* k = (kl_ctx_t*)c;
    k->emit(k->ctx, name, st->type == KFS_DIR, st->size);
    return 0;
}
static int kfs_lister(const char* path, void (*emit)(void*, const char*, int, uint64_t), void* ctx) {
    kl_ctx_t k = { emit, ctx };
    return vfs_list(path, kl_cb, &k);
}

static void scpy(char* d, const char* s, int cap) { int i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static int is_prefix_dir(const char* p, const char* dir) {   /* p лежит внутри dir */
    int i = 0; while (dir[i] && dir[i] == p[i]) i++;
    return !dir[i] && p[i] == '/';
}

static void toggle_open(const char* path) {
    for (int k = 0; k < nopen; k++)
        if (same(open_set[k], path)) {                              /* свернуть вместе с вложенными */
            int j = 0;
            for (int m = 0; m < nopen; m++) if (m != k && !is_prefix_dir(open_set[m], path)) { if (j != m) scpy(open_set[j], open_set[m], 128); j++; }
            nopen = j;
            return;
        }
    if (nopen < OPEN_MAX) scpy(open_set[nopen++], path, 128);
}

static void read_preview(void) {
    W.prev_len = 0; W.prev_bin = 0; W.sel_size = 0;
    scpy(W.sel_path, sel, sizeof W.sel_path);
    if (!sel[0]) return;
    kfs_stat_t st;
    if (vfs_stat(sel, &st) != 0) { W.prev_len = -1; return; }
    W.sel_size = st.size;
    int64_t n = vfs_read(sel, 0, W.prev, EXP_PREVIEW - 1);
    if (n < 0) { W.prev_len = -1; return; }
    W.prev_len = (int32_t)n;
    W.prev[n] = 0;
    for (int64_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)W.prev[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\t' && c != '\r')) { W.prev_bin = 1; break; }
    }
}

static void rebuild(void) {
    W.mounted = vfs_any();
    static vfs_info_t mi[VFS_MAX];
    int nm = vfs_mounts(mi, VFS_MAX);
    W.nvol = 0;
    for (int k = 0; k < nm && k < EXP_VOLS; k++) {
        scpy(W.vol[k].name, mi[k].name, sizeof W.vol[k].name);
        scpy(W.vol[k].label, mi[k].label[0] ? mi[k].label : mi[k].desc, sizeof W.vol[k].label);
        W.vol[k].total_kb = (uint32_t)(mi[k].total_kb > 0xFFFFFFFFu ? 0xFFFFFFFFu : mi[k].total_kb);
        W.nvol++;
    }
    W.nrows = 0; W.truncated = 0;
    if (W.mounted) {
        W.nrows = exp_build(kfs_lister, open_set, nopen, W.row, paths, EXP_ROWS, &W.truncated);
        W.total_kb = W.free_kb = W.files = 0;
        if (kfs_mounted()) {
            kfs_info_t fi; kfs_info(&fi);
            W.total_kb = fi.total_blocks * (KFS_BLOCK / 1024); W.free_kb = fi.free_blocks * (KFS_BLOCK / 1024);
            W.files = fi.inodes - fi.free_inodes;
        }
        for (uint32_t i = 0; i < W.nrows; i++) W.row[i].sel = sel[0] && same(paths[i], sel);
    }
}

static void worker_main(void* arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&lk);
        while (!req_refresh && req_row < 0 && !req_expand[0] && !wq[0].used && !wq[1].used && !wq[2].used && !wq[3].used && dock_read) {
            waiting = 1;
            sched_wait_and_unlock(&lk, ~0ULL);
            irq_restore(f);
            f = spin_lock_irqsave(&lk);
        }
        waiting = 0;
        int row = req_row, refresh = req_refresh; uint32_t ver = req_ver;
        req_row = -1; req_refresh = 0;
        char expand[128]; scpy(expand, req_expand, sizeof expand); req_expand[0] = 0;
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
        if (expand[0]) {
            int open = 0; for (int k = 0; k < nopen; k++) if (same(open_set[k], expand)) open = 1;
            if (!open) toggle_open(expand);
            refresh = 1;
        }

        int need_preview = refresh;
        if (row >= 0 && ver == W.version && (uint32_t)row < W.nrows) {   /* щелчок по устаревшему снимку пропускаем */
            if (W.row[row].dir) toggle_open(paths[row]);
            else { scpy(sel, paths[row], sizeof sel); need_preview = 1; }
        }
        rebuild();
        if (need_preview) read_preview();
        W.version++;
        f = spin_lock_irqsave(&lk);
        __builtin_memcpy(&V, &W, sizeof V);
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

void explorer_click(uint32_t version, int row) {
    uint64_t f = spin_lock_irqsave(&lk);
    req_row = row; req_ver = version;
    kick();
    spin_unlock_irqrestore(&lk, f);
}

void explorer_expand(const char* path) {
    uint64_t f = spin_lock_irqsave(&lk);
    scpy(req_expand, path, sizeof req_expand);
    kick();
    spin_unlock_irqrestore(&lk, f);
}

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
#endif
