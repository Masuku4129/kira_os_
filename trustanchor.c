#include "trustanchor.h"
#include "uart.h"

#if defined(__x86_64__)
#include "boottrust.h"
#include "kfs.h"
#include "rollback.h"
#include "tpm2.h"
#include "tpmtis.h"
#include "timer.h"

static int tpm_ok;                         /* TIS найден */
static int release_pending;                /* загрузчик сверил выпуск со счётчиком: его надо поднять после хорошей загрузки */
static int release_done;
static int anchor_state = -1;              /* последнее известное: 1 - якорь есть, 0 - нет, -1 - TPM недоступен */
static uint64_t anchor_val;
static uint64_t last_sync_tick;
static int rolled_back;                    /* на этом запуске обнаружен откат диска и безопасный режим не снят */

static int xfer(void* ctx, const uint8_t* c, size_t n, uint8_t* r, size_t* rl) { (void)ctx; return tpmtis_submit(c, n, r, rl); }

static void hexrc(uint32_t rc) { uart_print(" (TPM rc "); uart_print_hex(rc); uart_print(")"); }

/* Прочитать якорь. Состояние: 1 есть, 0 нет (ячейки нет или не записана), -1 ошибка (TPM или формат). */
static int anchor_read(void) {
    uint8_t b[RB_ANCHOR_LEN]; uint32_t rc = 0;
    if (!tpm_ok) { anchor_state = -1; return -1; }
    int e = tpm2_nv_read(xfer, 0, KIRA_NV_DISK_ANCHOR, b, RB_ANCHOR_LEN, &rc);
    if (e == TPM2_E_ABSENT || e == TPM2_E_EMPTY) { anchor_state = 0; anchor_val = 0; return 0; }
    if (e != TPM2_OK || rb_anchor_decode(b, &anchor_val) != RB_OK) {
        uart_print("[KIRA FS] anchor: cannot read the disk anchor from TPM ("); uart_print(e == TPM2_OK ? "bad format" : tpm2_strerror(e)); uart_print(")");
        if (e == TPM2_E_RC) hexrc(rc);
        uart_print("\n");
        anchor_state = -1;
        return -1;
    }
    anchor_state = 1;
    return 1;
}

/* Записать якорь (создать ячейку, если её нет). 0 - успех. */
static int anchor_write(uint64_t epoch) {
    uint8_t b[RB_ANCHOR_LEN]; uint32_t rc = 0;
    rb_anchor_encode(epoch, b);
    int e = tpm2_nv_write(xfer, 0, KIRA_NV_DISK_ANCHOR, b, RB_ANCHOR_LEN, &rc);
    if (e == TPM2_E_ABSENT) {
        e = tpm2_nv_define(xfer, 0, KIRA_NV_DISK_ANCHOR, TPM2_NV_ATTR_ORDINARY, RB_ANCHOR_LEN, &rc);
        if (e == TPM2_OK) e = tpm2_nv_write(xfer, 0, KIRA_NV_DISK_ANCHOR, b, RB_ANCHOR_LEN, &rc);
    }
    if (e != TPM2_OK) {
        uart_print("[KIRA FS] anchor: cannot write the disk anchor to TPM ("); uart_print(tpm2_strerror(e)); uart_print(")");
        if (e == TPM2_E_RC) hexrc(rc);
        uart_print("\n");
        return -1;
    }
    anchor_state = 1; anchor_val = epoch;
    return 0;
}

/* Закрепить эпоху: том пишет эпоху на носитель, и только потом значение уходит в TPM (якорь никогда не выше диска). force: записать даже без изменений. */
static int sync_anchor(const char* why) {
    uint64_t e = 0;
    if (!tpm_ok || rolled_back || !kfs_mounted()) return -1;
    if (kfs_epoch_sync(&e) != 0) { uart_print("[KIRA FS] anchor: cannot persist the disk epoch\n"); return -1; }
    if (anchor_state < 0) (void)anchor_read();
    if (!rb_anchor_should_write(anchor_state, anchor_val, e)) { last_sync_tick = timer_get_ticks(); return 0; }
    if (anchor_write(e) != 0) return -1;
    last_sync_tick = timer_get_ticks();
    uart_print("[KIRA FS] anchor: disk epoch "); uart_print_dec(e); uart_print(" saved in TPM ("); uart_print(why); uart_print(")\n");
    return 0;
}

