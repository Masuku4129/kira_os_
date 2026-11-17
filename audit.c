/* Журнал аудита в ядре: см. audit.h и docs/AUDIT.md. */
#include "audit.h"
#include "auditstore.h"
#include "kfs.h"
#include "kmutex.h"
#include "spinlock.h"
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "rtc.h"
#include "uart.h"
#include "rng.h"
#include "proc.h"
#include "uaccess.h"
#include "boottrust.h"
#include "sha256.h"
#include "crypto.h"
#include "pk.h"

#define EVQ 512                                   /* события, ещё не вошедшие в цепочку (по 160 байт) */
#define KEY_FILE "/var/audit.key"
#define KEY_MAGIC "KAK1"
#define PUMP_TICKS (TIMER_HZ / 4)                 /* поток просыпается 4 раза в секунду */

static aud_rec_t evq[EVQ];
static unsigned evq_h, evq_t;                     /* под evq_lock: пишут все, читает поток audit */
static uint64_t lost, lost_first_ms;
static spinlock_t evq_lock = SPINLOCK_INIT;
static volatile unsigned cmd_src = AUD_SRC_CONSOLE;
static volatile uint64_t st_events, st_ns;        /* накладные расходы горячего пути: сколько событий и сколько наносекунд в audit_event */

static aud_store_t ST;
static aud_chain_t CH;
static kmutex_t smx;                              /* хранилище, цепочка, ключ: поток журнала, команды и системный вызов */
static uint8_t SEED[32], PUB[32];
static int have_key, loaded;
static aud_vstate_t LV;                           /* итог проверки при загрузке */
static uint32_t boot_id;
static unsigned since_ckpt;
static uint64_t last_ckpt_ms;
static int disk_bad;                              /* 1 - запись на диск не проходит (сообщено) */
static uint64_t lost_reported;
static uint64_t tamper_seq_reported;
static int tamper_now;                            /* при загрузке или проверке найдено нарушение */

void audit_set_cmd_source(unsigned s) { cmd_src = s < AUD_SRC_MAX ? s : AUD_SRC_CONSOLE; }
unsigned audit_cmd_source(void) { return cmd_src; }

static void cpy_str(char* d, unsigned cap, const char* s) {                         /* усечение до cap-1, остальное нули */
    unsigned i = 0;
    if (s) for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    for (; i < cap; i++) d[i] = 0;
}

/* ---------- горячий путь: только копия записи в кольцо под замком (хэш, подпись и диск - в потоке) ---------- */
void audit_event(unsigned kind, unsigned src, unsigned res, uint32_t pid, uint32_t dom, uint32_t arg, const char* subject, const char* detail) {
    uint64_t t0 = clock_ready() ? clock_ns() : 0;
    aud_rec_t r;
    __builtin_memset(&r, 0, sizeof r);
    r.mono_ms = clock_ready() ? clock_ms() : 0;
    int64_t re = rtc_now_epoch();
    r.real_s = re > 0 ? (uint64_t)re : 0;
    r.kind = (uint16_t)kind; r.src = (uint8_t)src; r.res = (uint8_t)res; r.pid = pid; r.domain = dom; r.arg = arg;
    cpy_str(r.subject, sizeof r.subject, subject); cpy_str(r.detail, sizeof r.detail, detail);
    uint64_t f = spin_lock_irqsave(&evq_lock);
    if (evq_h - evq_t >= EVQ) { if (!lost) lost_first_ms = r.mono_ms; lost++; }
    else evq[evq_h++ % EVQ] = r;
    spin_unlock_irqrestore(&evq_lock, f);
    if (t0) { __atomic_fetch_add(&st_events, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&st_ns, clock_ns() - t0, __ATOMIC_RELAXED); }
}

void audit_event_cur(unsigned kind, unsigned res, uint32_t arg, const char* detail) {
    audit_event(kind, AUD_SRC_PROGRAM, res, proc_current_pid(), proc_current_domain(), arg, proc_current_name(), detail);
}

