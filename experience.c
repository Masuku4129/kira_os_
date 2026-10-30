#include "experience.h"
#include "store.h"
#include "blk.h"
#include "immune.h"
#include "mind_sys.h"
#include "heap.h"
#include "uart.h"

#define SAVE_PERIOD_S 30

static store_t st;
static experience_t xp;
static int enabled;
static uint64_t base_total_s;
static uint64_t session_s;
static uint64_t last_saved_s;
static uint32_t save_errors;

int experience_enabled(void) { return enabled; }

static void gather(void) {
    /* Краткая сводка Разума в секторе опыта (полная модель - в /var/mind.dat). */
    mind_view_t* v = (mind_view_t*)kmalloc(sizeof(mind_view_t));
    if (v) {
        mind_sys_view(v);
        xp.load_ema_x100 = v->mean_x100[MS_LOAD] > 0 ? (uint32_t)v->mean_x100[MS_LOAD] : 0;
        xp.mem_ema_x100 = v->mean_x100[MS_MEM] > 0 ? (uint32_t)v->mean_x100[MS_MEM] : 0;
        xp.peak_stress = v->health;                 /* поле прежнего формата: последнее здоровье */
        xp.anomalies = v->anomalies;
        kfree(v);
    }
    xp.curiosity = 0;                               /* поле прежнего формата, больше не используется */
    xp.last_session_s = (uint32_t)session_s;
    xp.total_uptime_s = base_total_s + session_s;

    uint32_t c = immune_quarantine_count();
    xp.qcount = 0;
    for (uint32_t i = 0; i < c && xp.qcount < EXP_QMAX; i++) {
        const immune_quarantine_entry_t* q = immune_quarantine_get(i);
        if (!q) continue;
        exp_quarantine_t* d = &xp.q[xp.qcount++];
        for (int k = 0; k < 32; k++) d->digest[k] = q->digest[k];
        d->size = q->size;
        d->verdict = (uint32_t)q->verdict;
        for (int k = 0; k < 32; k++) d->reason[k] = q->reason[k];
    }
}

int experience_save_now(void) {
    if (!enabled) return -1;
    gather();
    int r = store_commit(&st, &xp);
    if (r != 0) {
        save_errors++;
        uart_print("[KIRA EXPERIENCE] Disk write failed, code "); uart_print_dec((uint64_t)(-r)); uart_print(".\n");
        return r;
    }
    last_saved_s = session_s;
    return 0;
}

void experience_init(void) {
    blk_probe();
    if (!blk_primary()) {
        uart_print(blk_count() ? "[KIRA EXPERIENCE] No Kira disk: experience lives in RAM only (see 'disk', 'format').\n"
                               : "[KIRA EXPERIENCE] No disk: experience lives in RAM only (run_qemu.sh attaches kira.disk).\n");
        return;
    }
    experience_attach();
}

/* Подключить хранилище опыта к системному диску (при загрузке и после format на новом диске). */
void experience_attach(void) {
    blk_dev_t* d = blk_primary();
    if (!d) return;
    enabled = 0;
    int r = store_mount(&st, d, &xp);
    if (r < 0) { uart_print("[KIRA EXPERIENCE] Disk read failed: persistence disabled.\n"); return; }
    enabled = 1;

    if (r == 1) {
        for (uint32_t i = 0; i < xp.qcount && i < EXP_QMAX; i++)
            immune_quarantine_import(xp.q[i].digest, xp.q[i].size, xp.q[i].verdict, xp.q[i].reason);
        base_total_s = xp.total_uptime_s;
        uart_print("[KIRA EXPERIENCE] Restored from disk (generation ");
        uart_print_dec(st.generation);
        uart_print("): previous boots ");
        uart_print_dec(xp.boot_count);
        uart_print(", lived ");
        uart_print_dec(xp.total_uptime_s);
        uart_print(" s in total, last session ");
        uart_print_dec(xp.last_session_s);
        uart_print(" s, quarantine entries ");
        uart_print_dec(xp.qcount);
        uart_print(".\n");
    } else {
        uart_print("[KIRA EXPERIENCE] Disk found, no saved experience: this is the organism's first life.\n");
    }
    xp.boot_count++;
    experience_save_now();
}

void experience_tick(uint64_t session_seconds) {
    session_s = session_seconds;
    if (enabled && session_s - last_saved_s >= SAVE_PERIOD_S) experience_save_now();
}

void experience_print(void) {
    uart_print("\n[EXPERIENCE] ");
    if (!enabled) { uart_print("no disk (RAM only)\n\n"); return; }
    gather();
    uart_print("boot #"); uart_print_dec(xp.boot_count);
    uart_print(", total uptime "); uart_print_dec(xp.total_uptime_s);
    uart_print(" s, load norm "); uart_print_dec(xp.load_ema_x100 / 100);
    uart_print("%, memory norm "); uart_print_dec(xp.mem_ema_x100 / 100);
    uart_print("%, anomalies "); uart_print_dec(xp.anomalies);
    uart_print(", saved generation "); uart_print_dec(st.generation);
    uart_print(", write errors "); uart_print_dec(save_errors);
    uart_print("\n\n");
}