void trustanchor_init(void) {
    const boottrust_t* t = boottrust_get();
    tpm_ok = tpmtis_probe() == 0;
    release_pending = tpm_ok && (t->flags & KBI_TRUST_ROLLBACK_TPM) && (t->flags & KBI_TRUST_SIG_VERIFIED);
    if (!tpm_ok) {
        uart_print("[KIRA BOOT] rollback: no TPM driver device: image rollback is checked only against the embedded minimum release; disk rollback is not detected\n");
        return;
    }
    uart_print("[KIRA BOOT] rollback: TPM 2.0 (TIS) found");
    if (release_pending) {
        uint64_t v = 0; uint32_t rc = 0;
        int e = tpm2_nv_counter_read(xfer, 0, KIRA_NV_RELEASE_COUNTER, &v, &rc);
        if (e == TPM2_E_EMPTY) { v = 0; e = TPM2_OK; }
        if (e == TPM2_OK) { uart_print("; release "); uart_print_dec(t->image_version); uart_print(", TPM counter "); uart_print_dec(v); }
        else { uart_print("; release counter unreadable ("); uart_print(tpm2_strerror(e)); uart_print(")"); release_pending = 0; }
    } else if (t->flags & KBI_TRUST_SIG_VERIFIED) {
        uart_print("; the loader did not check the release counter");
    } else {
        uart_print("; boot is not signed, the release counter is not used");
    }
    uart_print("\n");
}

static void raise_release_counter(void) {
    const boottrust_t* t = boottrust_get();
    uint64_t v = 0; uint32_t rc = 0;
    int e = tpm2_nv_counter_read(xfer, 0, KIRA_NV_RELEASE_COUNTER, &v, &rc);
    if (e == TPM2_E_EMPTY) { v = 0; e = TPM2_OK; }
    if (e != TPM2_OK) { uart_print("[KIRA BOOT] rollback: cannot read the release counter: "); uart_print(tpm2_strerror(e)); uart_print("\n"); return; }
    uint32_t steps = rb_release_raise_steps(t->image_version, v);
    for (uint32_t i = 0; i < steps; i++) {
        e = tpm2_nv_increment(xfer, 0, KIRA_NV_RELEASE_COUNTER, &rc);
        if (e != TPM2_OK) { uart_print("[KIRA BOOT] rollback: counter increment failed: "); uart_print(tpm2_strerror(e)); hexrc(rc); uart_print("\n"); return; }
    }
    if (tpm2_nv_counter_read(xfer, 0, KIRA_NV_RELEASE_COUNTER, &v, &rc) != TPM2_OK) return;
    uart_print("[KIRA BOOT] rollback: good boot confirmed, TPM counter is now "); uart_print_dec(v);
    if (v > t->image_version) { uart_print(" (above release "); uart_print_dec(t->image_version); uart_print(": this TPM already had counters, releases below it will be refused)"); }
    else if (v < t->image_version) { uart_print(" (release "); uart_print_dec(t->image_version); uart_print(" needs more boots to reach the floor)"); }
    uart_print("\n");
}