/* ---------- поток журнала ---------- */
static void u64s(char* o, unsigned cap, unsigned* n, uint64_t v) { char t[21]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k && *n + 1 < cap) o[(*n)++] = t[--k]; }
static void put_hex(const uint8_t* b, unsigned n, void (*out)(const char*)) { static const char hx[] = "0123456789abcdef"; char l[3]; l[2] = 0; for (unsigned i = 0; i < n; i++) { l[0] = hx[b[i] >> 4]; l[1] = hx[b[i] & 15]; out(l); } }
static void hex_line(const uint8_t* b, unsigned n, char* o) { static const char hx[] = "0123456789abcdef"; for (unsigned i = 0; i < n; i++) { o[2 * i] = hx[b[i] >> 4]; o[2 * i + 1] = hx[b[i] & 15]; } o[2 * n] = 0; }

static int ensure_room(void) {
    if (aud_store_room(&ST)) return 0;
    if (aud_store_flush(&ST) != 0) return -1;
    return aud_store_room(&ST) ? 0 : -1;
}

static void warn_disk(int bad) {
    if (bad && !disk_bad) { disk_bad = 1; uart_print("[KIRA AUDIT] warning: cannot write /var/audit.log (disk full or read-only): events stay in memory and are retried; when memory fills, a signed 'loss' record counts the dropped events\n"); }
    else if (!bad && disk_bad) { disk_bad = 0; uart_print("[KIRA AUDIT] disk writes recovered; journal is complete up to the last 'loss' record\n"); }
}

static int make_ckpt(void) {                                      /* требует места (ensure_room) */
    aud_rec_t c; __builtin_memset(&c, 0, sizeof c);
    c.mono_ms = clock_ready() ? clock_ms() : 0;
    int64_t re = rtc_now_epoch(); c.real_s = re > 0 ? (uint64_t)re : 0;
    c.boot = boot_id;
    aud_ckpt_fill(&c, PUB);
    aud_seal(&CH, &c);
    if (aud_ckpt_sign(&c, SEED) != 0) { uart_print("[KIRA AUDIT] checkpoint signing failed\n"); return -1; }
    if (aud_store_append(&ST, &c) != 0) return -1;
    since_ckpt = 0; last_ckpt_ms = c.mono_ms;
    return 0;
}

/* Запечатывает одну запись (при необходимости - сначала контрольную точку границы сегмента). 0 - принята. */
static int put_event(aud_rec_t* r) {
    if (ensure_room()) return -1;
    if (aud_next_is_ckpt(&CH)) { if (make_ckpt()) return -1; if (ensure_room()) return -1; }
    r->boot = boot_id;
    aud_seal(&CH, r);
    if (aud_store_append(&ST, r) != 0) return -1;
    since_ckpt++;
    if (since_ckpt >= AUD_CKPT_EVERY) { if (ensure_room() == 0) (void)make_ckpt(); }
    return 0;
}
static void put_local(unsigned kind, unsigned src, unsigned res, uint32_t arg, const char* subj, const char* det) {
    aud_rec_t r; __builtin_memset(&r, 0, sizeof r);
    r.mono_ms = clock_ready() ? clock_ms() : 0;
    int64_t re = rtc_now_epoch(); r.real_s = re > 0 ? (uint64_t)re : 0;
    r.kind = (uint16_t)kind; r.src = (uint8_t)src; r.res = (uint8_t)res; r.arg = arg;
    cpy_str(r.subject, sizeof r.subject, subj); cpy_str(r.detail, sizeof r.detail, det);
    (void)put_event(&r);
}

/* Забирает события из кольца в цепочку. Под smx. */
static void drain(void) {
    for (;;) {
        aud_rec_t r; uint64_t lc = 0, lf = 0;
        uint64_t f = spin_lock_irqsave(&evq_lock);
        int have = evq_h != evq_t;
        if (have) r = evq[evq_t % EVQ];
        lc = lost; lf = lost_first_ms;
        spin_unlock_irqrestore(&evq_lock, f);
        if (lc && !have) {                                        /* кольцо опустело: теперь можно записать, сколько событий потеряно */
            char d[56]; unsigned n = 0; const char* a = "first at +"; while (*a) d[n++] = *a++; u64s(d, sizeof d, &n, lf); a = "ms, disk errors "; while (*a && n + 1 < sizeof d) d[n++] = *a++; u64s(d, sizeof d, &n, ST.io_errors); d[n] = 0;
            aud_rec_t l; __builtin_memset(&l, 0, sizeof l);
            l.mono_ms = clock_ready() ? clock_ms() : 0; l.kind = AUD_K_LOSS; l.src = AUD_SRC_KERNEL; l.res = AUD_R_FAIL; l.arg = (uint32_t)lc;
            cpy_str(l.subject, sizeof l.subject, "event-ring-full"); cpy_str(l.detail, sizeof l.detail, d);
            if (put_event(&l) == 0) {
                f = spin_lock_irqsave(&evq_lock); lost -= lc; spin_unlock_irqrestore(&evq_lock, f);
                if (lc > lost_reported) lost_reported = lc;
                uart_print("[KIRA AUDIT] "); uart_print_dec(lc); uart_print(" events were dropped (ring full while the disk was not accepting writes); recorded in the journal as a 'loss' record\n");
                continue;
            }
            break;
        }
        if (!have) break;
        if (put_event(&r) != 0) break;                            /* нет места на диске для закрытого сегмента: события ждут в кольце */
        f = spin_lock_irqsave(&evq_lock); evq_t++; spin_unlock_irqrestore(&evq_lock, f);
    }
}

static void idle_ckpt(void) {
    uint64_t now = clock_ready() ? clock_ms() : 0;
    if (since_ckpt && now - last_ckpt_ms >= (uint64_t)AUD_CKPT_IDLE_S * 1000 && ensure_room() == 0) (void)make_ckpt();
}

static void prune_if_due(void) {
    uint64_t upto;
    if (!aud_store_prune_due(&ST, &upto)) return;
    aud_rec_t r; __builtin_memset(&r, 0, sizeof r);
    r.mono_ms = clock_ready() ? clock_ms() : 0; r.kind = AUD_K_PRUNE; r.src = AUD_SRC_KERNEL; r.arg = (uint32_t)upto;
    cpy_str(r.subject, sizeof r.subject, "audit"); cpy_str(r.detail, sizeof r.detail, "oldest segment removed (retention limit)");
    if (put_event(&r) != 0) return;
    if (ensure_room() == 0) (void)make_ckpt();
    if (aud_store_flush(&ST) != 0) return;                        /* сначала запись о очистке на диске, потом удаление */
    unsigned g = aud_store_prune(&ST, upto);
    uart_print("[KIRA AUDIT] retention limit: "); uart_print_dec(g); uart_print(" oldest closed segment(s) removed, records up to #"); uart_print_dec(upto); uart_print(" (a signed 'prune' record is in the journal)\n");
}

static void pump_locked(void) {
    if (!loaded) return;
    drain();
    idle_ckpt();
    int bad = aud_store_flush(&ST) != 0;
    warn_disk(bad);
    if (!bad) prune_if_due();
}