void trustanchor_fs_mounted(void) {
    if (!kfs_mounted()) return;
    if (!tpm_ok) {
        uart_print("[KIRA FS] anchor: no TPM, a rolled-back disk copy would not be detected\n");
        return;
    }
    (void)anchor_read();
    uint64_t e = kfs_epoch();
    rb_disk_verdict_t v = rb_disk_check(e, anchor_state, anchor_val);
    if (v == RB_DISK_ROLLBACK) {
        rolled_back = 1;
        kfs_set_readonly(1);
        uart_print("[KIRA FS] rollback detected: disk epoch "); uart_print_dec(e); uart_print(" is older than the TPM anchor "); uart_print_dec(anchor_val);
        uart_print(": an old copy of the disk was put in place. The volume is READ-ONLY; 'fsanchor accept' confirms it (operator), see docs/SECURE_BOOT.md\n");
    } else if (v == RB_DISK_NO_ANCHOR) {
        uart_print("[KIRA FS] anchor: none in TPM yet, disk epoch "); uart_print_dec(e); uart_print(" is taken as the starting point\n");
        (void)anchor_write(e);
    } else if (v == RB_DISK_OK) {
        uart_print("[KIRA FS] anchor: disk epoch "); uart_print_dec(e); uart_print(" is not older than the TPM anchor "); uart_print_dec(anchor_val); uart_print(": ok\n");
    }
    last_sync_tick = timer_get_ticks();
}

void trustanchor_tick(void) {
    uint64_t now = timer_get_ticks();
    if (release_pending && !release_done && now >= (uint64_t)TRUSTANCHOR_GOOD_BOOT_S * TIMER_HZ) {
        release_done = 1;
        raise_release_counter();
    }
    if (tpm_ok && !rolled_back && kfs_mounted() && now - last_sync_tick >= (uint64_t)TRUSTANCHOR_ANCHOR_EVERY_S * TIMER_HZ && kfs_epoch_dirty())
        (void)sync_anchor("periodic");
}

void trustanchor_shutdown(void) {
    if (tpm_ok && !rolled_back && kfs_mounted()) (void)sync_anchor("shutdown");
}

void trustanchor_command(const char* a) {
    while (a && *a == ' ') a++;
    if (!a || !*a || (a[0] == 's' && a[1] == 't')) {
        uart_print("fsanchor: TPM "); uart_print(tpm_ok ? "present" : "absent"); uart_print(", volume "); uart_print(kfs_mounted() ? "mounted" : "not mounted");
        if (kfs_mounted()) { uart_print(", disk epoch "); uart_print_dec(kfs_epoch()); uart_print(kfs_epoch_dirty() ? " (changes not yet confirmed)" : ""); }
        if (tpm_ok) { (void)anchor_read(); if (anchor_state == 1) { uart_print(", TPM anchor "); uart_print_dec(anchor_val); } else uart_print(", TPM anchor none"); }
        uart_print(kfs_readonly() ? ", READ-ONLY (rollback)" : "");
        uart_print("\n  fsanchor sync - persist epoch and anchor now;  fsanchor accept - operator confirms the current disk (after a restore from backup)\n");
    } else if (a[0] == 's' && a[1] == 'y') {
        if (!tpm_ok) { uart_print("fsanchor: no TPM\n"); return; }
        if (rolled_back) { uart_print("fsanchor: rollback detected, first 'fsanchor accept' or restore the right disk\n"); return; }
        if (sync_anchor("command") == 0) uart_print("fsanchor: done\n");
    } else if (a[0] == 'a') {
        if (!tpm_ok || !kfs_mounted()) { uart_print("fsanchor: no TPM or no volume\n"); return; }
        uint64_t e = kfs_epoch();
        kfs_set_readonly(0);
        if (anchor_write(e) == 0) {            /* явное решение оператора: якорь может понизиться */
            rolled_back = 0;
            uart_print("[KIRA FS] anchor: operator accepted the disk, epoch "); uart_print_dec(e); uart_print(" is now the TPM anchor, volume is writable\n");
        } else kfs_set_readonly(rolled_back);
    } else {
        uart_print("fsanchor [status|sync|accept]\n");
    }
}
#else
void trustanchor_init(void) { uart_print("[KIRA BOOT] rollback: no TPM driver on this architecture: image rollback is not checked against a counter, disk rollback is not detected\n"); }
void trustanchor_fs_mounted(void) {}
void trustanchor_tick(void) {}
void trustanchor_shutdown(void) {}
void trustanchor_command(const char* a) { (void)a; uart_print("fsanchor: no TPM driver on this architecture\n"); }
#endif