/* Ключ устройства: создаётся при первой загрузке на самом устройстве, хранится в /var/audit.key (68 байт: метка, семя, SHA-256 метки и семени). */
static int key_load_or_create(unsigned* flags) {
    uint8_t b[68]; kfs_stat_t st;
    int64_t n = kfs_stat(KEY_FILE, &st) == 0 ? kfs_read(KEY_FILE, 0, b, sizeof b) : -1;
    int had_log = kfs_stat(AUD_ACTIVE, &st) == 0;
    if (n == (int64_t)sizeof b && !__builtin_memcmp(b, KEY_MAGIC, 4)) {
        uint8_t h[32], m[36]; __builtin_memcpy(m, b, 36); sha256(m, 36, h);
        if (crypto_eq(h, b + 36, 32)) { __builtin_memcpy(SEED, b + 4, 32); crypto_wipe(b, sizeof b); return 0; }
    }
    int exists = n >= 0;
    if (exists) { (void)kfs_clone(KEY_FILE, "/var/audit.key.bad"); uart_print("[KIRA AUDIT] device key file is damaged: replaced (old one kept as /var/audit.key.bad); the old journal will not verify\n"); }
    int q = rng_get(SEED, 32);
    uint8_t h[32], m[36]; __builtin_memcpy(m, KEY_MAGIC, 4); __builtin_memcpy(m + 4, SEED, 32); sha256(m, 36, h);
    __builtin_memcpy(b, m, 36); __builtin_memcpy(b + 36, h, 32);
    if (kfs_stat("/var", &st) != 0) (void)kfs_mkdir("/var");
    if (kfs_write(KEY_FILE, b, sizeof b) != 0) uart_print("[KIRA AUDIT] warning: cannot save the device key to " KEY_FILE ": signatures will not verify after a reboot\n");
    crypto_wipe(b, sizeof b); crypto_wipe(m, sizeof m);
    if (!q) uart_print("[KIRA AUDIT] warning: no hardware entropy source: the device key was seeded from event timing only\n");
    *flags |= AUD_BOOT_NEWKEY;
    if (exists || had_log) { *flags |= AUD_BOOT_TAMPER; }      /* ключа нет, а журнал есть (или ключ испорчен): старые записи не подтвердить */
    return 1;
}

static const char* vcode_short(int c) {
    static const char* const n[] = { "ok", "FORMAT", "KEY", "ANCHOR", "SEQ", "CHAIN", "SIG", "TRUNC", "PRUNE" };
    return c >= 0 && c <= AUD_V_PRUNE ? n[c] : "?";
}

static void report_violation(const aud_vstate_t* v, const char* where) {
    uart_print("[KIRA AUDIT] tamper detected ("); uart_print(where); uart_print("): "); uart_print(aud_verr_name(v->code));
    uart_print(": record #"); uart_print_dec(v->bad_seq); uart_print(" (file #"); uart_print_dec((uint64_t)v->bad_file); uart_print(", offset "); uart_print_dec(v->bad_off); uart_print(")\n");
    if (tamper_seq_reported != v->bad_seq + 1) {
        tamper_seq_reported = v->bad_seq + 1;
        char d[56]; unsigned n = 0; const char* a = vcode_short(v->code); while (*a) d[n++] = *a++;
        a = " file#"; while (*a) d[n++] = *a++; u64s(d, sizeof d, &n, (uint64_t)v->bad_file); a = " off "; while (*a) d[n++] = *a++; u64s(d, sizeof d, &n, v->bad_off); d[n] = 0;
        put_local(AUD_K_TAMPER, AUD_SRC_KERNEL, AUD_R_FAIL, (uint32_t)v->bad_seq, where, d);
    }
}

static int load_all(void) {
    unsigned flags = 0;
    if (!have_key) { key_load_or_create(&flags); if (ed25519_pubkey(SEED, PUB) != 0) { uart_print("[KIRA AUDIT] cannot derive the device public key\n"); return -1; } have_key = 1; }
    if (aud_store_load(&ST, PUB, &LV, &CH) != 0) return -1;
    boot_id = 1; int unclean = 0;
    if (ST.next_seq > 1) {
        aud_rec_t r; int shut = 0;
        for (uint64_t s = ST.next_seq - 1, k = 0; s >= 1 && k < 3; s--, k++) {
            if (aud_store_read_rec(&ST, s, &r) != 0) break;
            if (k == 0) boot_id = r.boot + 1;
            if (r.kind == AUD_K_SHUTDOWN) { shut = 1; break; }
            if (s == 1) break;
        }
        unclean = !shut;
    }
    if (unclean) flags |= AUD_BOOT_UNCLEAN;
    if (LV.code != AUD_V_OK) flags |= AUD_BOOT_TAMPER;
    loaded = 1; last_ckpt_ms = clock_ready() ? clock_ms() : 0;
    const boottrust_t* bt = boottrust_get();
    char d[56]; unsigned n = 0;
    const char* a = bt && bt->reported ? (boottrust_is_trusted(bt) ? "trusted (Secure Boot + signed kernel)" : "reported, not fully trusted") : "no trust chain (BIOS/GRUB/DT)"; while (*a && n + 1 < sizeof d) d[n++] = *a++; d[n] = 0;
    put_local(AUD_K_BOOT, AUD_SRC_SYSTEM, AUD_R_OK, flags, "kernel", d);
    if (flags & AUD_BOOT_NEWKEY) put_local(AUD_K_KEY, AUD_SRC_SYSTEM, flags & AUD_BOOT_TAMPER ? AUD_R_FAIL : AUD_R_OK, (flags & AUD_BOOT_TAMPER) ? 1 : 0, "device-key", (flags & AUD_BOOT_TAMPER) ? "replaced: old journal cannot be verified" : "created on first boot");
    if (bt) { n = 0; const char* t = "flags=0x"; while (*t) d[n++] = *t++; { static const char hx[] = "0123456789abcdef"; for (int i = 28; i >= 0; i -= 4) d[n++] = hx[(bt->flags >> i) & 15]; } t = " ver="; while (*t) d[n++] = *t++; u64s(d, sizeof d, &n, bt->image_version); d[n] = 0;
        put_local(AUD_K_TRUST, AUD_SRC_SYSTEM, boottrust_is_trusted(bt) ? AUD_R_OK : AUD_R_DENIED, bt->flags, "boot-trust", d); }
    if (LV.code != AUD_V_OK) { tamper_now = 1; report_violation(&LV, "boot"); }
    uart_print("[KIRA AUDIT] journal online: next record #"); uart_print_dec(ST.next_seq); uart_print(", boot #"); uart_print_dec(boot_id);
    uart_print(", closed segments "); uart_print_dec(ST.nsegs); uart_print(", key id "); { uint8_t id[16]; aud_keyid(PUB, id); put_hex(id, 4, uart_print); }
    uart_print(ST.next_seq <= 1 + 3 && boot_id == 1 ? ", first boot of this journal" : unclean ? ", previous run ended without shutdown record" : ", previous run ended cleanly");
    uart_print(LV.code == AUD_V_OK ? ", chain verified OK\n" : ", chain VIOLATED\n");
    return 0;
}

static void audit_main(void* arg) {
    (void)arg;
    while (!kfs_mounted()) sched_sleep_ticks(TIMER_HZ);
    for (;;) {
        kmutex_lock(&smx);
        if (!loaded && load_all() != 0) uart_print("[KIRA AUDIT] disk journal not available yet: events stay in memory\n");
        pump_locked();
        kmutex_unlock(&smx);
        sched_sleep_ticks(loaded ? PUMP_TICKS : TIMER_HZ * 5);
    }
}

void audit_init(void) {
    if (ed25519_prepare() != 0) uart_print("[KIRA AUDIT] Ed25519 constants failed to initialize\n");
    if (!sched_create("audit", audit_main, 0, PRIO_NORMAL)) uart_print("[KIRA AUDIT] cannot start the journal thread\n");
    uart_print("[KIRA AUDIT] tamper-evident journal: hash chain + Ed25519 checkpoints, /var/audit.log (starts when the file system is ready)\n");
}

static void sync_locked(int shutdown) {
    if (!loaded) return;
    drain();
    if (shutdown) put_local(AUD_K_SHUTDOWN, AUD_SRC_SYSTEM, AUD_R_OK, 0, "kernel", "clean shutdown");
    if ((shutdown || since_ckpt) && ensure_room() == 0) (void)make_ckpt();
    (void)aud_store_flush(&ST);
}
void audit_sync(void) { if (!kmutex_trylock(&smx)) return; sync_locked(0); kmutex_unlock(&smx); }
void audit_shutdown(void) { if (!kmutex_trylock(&smx)) return; sync_locked(1); kmutex_unlock(&smx); }

/* ---------- чтение программами ---------- */
int64_t audit_sys_read(uint64_t from, uint64_t ubuf, uint64_t cap) {
    if (cap < AUD_REC_SIZE) return -10;
    kmutex_lock(&smx);
    int64_t out = 0;
    if (loaded) {
        drain();
        uint64_t oldest = ST.nsegs ? ST.segs[0] : ST.cur_first;
        if (from < oldest) from = oldest;
        aud_rec_t r[8]; uint64_t want = cap / AUD_REC_SIZE; if (want > 8) want = 8;
        while ((uint64_t)out < want && from + (uint64_t)out < ST.next_seq && aud_store_read_rec(&ST, from + (uint64_t)out, &r[out]) == 0) out++;
        if (out && copy_to_user(ubuf, r, (uint64_t)out * AUD_REC_SIZE) != 0) out = -10;
    }
    kmutex_unlock(&smx);
    return out;
}

/* ---------- консоль ---------- */
static int word(const char* a, const char* w) { unsigned i = 0; while (w[i] && a[i] == w[i]) i++; return !w[i] && (a[i] == 0 || a[i] == ' '); }
static uint64_t num(const char** p) { while (**p == ' ') (*p)++; uint64_t v = 0; while (**p >= '0' && **p <= '9') v = v * 10 + (uint64_t)(*(*p)++ - '0'); return v; }

static void do_status(void) {
    uint64_t pend, ls; uint64_t f = spin_lock_irqsave(&evq_lock); pend = evq_h - evq_t; ls = lost; spin_unlock_irqrestore(&evq_lock, f);
    uart_print("[KIRA AUDIT] status: boot #"); uart_print_dec(boot_id); uart_print(", key id "); { uint8_t id[16]; aud_keyid(PUB, id); put_hex(id, 8, uart_print); }
    uart_print(", records "); uart_print_dec(ST.next_seq ? ST.next_seq - 1 : 0); uart_print(" (on disk up to #"); uart_print_dec(ST.flushed_next ? ST.flushed_next - 1 : 0); uart_print(")");
    uart_print(", last checkpoint #"); uart_print_dec(ST.have_ckpt ? ST.last_ckpt.seq : 0); uart_print(", closed segments "); uart_print_dec(ST.nsegs);
    uart_print(", pending "); uart_print_dec(pend); uart_print(", lost "); uart_print_dec(ls); uart_print(", disk errors "); uart_print_dec(ST.io_errors);
    uart_print(", tamper "); uart_print(tamper_now ? "DETECTED" : "none"); uart_print(", at boot: "); uart_print(vcode_short(LV.code)); uart_print("\n");
    uint64_t ev = st_events, ns = st_ns;
    uart_print("[KIRA AUDIT] hot path: "); uart_print_dec(ev); uart_print(" events, mean "); uart_print_dec(ev ? ns / ev : 0); uart_print(" ns per event (ring copy only; hash, signature and disk are in the audit thread)\n");
}

static void do_verify(void) {
    uart_print("[KIRA AUDIT] verifying every record and every checkpoint signature on disk...\n");
    drain();
    if (aud_store_flush(&ST) != 0) uart_print("[KIRA AUDIT] note: disk write failing, verification covers what is on disk\n");
    aud_vstate_t v;
    if (aud_store_verify(&ST, &v) != 0) { uart_print("[KIRA AUDIT] verify: cannot read the journal files\n"); return; }
    if (v.code == AUD_V_OK) {
        uart_print("[KIRA AUDIT] verify: OK: "); uart_print_dec(v.count); uart_print(" records (#"); uart_print_dec(v.first_seq); uart_print("..#"); uart_print_dec(v.next_seq - 1);
        uart_print("), "); uart_print_dec(v.ckpts); uart_print(" signed checkpoints, "); uart_print_dec((uint64_t)v.files); uart_print(" files; unsigned tail "); uart_print_dec(v.next_seq - 1 - v.last_ckpt_seq); uart_print(" records\n");
    } else {
        uart_print("[KIRA AUDIT] verify: VIOLATION\n");
        tamper_now = 1; report_violation(&v, "verify");
    }
}

static void do_list(uint64_t n) {
    drain();
    if (!n) n = 20; if (n > 100) n = 100;
    uint64_t last = ST.next_seq - 1;
    if (!last) { uart_print("[KIRA AUDIT] journal is empty\n"); return; }
    uint64_t from = last >= n ? last - n + 1 : 1;
    for (uint64_t s = from; s <= last; s++) {
        aud_rec_t r; char l[300];
        if (aud_store_read_rec(&ST, s, &r) != 0) continue;
        aud_format(&r, l, sizeof l); uart_print("  "); uart_print(l); uart_print("\n");
    }
}

static void do_export(const char* a) {
    drain();
    uint64_t from = num(&a), cnt = num(&a);
    uint64_t oldest = ST.nsegs ? ST.segs[0] : ST.cur_first;
    if (from < oldest) from = oldest;
    if (!cnt || cnt > 16) cnt = 8;
    uart_print("AUDKEY "); put_hex(PUB, 32, uart_print); uart_print("\n");
    uint64_t s = from;
    for (; s < from + cnt && s < ST.next_seq; s++) {
        aud_rec_t r; char l[8 + 2 * AUD_REC_SIZE];
        if (aud_store_read_rec(&ST, s, &r) != 0) break;
        __builtin_memcpy(l, "AUDREC ", 7); hex_line((const uint8_t*)&r, AUD_REC_SIZE, l + 7);
        uart_print(l); uart_print("\n");
    }
    if (s < ST.next_seq) { uart_print("AUDNEXT "); uart_print_dec(s); uart_print("\n"); } else uart_print("AUDEND\n");
}

void audit_command(const char* arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg) arg = "";
    kmutex_lock(&smx);
    if (!loaded) { kmutex_unlock(&smx); uart_print("[KIRA AUDIT] journal is not online yet (waiting for the file system); events are queued\n"); return; }
    if (word(arg, "status")) do_status();
    else if (word(arg, "verify")) do_verify();
    else if (word(arg, "key")) { uart_print("[KIRA AUDIT] device public key (Ed25519): "); put_hex(PUB, 32, uart_print); uart_print("\n"); }
    else if (word(arg, "export")) do_export(arg + 6);
    else if (word(arg, "checkpoint")) {
        drain();
        if (ensure_room() == 0 && make_ckpt() == 0 && aud_store_flush(&ST) == 0) { uart_print("[KIRA AUDIT] checkpoint #"); uart_print_dec(ST.last_ckpt.seq); uart_print(" signed and written\n"); }
        else uart_print("[KIRA AUDIT] checkpoint not written (disk not accepting writes)\n");
    }
    else if (!arg[0] || (arg[0] >= '0' && arg[0] <= '9')) { const char* p = arg; do_list(num(&p)); }
    else uart_print("audit [N]: последние записи; audit status; audit verify: проверить цепочку и подписи (OK или где нарушено); audit export [с_номера [сколько<=16]]: записи шестнадцатеричным текстом (AUDREC) для tools/auditverify.py; audit key: открытый ключ устройства; audit checkpoint: подписать и записать сейчас\n");
    kmutex_unlock(&smx);
}
