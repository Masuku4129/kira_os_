/* Менеджер драйверов Ring 3 (kdrv.h, kdrv_format.h, docs/DRIVERS.md). Одно устройство PCI - одна привязка (слот) на всё время работы
   ядра: в слоте живут прерывание устройства (вектор MSI выдаётся один раз и не отдаётся обратно) и карта драйвера для сетевого стека
   (стек не умеет забывать карты). Все обращения драйвера к устройству идут через вызовы KDRV_SYS_* ниже, и каждый из них смотрит
   на привязку ТЕКУЩЕГО процесса: другого устройства драйвер не видит, потому что вызовы его не называют. */
#include "auditev.h"
#include "kdrv.h"
#include "fb.h"
#include "kdrv_format.h"
#include "kdrv_sig.h"
#include "kdrv_trust.h"
#include "kdrv_state.h"
#include "kdrv_blk.h"
#include "blk.h"
#include "virtio.h"
#include "kmutex.h"
#include "pci.h"
#if defined(__x86_64__)
#include "pic.h"
#include "irqmode.h"
#include "acpidev.h"
#endif
#include "iommu.h"
#include "msi.h"
#include "proc.h"
#include "kobj.h"
#include "kfs.h"
#include "rtc.h"
#include "immune.h"
#include "uart.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "uaccess.h"
#include "sched.h"
#include "timer.h"
#include "net.h"
#include "life.h"
#include "spinlock.h"

#define KDRV_SLOTS 8
#define MMIO_VA USER_VA(0x0000200000000000ULL)      /* окна драйвера в его адресном пространстве (SHM - ниже, до 0x2000_0000_0000) */
#define DMA_VA  USER_VA(0x0000280000000000ULL)
#define NET_VA  USER_VA(0x0000300000000000ULL)
#define BLK_VA  USER_VA(0x0000380000000000ULL)      /* кольца и буферы диска драйвера (CAP_BLKDEV) */
#define KDRV_STABLE_TICKS (5 * TIMER_HZ)           /* драйвер новой версии считается успешно запущенным, если проработал столько */
#define RELEASE_DELAY_TICKS (TIMER_HZ / 10 + 1)     /* 100 мс: запросы устройства, уже ушедшие в шину, успевают закончиться */

int platform_dma_noncoherent(void) __attribute__((weak));   /* ARM64: плата без согласованности кэша - пул без кэша, драйверам его не отдаём */

enum { B_FREE = 0, B_RUN = 1, B_DEAD = 2 };
typedef struct { uint64_t va; uint32_t pages; } kmap_t;
typedef struct { uint64_t pa; uint32_t pages; } kblk_t;

typedef struct {
    const pci_dev_t* dev;            /* слот закреплён за устройством навсегда (0 - свободен) */
    volatile int state;
    char name[16];
    uint32_t granted, version, pid;
    /* прерывание: вектор выдаётся один раз */
    irqwait_t iw; volatile uint32_t irq_count; uint32_t irq_seen;
    int irq_tried, irq_have, irq_on; uint64_t irq_addr; uint32_t irq_data;
    int intx; int intx_line; volatile uint32_t intx_masked;   /* прерывание - линия INTx (нет MSI) */
    volatile uint32_t waiting, tx_pending;
    uint32_t denied;                 /* отказов по политике (в журнал - первый) */
    kmap_t map[KDRV_MAX_MMIO]; uint32_t nmap; uint64_t mmio_next;
    kblk_t dma[KDRV_MAX_DMA]; uint32_t ndma, dma_pages; uint64_t dma_next;
    /* карта драйвера для сетевого стека */
    netdev_t nd; int nd_registered;
    spinlock_t nlk; volatile int net_alive;
    kdrv_netctl_t* ctl; uint64_t net_pa; uint32_t net_pages;
    uint32_t tx_head, rx_tail;       /* свои счётчики ядра: общую память драйвера читаем только как данные */
    irqwait_t net_iw;
    uint64_t net_tx, net_rx, net_bad;
    /* диск драйвера для blk.c (kdrv_blk.h): кольца в общей памяти, запросы ядро кладёт само, ответы драйвера недоверенные */
    blk_dev_t bd; char blk_id[12]; uint32_t blk_flags; uint64_t blk_sectors;
    spinlock_t blk_lk; volatile int blk_alive; volatile uint32_t blk_pending; volatile int blk_reg_pending;
    kdrv_blkctl_t* bctl; uint64_t blk_pa; uint32_t blk_pages;
    uint32_t req_head, rsp_tail;     /* свои счётчики ядра: общую память драйвера читаем только как данные */
    kdrv_blk_track_t trk; irqwait_t blk_iw; kmutex_t blk_mx;   /* blk_mx: одна операция за раз (blk.c и так держит свой замок диска; здесь - и для blk_flush_all) */
    uint64_t blk_ops, blk_bad;
    uint64_t dead_at;
    int floor_pending; uint64_t started_at;   /* пол версий поднимается, когда драйвер проработал KDRV_STABLE_TICKS */
} bind_t;

static bind_t binds[KDRV_SLOTS];
static spinlock_t klock = SPINLOCK_INIT;
static void blk_reg_run(void);
static volatile uint32_t ndead;

static void print_bdf(const pci_dev_t* d) {
    static const char hx[] = "0123456789abcdef";
    char s[10] = { hx[d->bus >> 4], hx[d->bus & 15], ':', hx[d->dev >> 4], hx[d->dev & 15], '.', (char)('0' + (d->fn & 7)), 0 };
    uart_print(s);
}
static void print_id(const pci_dev_t* d) {
    static const char hx[] = "0123456789abcdef";
    char s[10] = { hx[d->vendor >> 12], hx[(d->vendor >> 8) & 15], hx[(d->vendor >> 4) & 15], hx[d->vendor & 15], ':',
                   hx[d->device >> 12], hx[(d->device >> 8) & 15], hx[(d->device >> 4) & 15], hx[d->device & 15], 0 };
    uart_print(s);
}
static void print_caps(uint32_t c) {
    for (uint32_t b = 1; b; b <<= 1) if ((c & b) && (b & CAP_DRIVER_)) { uart_print(" "); uart_print(caps_bit_name(b)); }
    if (!(c & CAP_DRIVER_)) uart_print(" -");
}
static uint32_t cls_of(const pci_dev_t* d) { return (uint32_t)d->class_code << 16 | (uint32_t)d->subclass << 8 | d->prog_if; }
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ---- какие устройства уже принадлежат встроенным драйверам ядра (то, что они ищут по идентификатору и классу) ---- */
static int builtin_owned(const pci_dev_t* d) {
    static const uint16_t e1000[] = { 0x100E, 0x100F, 0x1004, 0x1009, 0x100C, 0x100D, 0x1010, 0x1011, 0x1015, 0x1026, 0x1076, 0x1079, 0x107C, 0x10D3 };
    if (d->class_code == 0x06) return 1;                                              /* мосты: драйверу не отдаются никогда */
    if ((d->header_type & 0x7F) != 0) return 1;                                       /* только обычные устройства */
    /* Второй и далее virtio-blk: встроенный драйвер берёт один диск (первый), остальные никому не принадлежат - их можно отдать драйверу Ring 3. */
    if (d->vendor == 0x1AF4 && (d->device == 0x1042 || d->device == 0x1001) && d->class_code == 0x01 && virtio_blk_pci() && d != virtio_blk_pci()) return 0;
    if (d->vendor == 0x1AF4 && d->device >= 0x1000 && d->device <= 0x107F) return 1;  /* virtio */
    if (d->vendor == 0x8086) for (unsigned i = 0; i < sizeof e1000 / sizeof e1000[0]; i++) if (d->device == e1000[i]) return 1;
    if (d->class_code == 0x01 && (d->subclass == 0x01 || d->subclass == 0x06 || d->subclass == 0x08)) return 1;   /* IDE, AHCI, NVMe */
    if (d->class_code == 0x0C && d->subclass == 0x03) return 1;                       /* USB */
    if (d->class_code == 0x04 && (d->subclass == 0x01 || d->subclass == 0x03)) return 1;                           /* AC'97, HDA */
    return 0;
}

static bind_t* find_slot(const pci_dev_t* d, int create) {
    uint64_t f = spin_lock_irqsave(&klock);
    bind_t* r = 0;
    for (unsigned i = 0; i < KDRV_SLOTS && !r; i++) if (binds[i].dev == d) r = &binds[i];
    for (unsigned i = 0; create && i < KDRV_SLOTS && !r; i++) if (!binds[i].dev) { binds[i].dev = d; r = &binds[i]; }
    spin_unlock_irqrestore(&klock, f);
    return r;
}

/* ---- пакеты в /drv ---- */
typedef struct { uint8_t* buf; uint32_t len; kdrv_header_t h; kdrv_rec_t r; int auth; int sig_error; } pkg_t;

static void path_of(char* out, const char* name, const char* ext) {
    unsigned n = 0;
    for (const char* p = "/drv/"; *p; p++) out[n++] = *p;
    for (const char* p = name; *p; p++) out[n++] = *p;
    for (const char* p = ext; *p; p++) out[n++] = *p;
    out[n] = 0;
}

static int name_valid(const char* n) {
    unsigned i = 0;
    for (; n[i]; i++) { char c = n[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') || i >= KDRV_NAME_MAX) return 0; }
    return i > 0;
}

/* ---- постоянное состояние защиты от отката (kdrv_state.h): пол версий, максимум времени, отозванные ключи ---- */
static kmutex_t state_mx = KMUTEX_INIT;
static kdrv_state_t gst;
static int gst_loaded;

/* Вызывать под state_mx. Повреждённый файл НЕ сбрасывается: status CORRUPT остаётся, драйверы отклоняются с сообщением (KDRV_E_STATE),
   сбросить можно только явной командой drv state-reset. Ошибка чтения не кэшируется - следующая загрузка попробует снова. */
static void state_load_locked(void) {
    if (gst_loaded) return;
    kfs_stat_t st;
    int r = kfs_stat(KDRV_STATE_PATH, &st);
    if (r == KFS_ERR_NOENT) { kdrv_state_init(&gst); gst_loaded = 1; return; }       /* первый запуск: файла ещё нет */
    if (r != 0) { kdrv_state_mark_io_error(&gst); return; }
    uint8_t buf[KDRV_STATE_FILE_SIZE];
    if (st.type != KFS_FILE || st.size != KDRV_STATE_FILE_SIZE) { (void)kdrv_state_decode(&gst, buf, (size_t)st.size); gst_loaded = 1; return; }
    if (kfs_read(KDRV_STATE_PATH, 0, buf, sizeof buf) != (int64_t)sizeof buf) { kdrv_state_mark_io_error(&gst); return; }
    (void)kdrv_state_decode(&gst, buf, sizeof buf);
    gst_loaded = 1;
}

static int state_save_locked(void) {
    if (!gst.dirty || gst.status != KDRV_STATE_OK) return 0;
    uint8_t buf[KDRV_STATE_FILE_SIZE];
    kdrv_state_encode(&gst, buf);
    kfs_stat_t st;
    if (kfs_stat("/var", &st) != 0 && kfs_mkdir("/var") != 0) { uart_print("[KIRA DRV] не удалось создать /var: состояние защиты от отката НЕ сохранено\n"); return -1; }
    if (kfs_write(KDRV_STATE_PATH, buf, sizeof buf) != 0) { uart_print("[KIRA DRV] не удалось записать " KDRV_STATE_PATH ": состояние защиты от отката НЕ сохранено\n"); return -1; }
    gst.dirty = 0; gst.persisted_time = gst.max_time;
    return 0;
}

/* Читает пакет и ВСЕГДА сверяет хеши: образа ELF и всего файла .kdrv - с /drv/ИМЯ.rec. Ошибка - код KDRV_E_* или KFS-код (< -100 не бывает). */
static int pkg_load(const char* name, pkg_t* p) {
    p->buf = 0; p->len = 0; p->auth = KDRV_AUTH_LOCAL; p->sig_error = KDRV_SIG_OK;
    char path[48], rpath[48], spath[48];
    path_of(path, name, ".kdrv"); path_of(rpath, name, ".rec"); path_of(spath, name, ".sig");
    kfs_stat_t st, rs, ss;
    if (kfs_stat(path, &st) != 0 || st.type != KFS_FILE) return KDRV_E_SHORT;
    if (st.size < sizeof(kdrv_header_t) || st.size > sizeof(kdrv_header_t) + KDRV_ELF_MAX) return KDRV_E_SIZE;
    if (kfs_stat(rpath, &rs) != 0 || rs.type != KFS_FILE || rs.size != sizeof(kdrv_rec_t)) return KDRV_E_UNCONFIRMED;
    uint8_t* b = (uint8_t*)kmalloc((size_t)st.size);
    if (!b) return KDRV_E_SIZE;
    kdrv_rec_t rec;
    if (kfs_read(path, 0, b, (size_t)st.size) != (int64_t)st.size ||
        kfs_read(rpath, 0, &rec, sizeof rec) != (int64_t)sizeof rec) { kfree(b); return KDRV_E_SHORT; }
    int rc = kdrv_header_check(b, (uint32_t)st.size, &p->h);
    if (rc == KDRV_OK && !streq(p->h.name, name)) rc = KDRV_E_NAME;
    if (rc == KDRV_OK) rc = kdrv_rec_check(&rec, sizeof rec, &p->r);
    if (rc == KDRV_OK) {
        uint8_t d1[32], d2[32];
        sha256(b + sizeof(kdrv_header_t), p->h.elf_size, d1);
        sha256(b, (size_t)st.size, d2);
        int bad = 0;
        for (int i = 0; i < 32; i++) bad |= d1[i] ^ p->r.elf_sha[i];
        for (int i = 0; i < 32; i++) bad |= d2[i] ^ p->r.pkg_sha[i];
        if (bad) rc = KDRV_E_HASH;
    }
    if (rc == KDRV_OK) {
        kdrv_sig_t sig;
        const kdrv_sig_t* sp = 0;
        int se = kfs_stat(spath, &ss);
        if (se == 0) {
            if (ss.type != KFS_FILE || ss.size != sizeof(kdrv_sig_t)) { rc = KDRV_E_SIGNATURE; p->sig_error = KDRV_SIG_E_SIZE; }
            else if (kfs_read(spath, 0, &sig, sizeof sig) != (int64_t)sizeof sig) { rc = KDRV_E_SIGNATURE; p->sig_error = KDRV_SIG_E_IO; }
            else sp = &sig;
        } else if (se != KFS_ERR_NOENT) { rc = KDRV_E_SIGNATURE; p->sig_error = KDRV_SIG_E_IO; }
        if (rc == KDRV_OK) {
            /* Допуск: состояние читаемо -> часы не ушли назад -> ключ не отозван и не заменён -> подпись -> версия не ниже пола. */
            rtc_time_t current;
            int64_t now = rtc_now(0, &current) == 0 ? rtc_to_epoch(&current) : 0;
            kmutex_lock(&state_mx);
            state_load_locked();
            rc = kdrv_admit(&gst, b, (size_t)st.size, sp, sp ? sizeof sig : 0, kdrv_rec_publisher_required(&p->r),
                            kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, now, &p->h, &p->auth, &p->sig_error);
            (void)state_save_locked();
            kmutex_unlock(&state_mx);
        }
    }
    if (rc != KDRV_OK) { kfree(b); return rc; }
    p->buf = b; p->len = (uint32_t)st.size;
    return KDRV_OK;
}

/* Подпись приложения: /bin/ИМЯ.sig рядом с образом. Тот же ключ издателя и то же состояние (часы, отзыв), но свой домен подписи,
   поэтому подпись драйвера не годится для приложения и наоборот. Возвращает 0 при успехе, иначе код KDRV_SIG_*; прав такое приложение не получает. */
int kdrv_app_sig_check(const char* path, const void* image, size_t len, int* missing) {
    char spath[80]; unsigned n = 0;
    for (const char* q = path; *q && n < sizeof spath - 5; q++) spath[n++] = *q;
    spath[n++] = '.'; spath[n++] = 's'; spath[n++] = 'i'; spath[n++] = 'g'; spath[n] = 0;
    if (missing) *missing = 0;
    kfs_stat_t ss; kdrv_sig_t sig;
    int se = kfs_stat(spath, &ss);
    if (se == KFS_ERR_NOENT) { if (missing) *missing = 1; return KDRV_SIG_E_MISSING; }
    if (se != 0) return KDRV_SIG_E_IO;
    if (ss.type != KFS_FILE || ss.size != sizeof sig) return KDRV_SIG_E_SIZE;
    if (kfs_read(spath, 0, &sig, sizeof sig) != (int64_t)sizeof sig) return KDRV_SIG_E_IO;
    rtc_time_t current;
    int64_t now = rtc_now(0, &current) == 0 ? rtc_to_epoch(&current) : 0;
    kmutex_lock(&state_mx);
    state_load_locked();
    int rc = kdrv_state_verify(&gst, KDRV_SIG_DOMAIN_APP, image, len, &sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, now);
    (void)state_save_locked();
    kmutex_unlock(&state_mx);
    return rc;
}

/* Подпись манифеста приложений: /etc/apps.sig рядом с /etc/apps.manifest (appman.h), домен APPS, тот же ключ издателя и то же состояние (часы, отзыв, срок ключа).
   0 - верна, иначе KDRV_SIG_*; missing=1 - подписи нет. */
int kdrv_apps_sig_check(const void* manifest, size_t len, int* missing) {
    static const char spath[] = "/etc/apps.sig";
    if (missing) *missing = 0;
    kfs_stat_t ss; kdrv_sig_t sig;
    int se = kfs_stat(spath, &ss);
    if (se == KFS_ERR_NOENT) { if (missing) *missing = 1; return KDRV_SIG_E_MISSING; }
    if (se != 0) return KDRV_SIG_E_IO;
    if (ss.type != KFS_FILE || ss.size != sizeof sig) return KDRV_SIG_E_SIZE;
    if (kfs_read(spath, 0, &sig, sizeof sig) != (int64_t)sizeof sig) return KDRV_SIG_E_IO;
    rtc_time_t current;
    int64_t now = rtc_now(0, &current) == 0 ? rtc_to_epoch(&current) : 0;
    kmutex_lock(&state_mx);
    state_load_locked();
    int rc = kdrv_state_verify(&gst, KDRV_SIG_DOMAIN_APPS, manifest, len, &sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, now);
    (void)state_save_locked();
    kmutex_unlock(&state_mx);
    return rc;
}

/* Подпись печати системы: /etc/system.sig рядом с /etc/system.cbor, тот же ключ издателя и состояние, домен SEAL.
   Печать без подписи защищает от порчи, подписанная - ещё и от подмены всего образа. 0 - верна, иначе KDRV_SIG_*; missing=1 - подписи нет. */
int kdrv_seal_sig_check(const void* manifest, size_t len, int* missing) {
    static const char spath[] = "/etc/system.sig";
    if (missing) *missing = 0;
    kfs_stat_t ss; kdrv_sig_t sig;
    int se = kfs_stat(spath, &ss);
    if (se == KFS_ERR_NOENT) { if (missing) *missing = 1; return KDRV_SIG_E_MISSING; }
    if (se != 0) return KDRV_SIG_E_IO;
    if (ss.type != KFS_FILE || ss.size != sizeof sig) return KDRV_SIG_E_SIZE;
    if (kfs_read(spath, 0, &sig, sizeof sig) != (int64_t)sizeof sig) return KDRV_SIG_E_IO;
    rtc_time_t current;
    int64_t now = rtc_now(0, &current) == 0 ? rtc_to_epoch(&current) : 0;
    kmutex_lock(&state_mx);
    state_load_locked();
    int rc = kdrv_state_verify(&gst, KDRV_SIG_DOMAIN_SEAL, manifest, len, &sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, now);
    (void)state_save_locked();
    kmutex_unlock(&state_mx);
    return rc;
}

/* ---- прерывание устройства ---- */
static void kdrv_irq(void* ctx) {
    bind_t* b = (bind_t*)ctx;
    __atomic_add_fetch(&b->irq_count, 1, __ATOMIC_RELAXED);
    irqwait_fire(&b->iw);
}

static uint16_t find_cap(const pci_dev_t* d, uint8_t want) {
    uint8_t id; uint16_t c = 0;
    for (int n = 0; n < 48 && (c = pci_next_cap(d, c, &id)) != 0; n++) if (id == want) return c;
    return 0;
}

/* ---- INTx для драйверов Ring 3 (x86, APIC-режим): линия PCI - уровень, активный низкий; устройство гасится битом INTx-disable командного регистра,
   пока драйвер не вызовет KDRV_SYS_IRQ_WAIT снова (то есть снял причину). Линию делят несколько драйверов; линия, занятая ядром, не отдаётся. ---- */
#if defined(__x86_64__)
#define XL_MAX 8
#define XH_MAX 8                                  /* линии с GSI 16 и выше: ключи 16..23 в той же таблице */
static struct { bind_t* volatile b[XL_MAX]; volatile int active; int reg; volatile uint32_t unclaimed; uint32_t gsi; uint8_t vec; } xl[16 + XH_MAX];
#define XL_STORM 2000u   /* столько срабатываний подряд без владельца - уже шторм чужого устройства: линия гасится */
static void intx_dispatch(unsigned irq) {
    __atomic_add_fetch(&xl[irq].active, 1, __ATOMIC_ACQ_REL);
    int fired = 0;
    for (int i = 0; i < XL_MAX; i++) {
        bind_t* b = __atomic_load_n(&xl[irq].b[i], __ATOMIC_ACQUIRE);
        if (!b || !(pci_read16(b->dev, 0x06) & 0x08)) continue;                    /* бит «Interrupt Status»: именно это устройство держит линию */
        fired = 1;                                                                  /* линия принадлежит нашему устройству, даже если оно уже заглушено: линию не гасим */
        if (__atomic_exchange_n(&b->intx_masked, 1, __ATOMIC_ACQ_REL)) continue;    /* повторное срабатывание до того, как драйвер вернулся к ожиданию: событие уже записано */
        pci_write16(b->dev, 0x04, (uint16_t)(pci_read16(b->dev, 0x04) | PCI_CMD_INTX_OFF));
        __atomic_add_fetch(&b->irq_count, 1, __ATOMIC_RELAXED);
        irqwait_fire(&b->iw);
    }
    /* Никто из наших не держит линию. Одно такое срабатывание - обычное дело (драйвер на другом ядре уже снял причину раньше, чем мы прочитали статус): линия
       уровневая, без источника она больше не сработает. Гасить её нужно только при шторме - подряд много срабатываний без владельца (чужое устройство на общей линии). */
    if (fired) __atomic_store_n(&xl[irq].unclaimed, 0, __ATOMIC_RELAXED);
    else if (__atomic_add_fetch(&xl[irq].unclaimed, 1, __ATOMIC_RELAXED) >= XL_STORM) (void)(irq < 16 ? irqmode_pci_line(irq, 0) : irqmode_pci_gsi(xl[irq].gsi, xl[irq].vec, 0));
    __atomic_sub_fetch(&xl[irq].active, 1, __ATOMIC_ACQ_REL);
}
#define XH(n) static void intx_h##n(void) { intx_dispatch(n); }
XH(0) XH(1) XH(2) XH(3) XH(4) XH(5) XH(6) XH(7) XH(8) XH(9) XH(10) XH(11) XH(12) XH(13) XH(14) XH(15)
static const irq_handler_t intx_h[16] = { intx_h0, intx_h1, intx_h2, intx_h3, intx_h4, intx_h5, intx_h6, intx_h7, intx_h8, intx_h9, intx_h10, intx_h11, intx_h12, intx_h13, intx_h14, intx_h15 };

static void intx_high(void* ctx) { intx_dispatch((unsigned)(uintptr_t)ctx); }   /* линия с GSI >= 16: вызывается через вектор пула MSI */

/* Устройство за мостами PCI-PCI: линия INTx выбирается по правилу моста (PCI-to-PCI Bridge Architecture, 9.1): на каждом шаге
   вверх номер ножки сдвигается на номер устройства; _PRT спрашивается для устройства на корневой шине. Мосты со своим _PRT - вне этого. */
static void intx_root(const pci_dev_t* d, unsigned* bus, unsigned* dev, unsigned* fn, unsigned* pin) {
    *bus = d->bus; *dev = d->dev; *fn = d->fn;
    for (int hop = 0; hop < 8; hop++) {
        const pci_dev_t* br = 0;
        for (int i = 0; i < pci_count() && !br; i++) {
            const pci_dev_t* c = pci_get(i);
            if (c->class_code == 0x06 && c->subclass == 0x04 && (c->header_type & 0x7F) == 1 &&
                pci_read8(c, 0x19) <= *bus && *bus <= pci_read8(c, 0x1A) && c->bus != *bus) br = c;
        }
        if (!br) return;
        *pin = ((*pin - 1 + *dev) & 3) + 1;
        *bus = br->bus; *dev = br->dev; *fn = br->fn;
    }
}

static int intx_attach(bind_t* b) {
    const pci_dev_t* d = b->dev;
    unsigned pin = pci_read8(d, 0x3D), line = pci_read8(d, 0x3C);
    uint32_t gsi = 0;                                                               /* 0 - линия не определена */
    if (pin && pin <= 4 && irqmode_is_apic()) {
        /* В APIC-режиме верна только таблица _PRT (по правилу моста): регистр 0x3C - маршрут PIC, на q35 и почти на всех ПК он не
           совпадает с GSI IOAPIC. Нет _PRT или нет ответа - берём регистр, если там ISA 3..15. */
        unsigned rb, rd_, rf, rp = pin;
        intx_root(d, &rb, &rd_, &rf, &rp);
        acpidev_irq_t q;
        if (acpidev_pci_irq(rb, rd_, rf, rp, &q) && q.gsi >= 3 && q.gsi < 16 + XH_MAX * 8) {
            gsi = q.gsi;
            if (gsi < 16) pci_write16(d, 0x3C, (uint16_t)((pci_read16(d, 0x3C) & 0xFF00u) | gsi));   /* регистр - только для программ, на устройство не действует */
        } else if (line >= 3 && line < 16) gsi = line;
    }
    if (!pin || pin > 4 || !gsi || !irqmode_is_apic()) return 0;
    unsigned key;                                                                   /* ключ таблицы: ISA 3..15 как есть, GSI >= 16 - 16..23 */
    if (gsi < 16) key = gsi;
    else {
        key = 0;
        uint64_t hf = spin_lock_irqsave(&klock);
        for (unsigned k = 16; k < 16 + XH_MAX; k++) if (xl[k].gsi == gsi) { key = k; break; }
        if (!key) for (unsigned k = 16; k < 16 + XH_MAX; k++) if (!xl[k].reg && !xl[k].gsi) { key = k; xl[k].gsi = gsi; break; }
        spin_unlock_irqrestore(&klock, hf);
        if (!key) return 0;
    }
    if (key < 16 && !xl[key].reg && irq_has_handler(key)) return 0;                 /* линию занял драйвер ядра */
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (uint16_t)(cmd | PCI_CMD_INTX_OFF));
    int can = (pci_read16(d, 0x04) & PCI_CMD_INTX_OFF) != 0;                      /* устройство обязано уметь INTx-disable (PCI 2.3) */
    pci_write16(d, 0x04, (uint16_t)(cmd & ~PCI_CMD_INTX_OFF));
    if (!can) return 0;
    uint64_t f = spin_lock_irqsave(&klock);
    int slot = -1;
    for (int i = 0; i < XL_MAX; i++) if (!xl[key].b[i]) { slot = i; break; }
    if (slot < 0) { spin_unlock_irqrestore(&klock, f); return 0; }
    if (!xl[key].reg) {
        if (key < 16) { if (irq_register_handler((unsigned char)key, intx_h[key]) != 0) { spin_unlock_irqrestore(&klock, f); return 0; } }
        else if (msi_vector_alloc(intx_high, (void*)(uintptr_t)key, &xl[key].vec) != 0) { spin_unlock_irqrestore(&klock, f); return 0; }
        xl[key].reg = 1;
    }
    b->intx_masked = 0; b->intx_line = (int)key;
    __atomic_store_n(&xl[key].b[slot], b, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&klock, f);
    if ((key < 16 ? irqmode_pci_line(key, 1) : irqmode_pci_gsi(xl[key].gsi, xl[key].vec, 1)) != 0) return 0;
    b->intx = 1;
    uart_print("[KIRA DRV] INTx: GSI "); uart_print_dec(gsi); uart_print(gsi >= 16 ? " via IOAPIC vector" : " via ISA line"); uart_print(" (PCI "); uart_print_dec(d->bus); uart_print(":"); uart_print_dec(d->dev); uart_print("."); uart_print_dec(d->fn); uart_print(")\n");
    return 1;
}
static void intx_detach(bind_t* b) {
    if (!b->intx) return;
    unsigned line = (unsigned)b->intx_line;
    pci_write16(b->dev, 0x04, (uint16_t)(pci_read16(b->dev, 0x04) | PCI_CMD_INTX_OFF));
    uint64_t f = spin_lock_irqsave(&klock);
    int left = 0;
    for (int i = 0; i < XL_MAX; i++) { if (xl[line].b[i] == b) __atomic_store_n(&xl[line].b[i], 0, __ATOMIC_RELEASE); else if (xl[line].b[i]) left++; }
    spin_unlock_irqrestore(&klock, f);
    while (__atomic_load_n(&xl[line].active, __ATOMIC_ACQUIRE)) cpu_relax();       /* обработчик на другом ядре мог держать b */
    if (!left) {
        if (line < 16) { (void)irqmode_pci_line(line, 0); irq_unregister_handler(line); xl[line].reg = 0; }
        else (void)irqmode_pci_gsi(xl[line].gsi, xl[line].vec, 0);                  /* вектор пула остаётся за линией: выдаётся один раз */
    }
    b->intx = 0;
}
static void intx_rearm(bind_t* b) {
    if (b->intx && __atomic_exchange_n(&b->intx_masked, 0, __ATOMIC_ACQ_REL))
        pci_write16(b->dev, 0x04, (uint16_t)(pci_read16(b->dev, 0x04) & ~PCI_CMD_INTX_OFF));
}
#else
static int intx_attach(bind_t* b) { (void)b; return 0; }
static void intx_detach(bind_t* b) { (void)b; }
static void intx_rearm(bind_t* b) { (void)b; }
#endif

static void irq_enable(bind_t* b) {
    const pci_dev_t* d = b->dev;
    if (!find_cap(d, 0x05) && !find_cap(d, 0x11)) { b->irq_on = intx_attach(b); return; }   /* ни MSI, ни MSI-X: только линия INTx */
    if (!b->irq_tried) {
        b->irq_tried = 1;
        if (msi_alloc(d, kdrv_irq, b, &b->irq_addr, &b->irq_data) == 0) b->irq_have = 1;
    }
    if (!b->irq_have) return;
    b->irq_on = pci_msi_setup(d, b->irq_addr, b->irq_data) == 0 || pci_msix_setup(d, 0, b->irq_addr, b->irq_data) == 0;
}

/* Устройство останавливается: ни обращений к памяти, ни сообщений прерываний. Не спит (зовётся при гибели процесса). */
static uint16_t quiesce(const pci_dev_t* d) {
    uint16_t c = find_cap(d, 0x05);
    if (c) pci_write16(d, (uint16_t)(c + 2), (uint16_t)(pci_read16(d, (uint16_t)(c + 2)) & ~1u));
    c = find_cap(d, 0x11);
    if (c) pci_write16(d, (uint16_t)(c + 2), (uint16_t)((pci_read16(d, (uint16_t)(c + 2)) & ~0x8000u) | 0x4000u));
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (uint16_t)((cmd & ~(PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER)) | PCI_CMD_INTX_OFF));
    return pci_read16(d, 0x04);
}

/* ---- запуск ---- */
static void bind_reset(bind_t* b) {
    b->granted = b->version = b->pid = 0;
    b->irq_count = 0; b->irq_seen = 0; b->irq_on = 0; b->intx = 0; b->intx_masked = 0; b->waiting = 0; b->tx_pending = 0; b->denied = 0;
    b->nmap = 0; b->mmio_next = MMIO_VA; b->ndma = 0; b->dma_pages = 0; b->dma_next = DMA_VA;
    b->net_alive = 0; b->ctl = 0; b->net_pa = 0; b->net_pages = 0; b->tx_head = b->rx_tail = 0;
    b->net_tx = b->net_rx = b->net_bad = 0;
    b->blk_alive = 0; b->blk_pending = 0; b->blk_reg_pending = 0; b->bctl = 0; b->blk_pa = 0; b->blk_pages = 0; b->req_head = b->rsp_tail = 0;
    b->blk_flags = 0; b->blk_sectors = 0; b->blk_id[0] = 0; b->blk_ops = b->blk_bad = 0; kdrv_blk_track_init(&b->trk);
    b->floor_pending = 0; b->started_at = 0;
}

static int start(const char* name, const pci_dev_t* d, const char* args) {
    pkg_t p;
    int rc = pkg_load(name, &p);
    if (rc != KDRV_OK) {
        uart_print("[KIRA DRV] "); uart_print(name);
        if (rc == KDRV_E_HASH) { uart_print(": SHA-256 не совпал с записью /drv/"); uart_print(name); uart_print(".rec"); }
        else if (rc == KDRV_E_ROLLBACK) {
            uart_print(": откат запрещён - версия "); uart_print_dec(p.h.version); uart_print(" ниже пола "); uart_print_dec(kdrv_state_floor_of(&gst, name));
            uart_print(" (/var/kdrv_state; поднять пол: drv floor ИМЯ N)");
        }
        else if (rc == KDRV_E_SIGNATURE) { uart_print(": подпись издателя отклонена ("); uart_print(kdrv_sig_strerror(p.sig_error)); uart_print(")"); }
        else { uart_print(": пакет отклонён ("); uart_print(kdrv_strerror(rc)); uart_print(")"); }
        uart_print(" - драйвер НЕ запущен\n");
        life_note("drv-denied", name, rc);
        return rc;
    }
    if (!kdrv_match_dev(&p.h, d->vendor, d->device, cls_of(d))) { kfree(p.buf); uart_print("[KIRA DRV] "); uart_print(name); uart_print(": не подходит этому устройству\n"); return -1; }
    uint32_t granted = p.h.caps & p.r.caps_ok;                                      /* запрошено в пакете И подтверждено пользователем */
    /* Fail closed: DMA и MSI требуют bus mastering и без IOMMU могут записывать в произвольную физическую память.
       Ring 3 драйвер с DMA/IRQ запускается только когда iommu_device_isolated() истинно для ЭТОГО устройства
       (VT-d: свой домен, устройство видит лишь страницы, выданные через sys_dma_alloc и кольца сети). */
    uint32_t policy_caps = granted;
    if ((granted & CAP_IRQ) && !find_cap(d, 0x05) && !find_cap(d, 0x11)) policy_caps &= ~(uint32_t)CAP_IRQ;   /* только INTx: устройство в память не пишет, изоляция для прерывания не нужна (bus mastering даёт лишь CAP_DMA) */
    if (policy_caps & (CAP_DMA | CAP_IRQ)) (void)iommu_isolate_device(d);      /* свой домен VT-d без отображений; отказ - устройство остаётся неизолированным */
    if (!kdrv_dma_policy_allow(policy_caps, iommu_device_isolated(d))) {
        kfree(p.buf);
        uart_print("[KIRA DRV] "); uart_print(name);
        uart_print(": DMA/MSI запрещены — изоляция IOMMU для устройства не настроена; драйвер НЕ запущен\n");
        life_note("drv-denied-dma", name, KDRV_E_DMA_UNSAFE);
        return KDRV_E_DMA_UNSAFE;
    }
    if ((granted & CAP_DMA) && platform_dma_noncoherent && platform_dma_noncoherent()) {
        kfree(p.buf);
        uart_print("[KIRA DRV] "); uart_print(name); uart_print(": плата без согласованности кэша (пул DMA без кэша) - DMA драйверам Ring 3 здесь не отдаётся, драйвер НЕ запущен\n");
        return -1;
    }
    bind_t* b = find_slot(d, 1);
    if (!b) { kfree(p.buf); uart_print("[KIRA DRV] нет свободной привязки (устройств с драйверами не больше 8)\n"); return -1; }
    uint64_t f = spin_lock_irqsave(&klock);
    int st = b->state;
    if (st == B_FREE) b->state = B_RUN;
    spin_unlock_irqrestore(&klock, f);
    if (st != B_FREE) { kfree(p.buf); uart_print("[KIRA DRV] устройство занято драйвером или ещё освобождается\n"); return -1; }
    bind_reset(b);
    unsigned k = 0; while (p.h.name[k] && k < sizeof b->name - 1) { b->name[k] = p.h.name[k]; k++; }
    b->name[k] = 0;
    b->granted = granted; b->version = p.h.version;
    if (!(policy_caps & (CAP_DMA | CAP_IRQ))) {      /* прошивка могла оставить bus mastering и MSI включёнными: драйвер без DMA/IRQ их не получает */
        uint16_t mc = find_cap(d, 0x05);
        if (mc) pci_write16(d, (uint16_t)(mc + 2), (uint16_t)(pci_read16(d, (uint16_t)(mc + 2)) & ~1u));
        mc = find_cap(d, 0x11);
        if (mc) pci_write16(d, (uint16_t)(mc + 2), (uint16_t)((pci_read16(d, (uint16_t)(mc + 2)) & ~0x8000u) | 0x4000u));
        pci_write16(d, 0x04, (uint16_t)(pci_read16(d, 0x04) & ~PCI_CMD_MASTER));
    }
    if (granted & CAP_MMIO) pci_enable(d, PCI_CMD_MEM | PCI_CMD_IO);
    /* Эти действия достижимы только после успешной проверки IOMMU policy выше. */
    if (policy_caps & (CAP_DMA | CAP_IRQ)) pci_enable(d, PCI_CMD_MASTER);
    if (granted & CAP_IRQ) irq_enable(b);
    char pname[16] = "drv-";
    for (k = 0; b->name[k]; k++) pname[4 + k] = b->name[k];
    pname[4 + k] = 0;
    int pid = proc_spawn_driver(pname, p.buf + sizeof(kdrv_header_t), p.h.elf_size, granted, (uint32_t)(b - binds), args);
    kfree(p.buf);
    if (pid < 0) {
        quiesce(d);
        b->state = B_FREE;
        uart_print("[KIRA DRV] "); uart_print(name); uart_print(": ELF драйвера не принят - не запущен\n");
        return -1;
    }
    b->pid = (uint32_t)pid;
    b->started_at = timer_get_ticks(); b->floor_pending = 1;
    /* строка собирается целиком и печатается одним вызовом: драйвер уже работает и пишет в тот же порт */
    char ln[320]; unsigned n = 0;
    #define LN_ADD(str) do { const char* _s = (str); while (*_s && n < sizeof ln - 1) ln[n++] = *_s++; } while (0)
    static const char hx[] = "0123456789abcdef";
    char bdf[10] = { hx[d->bus >> 4], hx[d->bus & 15], ':', hx[d->dev >> 4], hx[d->dev & 15], '.', (char)('0' + (d->fn & 7)), 0 };
    char vid[10] = { hx[d->vendor >> 12], hx[(d->vendor >> 8) & 15], hx[(d->vendor >> 4) & 15], hx[d->vendor & 15], ':',
                     hx[d->device >> 12], hx[(d->device >> 8) & 15], hx[(d->device >> 4) & 15], hx[d->device & 15], 0 };
    char pidtxt[12]; { unsigned v = (unsigned)pid, l = 0; char t[12]; do { t[l++] = (char)('0' + v % 10); v /= 10; } while (v);
                       for (unsigned i = 0; i < l; i++) pidtxt[i] = t[l - 1 - i]; pidtxt[l] = 0; }
    LN_ADD("[KIRA DRV] "); LN_ADD(name); LN_ADD(": устройство "); LN_ADD(bdf); LN_ADD(" "); LN_ADD(vid);
    LN_ADD(" -> драйвер запущен как "); LN_ADD(pname); LN_ADD(" (pid "); LN_ADD(pidtxt); LN_ADD("), SHA-256 сверен, права:");
    for (uint32_t bit = 1; bit; bit <<= 1) if ((granted & bit) && (bit & CAP_DRIVER_)) { LN_ADD(" "); LN_ADD(caps_bit_name(bit)); }
    if (!(granted & CAP_DRIVER_)) LN_ADD(" -");
    LN_ADD(p.auth == KDRV_AUTH_PUBLISHER ? ", подпись издателя Ed25519 подтверждена" : ", издатель не подтверждён (локально)");
    LN_ADD(b->irq_on ? ", прерывание MSI включено" : (granted & CAP_IRQ) ? ", прерывания у устройства нет" : ""); LN_ADD("\n");
    #undef LN_ADD
    ln[n] = 0; uart_print(ln);
    life_note("drv-start", name, pid);
    return pid;
}

typedef struct { char names[16][16]; unsigned n; } list_t;
static int list_cb(void* c, const char* nm, const kfs_stat_t* st) {
    list_t* l = (list_t*)c;
    unsigned len = 0; while (nm[len]) len++;
    if (st->type != KFS_FILE || len < 6 || l->n >= 16) return 0;
    if (!streq(nm + len - 5, ".kdrv")) return 0;
    char stem[16]; unsigned i = 0;
    for (; i < len - 5 && i < 15; i++) stem[i] = nm[i];
    stem[i] = 0;
    if (!name_valid(stem)) return 0;
    for (i = 0; stem[i]; i++) l->names[l->n][i] = stem[i];
    l->names[l->n][i] = 0; l->n++;
    return 0;
}
static int list_pkgs(list_t* l) { l->n = 0; return kfs_list("/drv", list_cb, l); }

/* Дешёвая проверка «этот пакет вообще про такое устройство» по заголовку; полная проверка с хешами - в start(). */
static int pkg_wants(const char* name, const pci_dev_t* d) {
    char path[48]; path_of(path, name, ".kdrv");
    kdrv_header_t h; uint8_t raw[sizeof(kdrv_header_t)];
    if (kfs_read(path, 0, raw, sizeof raw) != (int64_t)sizeof raw) return 0;
    for (unsigned i = 0; i < sizeof raw; i++) ((uint8_t*)&h)[i] = raw[i];
    if (h.magic != KDRV_MAGIC || h.format != KDRV_FORMAT || h.hdr_size != sizeof h || h.nmatch < 1 || h.nmatch > KDRV_MATCH_MAX) return 0;
    return kdrv_match_dev(&h, d->vendor, d->device, cls_of(d));
}

static void scan(void) {
    if (!kfs_mounted() || !pci_count()) return;
    list_t* l = (list_t*)kmalloc(sizeof *l);
    if (!l) return;
    int started = 0;
    if (list_pkgs(l) == 0 && l->n) {
        for (int i = 0; i < pci_count(); i++) {
            const pci_dev_t* d = pci_get(i);
            if (builtin_owned(d)) continue;
            bind_t* b = find_slot(d, 0);
            if (b && b->state != B_FREE) continue;
            for (unsigned k = 0; k < l->n; k++) {
                if (!pkg_wants(l->names[k], d)) continue;
                if (start(l->names[k], d, 0) > 0) { started++; break; }
            }
        }
    }
    kfree(l);
    uart_print("[KIRA DRV] drvd: устройства PCI без встроенного драйвера просмотрены, драйверов Ring 3 запущено: ");
    uart_print_dec((uint64_t)started); uart_print("\n");
}

/* ---- гибель драйвера ---- */
void kdrv_proc_gone(uint32_t slot, uint32_t pid) {
    if (slot >= KDRV_SLOTS) return;
    bind_t* b = &binds[slot];
    if (b->state != B_RUN) return;
    uint64_t f = spin_lock_irqsave(&b->nlk);
    b->net_alive = 0;                               /* карта драйвера для стека: link down, send отказывает, recv пуст */
    spin_unlock_irqrestore(&b->nlk, f);
    f = spin_lock_irqsave(&b->blk_lk);
    b->blk_alive = 0;                               /* диск драйвера: операции получают -5, общую память ядро больше не трогает */
    spin_unlock_irqrestore(&b->blk_lk, f);
    irqwait_fire(&b->blk_iw);                       /* ядро, ждущее ответа драйвера, просыпается и видит, что драйвера нет */
    uint16_t cmd = quiesce(b->dev);
    b->dead_at = timer_get_ticks();
    f = spin_lock_irqsave(&klock);
    b->state = B_DEAD;
    spin_unlock_irqrestore(&klock, f);
    __atomic_add_fetch(&ndead, 1, __ATOMIC_ACQ_REL);
    uart_print("[KIRA DRV] drv-"); uart_print(b->name); uart_print(" (pid "); uart_print_dec(pid); uart_print(") завершён: устройство ");
    print_bdf(b->dev); uart_print(" остановлено (команда PCI "); uart_print_hex(cmd); uart_print(", MSI выключено), DMA ");
    uart_print_dec(b->dma_pages); uart_print(" стр. в "); uart_print_dec(b->ndma); uart_print(" блок(ах) и кольца ");
    uart_print_dec(b->net_pages + b->blk_pages); uart_print(" стр. вернутся через 100 мс\n");
}

static void release(bind_t* b) {
    intx_detach(b);
    uint32_t got = 0, want = b->dma_pages + b->net_pages + b->blk_pages;
    iommu_unmap_all(b->dev);                       /* сначала закрыть доступ устройства, потом отдавать страницы */
    for (uint32_t i = 0; i < b->ndma; i++) if (pmm_free_contiguous(b->dma[i].pa, b->dma[i].pages) == 0) got += b->dma[i].pages;
    if (b->net_pages && pmm_free_contiguous(b->net_pa, b->net_pages) == 0) got += b->net_pages;
    if (b->blk_pages && pmm_free_contiguous(b->blk_pa, b->blk_pages) == 0) got += b->blk_pages;
    uart_print("[KIRA DRV] "); uart_print(b->name); uart_print(": возвращено памяти DMA и колец "); uart_print_dec(got); uart_print(" из ");
    uart_print_dec(want); uart_print(" стр., отображений устройства и DMA снято "); uart_print_dec(b->nmap + b->ndma);
    uart_print(got == want ? ", утечек нет\n" : ", УТЕЧКА\n");
    b->ndma = 0; b->dma_pages = 0; b->net_pages = 0; b->blk_pages = 0; b->nmap = 0; b->ctl = 0; b->bctl = 0;
    uint64_t f = spin_lock_irqsave(&klock);
    b->state = B_FREE;                              /* устройство снова можно отдать драйверу */
    spin_unlock_irqrestore(&klock, f);
    __atomic_sub_fetch(&ndead, 1, __ATOMIC_ACQ_REL);
}

static int rescan_req;
void kdrv_request_rescan(void) { __atomic_store_n(&rescan_req, 1, __ATOMIC_RELEASE); }

/* Запросы окна «Драйверы» (fsrv.c, FS_OP_DRV_START/STOP): кольцо из 4 записей. Исполняет основной цикл теми же командами консоли
   (drv start/stop), поэтому правила и сообщения журнала одинаковы; запрашивающий видит итог по списку PCI. */
#define CTL_Q 4
static struct { int op; char name[16]; } ctlq[CTL_Q];
static unsigned ctl_head, ctl_tail;
static spinlock_t ctl_lock = SPINLOCK_INIT;

int kdrv_ctl_request(int op, const char* name) {
    if ((op != KDRV_CTL_START && op != KDRV_CTL_STOP) || !name_valid(name)) return -1;
    uint64_t f = spin_lock_irqsave(&ctl_lock);
    int rc = -1;
    if (ctl_head - ctl_tail < CTL_Q) {
        unsigned i = ctl_head % CTL_Q, k = 0;
        ctlq[i].op = op;
        while (name[k] && k < sizeof ctlq[i].name - 1) { ctlq[i].name[k] = name[k]; k++; }
        ctlq[i].name[k] = 0;
        ctl_head++;
        rc = 0;
    }
    spin_unlock_irqrestore(&ctl_lock, f);
    return rc;
}

static void ctl_run(void) {
    for (;;) {
        char cmd[32]; unsigned n = 0;
        uint64_t f = spin_lock_irqsave(&ctl_lock);
        int have = ctl_tail != ctl_head;
        if (have) {
            unsigned i = ctl_tail % CTL_Q;
            const char* v = ctlq[i].op == KDRV_CTL_START ? "start " : "stop ";
            while (*v) cmd[n++] = *v++;
            for (unsigned k = 0; ctlq[i].name[k] && n < sizeof cmd - 1; k++) cmd[n++] = ctlq[i].name[k];
            ctl_tail++;
        }
        spin_unlock_irqrestore(&ctl_lock, f);
        if (!have) return;
        cmd[n] = 0;
        kdrv_cmd(cmd);
    }
}

/* Пол версий растёт только после УСПЕШНОГО запуска: драйвер проработал KDRV_STABLE_TICKS и не умер. Упавший сразу пол не поднимает. */
static void floor_commit_due(void) {
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        bind_t* b = &binds[i];
        if (!b->floor_pending) continue;
        if (b->state != B_RUN) { b->floor_pending = 0; continue; }
        if (timer_get_ticks() - b->started_at < KDRV_STABLE_TICKS) continue;
        b->floor_pending = 0;
        kmutex_lock(&state_mx);
        state_load_locked();
        int r = gst.status == KDRV_STATE_OK ? kdrv_state_raise_floor(&gst, b->name, b->version) : KDRV_STATE_E_CORRUPT;
        if (r > 0) { uart_print("[KIRA DRV] "); uart_print(b->name); uart_print(": пол версий поднят до "); uart_print_dec(b->version); uart_print(" (драйвер работает стабильно)\n"); }
        else if (r < 0) { uart_print("[KIRA DRV] "); uart_print(b->name); uart_print(": пол версий НЕ поднят (таблица полов полна или состояние недоступно)\n"); }
        (void)state_save_locked();
        kmutex_unlock(&state_mx);
    }
}

/* Сон S3: драйвер Ring 3 владеет своим устройством (регистры, DMA), ядро не может ни остановить его, ни восстановить после сна. Возвращает
   число работающих драйверов и имя первого (name - буфер на 16 байт, может быть 0): сон отказывается, пока такой драйвер есть. */
unsigned kdrv_active_count(char* name) {
    unsigned n = 0;
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        if (!binds[i].dev || binds[i].state == B_FREE) continue;
        if (!n && name) { unsigned k = 0; for (; k < 15 && binds[i].name[k]; k++) name[k] = binds[i].name[k]; name[k] = 0; }
        n++;
    }
    return n;
}

void kdrv_poll(void) {
    static int scanned;
    if (!scanned) { scanned = 1; scan(); }
    else if (__atomic_exchange_n(&rescan_req, 0, __ATOMIC_ACQ_REL)) scan();               /* drvget установил пакет */
    ctl_run();                                                                             /* окно «Драйверы»: запуск и остановка по просьбе */
    blk_reg_run();                                                                         /* диски драйверов: заводятся здесь, когда драйвер уже обслуживает кольца */
    floor_commit_due();
    if (!__atomic_load_n(&ndead, __ATOMIC_ACQUIRE)) return;
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        bind_t* b = &binds[i];
        if (b->state == B_DEAD && timer_get_ticks() - b->dead_at >= RELEASE_DELAY_TICKS) release(b);
    }
}

/* ---- сетевая карта драйвера: кольца в общей памяти ---- */
static int nd_send(netdev_t* d, const void* frame, uint32_t len) {
    bind_t* b = (bind_t*)d->priv;
    if (len < 14 || len > KDRV_NET_MAXFRAME) return -1;
    uint64_t f = spin_lock_irqsave(&b->nlk);
    if (!b->net_alive) { spin_unlock_irqrestore(&b->nlk, f); return -1; }
    uint32_t tail = __atomic_load_n(&b->ctl->tx_tail, __ATOMIC_ACQUIRE);          /* читаем у драйвера: данные недоверенные */
    if (b->tx_head - tail > KDRV_NET_SLOTS) { b->net_bad++; spin_unlock_irqrestore(&b->nlk, f); return -1; }   /* tail в будущем: считаем полным */
    if (b->tx_head - tail == KDRV_NET_SLOTS) { spin_unlock_irqrestore(&b->nlk, f); return -1; }
    kdrv_slot_t* s = (kdrv_slot_t*)((uint8_t*)b->ctl + KDRV_NET_TX_OFF) + (b->tx_head % KDRV_NET_SLOTS);
    const uint8_t* src = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) s->data[i] = src[i];
    s->len = len;
    b->tx_head++;
    __atomic_store_n(&b->ctl->tx_head, b->tx_head, __ATOMIC_RELEASE);
    b->net_tx++;
    spin_unlock_irqrestore(&b->nlk, f);
    __atomic_store_n(&b->tx_pending, 1, __ATOMIC_RELEASE);
    irqwait_fire(&b->iw);                                                          /* звонок драйверу */
    return 0;
}

static int nd_recv(netdev_t* d, void* buf, uint32_t max) {
    bind_t* b = (bind_t*)d->priv;
    uint64_t f = spin_lock_irqsave(&b->nlk);
    int n = 0;
    while (b->net_alive) {
        uint32_t head = __atomic_load_n(&b->ctl->rx_head, __ATOMIC_ACQUIRE);
        uint32_t avail = head - b->rx_tail;
        if (avail == 0) break;
        if (avail > KDRV_NET_SLOTS) { b->net_bad++; b->rx_tail = head; __atomic_store_n(&b->ctl->rx_tail, b->rx_tail, __ATOMIC_RELEASE); break; }   /* head вне кольца: драйвер врёт */
        kdrv_slot_t* s = (kdrv_slot_t*)((uint8_t*)b->ctl + KDRV_NET_RX_OFF) + (b->rx_tail % KDRV_NET_SLOTS);
        uint32_t len = s->len;                                                      /* один раз: дальше пользуемся копией */
        int ok = len >= 14 && len <= KDRV_NET_MAXFRAME && len <= max;
        if (ok) { uint8_t* o = (uint8_t*)buf; for (uint32_t i = 0; i < len; i++) o[i] = s->data[i]; n = (int)len; b->net_rx++; }
        else b->net_bad++;
        b->rx_tail++;
        __atomic_store_n(&b->ctl->rx_tail, b->rx_tail, __ATOMIC_RELEASE);
        if (ok) break;
    }
    spin_unlock_irqrestore(&b->nlk, f);
    return n;
}

static int nd_link(netdev_t* d) {
    bind_t* b = (bind_t*)d->priv;
    uint64_t f = spin_lock_irqsave(&b->nlk);
    int l = b->net_alive && __atomic_load_n(&b->ctl->link, __ATOMIC_ACQUIRE) == 1;
    spin_unlock_irqrestore(&b->nlk, f);
    return l;
}

static int64_t sys_netdev(bind_t* b, uint64_t op, uint64_t a1) {
    if (op == KDRV_NET_KICK) {
        if (!b->net_alive) return KE_NOENT;
        irqwait_fire(&b->net_iw);
        return 0;
    }
    if (op != KDRV_NET_REGISTER) return KE_ARG;
    uint8_t mac[6];
    if (copy_from_user(mac, a1, 6) != 0) return KE_ARG;
    if (mac[0] & 1) return KE_ARG;                                                  /* адрес карты - не групповой */
    netdev_t* cur = net_dev();
    if (cur && cur != &b->nd) return KE_EXIST;                                      /* стек работает с одной картой, она уже есть */
    proc_space_lock();
    int64_t rc = 0;
    uint32_t pages = KDRV_NET_BYTES / 4096;
    if (b->ctl) rc = KE_EXIST;
    uint64_t pa = rc ? 0 : pmm_alloc_contiguous(pages);
    if (!rc && !pa) rc = KE_NOMEM;
    uint64_t va = NET_VA;
    if (!rc) {
        uint8_t* z = (uint8_t*)(uintptr_t)pa;
        for (uint64_t i = 0; i < (uint64_t)pages * 4096; i++) z[i] = 0;
        kdrv_netctl_t* c = (kdrv_netctl_t*)(uintptr_t)pa;
        c->slots = KDRV_NET_SLOTS; c->slot_size = KDRV_NET_SLOT; c->max_frame = KDRV_NET_MAXFRAME;
        if (proc_map_phys(va, pa, pages, VMM_WRITE | VMM_NOEXEC) != 0) { pmm_free_contiguous(pa, pages); rc = KE_NOMEM; }
    }
    if (!rc) {
        uint64_t f = spin_lock_irqsave(&b->nlk);
        b->ctl = (kdrv_netctl_t*)(uintptr_t)pa; b->net_pa = pa; b->net_pages = pages;
        b->tx_head = 0; b->rx_tail = 0; b->net_alive = 1;
        spin_unlock_irqrestore(&b->nlk, f);
        for (int i = 0; i < 6; i++) b->nd.mac[i] = mac[i];
        unsigned k = 0; while (b->name[k] && k < sizeof b->nd.name - 1) { b->nd.name[k] = b->name[k]; k++; }
        b->nd.name[k] = 0;
        b->nd.send = nd_send; b->nd.recv = nd_recv; b->nd.link = nd_link; b->nd.priv = b; b->nd.iw = &b->net_iw; b->nd.mcast_add = 0;
        rc = (int64_t)va;
    }
    proc_space_unlock();
    if (rc > 0 && !b->nd_registered) { b->nd_registered = 1; net_register(&b->nd); }   /* последним: сетевой поток начнёт работать сразу */
    return rc;
}

/* ---- диск драйвера: кольца запросов в общей памяти (kdrv_blk.h) ---- */
#define BLK_TIMEOUT_MS 10000u                      /* драйвер не ответил за это время: он завис, диск отключается, драйвер останавливается */

static void blk_copy(void* d, const void* s, uint32_t n) {
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    while (n >= 8 && !(((uintptr_t)a | (uintptr_t)b) & 7)) { *(volatile uint64_t*)a = *(const volatile uint64_t*)b; a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
}
static uint8_t* blk_slot_data(kdrv_blkctl_t* c, unsigned slot) { return (uint8_t*)c + KDRV_BLK_DATA_OFF + (uint64_t)slot * KDRV_BLK_SLOT_BYTES; }

static int blk_lock(bind_t* b) {
    if (sched_can_sleep()) { kmutex_lock(&b->blk_mx); return 0; }
    blk_wait_t w; blk_wait_begin(&w, 2000);          /* выключение: спать нельзя, ждём короткое время */
    while (!kmutex_trylock(&b->blk_mx)) if (!blk_wait_step(&w)) return -4;
    return 0;
}

/* Одна операция над диском драйвера. Большая передача режется на куски по буферам данных, до KDRV_BLK_SLOTS кусков одновременно в полёте.
   Всё, что пришло из общей памяти (head колец, номера и статусы ответов), недоверенно: head вне кольца - пересинхронизация, ответ с неизвестным
   номером - отброшен и посчитан, статус - только «0 или ошибка». Общая память трогается только под blk_lk и только пока драйвер жив:
   гибель драйвера (kdrv_proc_gone) берёт тот же замок, а страницы возвращаются не раньше чем через 100 мс. */
static int bd_xfer(bind_t* b, uint32_t op, uint64_t lba, uint32_t count, void* rbuf, const void* wbuf) {
    if (!__atomic_load_n(&b->blk_alive, __ATOMIC_ACQUIRE)) return -5;
    if (op == KDRV_BLK_OP_WRITE && (b->blk_flags & KDRV_BLK_F_RO)) return -6;
    if (op == KDRV_BLK_OP_FLUSH && !(b->blk_flags & KDRV_BLK_F_FLUSH)) return 0;          /* кэша записи нет: запись уже долговечна */
    if (op != KDRV_BLK_OP_FLUSH) {
        if (lba > b->blk_sectors || count > b->blk_sectors - lba) return -1;                /* выход за границы: драйвер этого не увидит */
        if (count == 0) return 0;
    }
    if (blk_lock(b)) return -4;
    uint64_t next = lba, end = lba + count;
    const uint8_t* wp = (const uint8_t*)wbuf; uint8_t* rp = (uint8_t*)rbuf;
    uint8_t* prb[KDRV_BLK_SLOTS]; uint32_t pn[KDRV_BLK_SLOTS];
    for (unsigned i = 0; i < KDRV_BLK_SLOTS; i++) { prb[i] = 0; pn[i] = 0; }
    int err = 0, flush_posted = 0;
    blk_wait_t w; blk_wait_begin(&w, BLK_TIMEOUT_MS); blk_wait_irq(&w, &b->blk_iw);
    for (;;) {
        int posted = 0, finished = 0;
        uint64_t f = spin_lock_irqsave(&b->blk_lk);
        if (!b->blk_alive || !b->bctl) { spin_unlock_irqrestore(&b->blk_lk, f); err = -5; break; }
        kdrv_blkctl_t* c = b->bctl;
        uint32_t head = __atomic_load_n(&c->rsp_head, __ATOMIC_ACQUIRE);
        int av = kdrv_blk_ring_avail(head, b->rsp_tail, KDRV_BLK_SLOTS);
        if (av < 0) { b->blk_bad++; b->rsp_tail = head; __atomic_store_n(&c->rsp_tail, head, __ATOMIC_RELEASE); av = 0; }      /* head вне кольца: драйвер врёт */
        while (av-- > 0) {
            volatile kdrv_blkrsp_t* rs = &c->rsp[b->rsp_tail % KDRV_BLK_SLOTS];
            uint32_t rid = rs->id; int32_t rst = rs->status;                          /* один раз: дальше пользуемся копией */
            b->rsp_tail++;
            __atomic_store_n(&c->rsp_tail, b->rsp_tail, __ATOMIC_RELEASE);
            int sl = kdrv_blk_track_complete(&b->trk, rid);
            if (sl < 0) { b->blk_bad++; continue; }                                   /* такого запроса в полёте нет */
            if (rst != 0) { if (!err) err = -3; }
            else if (prb[sl]) blk_copy(prb[sl], blk_slot_data(c, (unsigned)sl), pn[sl] * KDRV_BLK_SECTOR);
            prb[sl] = 0;
        }
        while (!err && b->trk.inflight < KDRV_BLK_SLOTS) {
            kdrv_blkreq_t q; uint8_t* z = (uint8_t*)&q; for (unsigned i = 0; i < sizeof q; i++) z[i] = 0;
            uint32_t n = 0;
            if (op == KDRV_BLK_OP_FLUSH) { if (flush_posted) break; q.op = KDRV_BLK_OP_FLUSH; }
            else {
                if (next >= end) break;
                n = end - next > KDRV_BLK_SLOT_SECTORS ? KDRV_BLK_SLOT_SECTORS : (uint32_t)(end - next);
                q.op = op; q.lba = next; q.count = n;
            }
            uint32_t rt = __atomic_load_n(&c->req_tail, __ATOMIC_ACQUIRE);
            if (b->req_head - rt >= KDRV_BLK_SLOTS) { if (b->req_head - rt > KDRV_BLK_SLOTS) b->blk_bad++; break; }   /* кольцо запросов полно (или tail вне кольца) */
            uint32_t id;
            int sl = kdrv_blk_track_alloc(&b->trk, &id);
            if (sl < 0) break;
            q.id = id; q.slot = (uint32_t)sl;
            if (kdrv_blk_req_check(&q, b->blk_sectors, b->blk_flags) != KDRV_BLK_OK) { (void)kdrv_blk_track_complete(&b->trk, id); err = -1; break; }   /* страховка: ядро само не шлёт плохих запросов */
            if (op == KDRV_BLK_OP_WRITE) { blk_copy(blk_slot_data(c, (unsigned)sl), wp, n * KDRV_BLK_SECTOR); wp += (uint64_t)n * KDRV_BLK_SECTOR; }
            prb[sl] = op == KDRV_BLK_OP_READ ? rp : 0; pn[sl] = n;
            if (op == KDRV_BLK_OP_READ) rp += (uint64_t)n * KDRV_BLK_SECTOR;
            volatile kdrv_blkreq_t* slot = &c->req[b->req_head % KDRV_BLK_SLOTS];
            slot->id = q.id; slot->op = q.op; slot->lba = q.lba; slot->count = q.count; slot->slot = q.slot; slot->pad = 0;
            b->req_head++;
            __atomic_store_n(&c->req_head, b->req_head, __ATOMIC_RELEASE);
            posted = 1; next += n; if (op == KDRV_BLK_OP_FLUSH) flush_posted = 1;
        }
        finished = b->trk.inflight == 0 && (err || (op == KDRV_BLK_OP_FLUSH ? flush_posted : next >= end));
        spin_unlock_irqrestore(&b->blk_lk, f);
        if (posted) { b->blk_ops++; __atomic_store_n(&b->blk_pending, 1, __ATOMIC_RELEASE); irqwait_fire(&b->iw); }   /* звонок драйверу */
        if (finished) break;
        if (!blk_wait_step(&w)) {                                                     /* драйвер не ответил в срок */
            f = spin_lock_irqsave(&b->blk_lk);
            b->blk_alive = 0;
            spin_unlock_irqrestore(&b->blk_lk, f);
            uart_print("[KIRA DRV] drv-"); uart_print(b->name); uart_print(": диск "); uart_print(b->blk_id); uart_print(" - драйвер не ответил за ");
            uart_print_dec(BLK_TIMEOUT_MS / 1000); uart_print(" с, диск отключён, драйвер останавливается\n");
            proc_kill(b->pid);
            err = -4;
            break;
        }
    }
    kmutex_unlock(&b->blk_mx);
    return err;
}

static int bd_read(blk_dev_t* d, uint64_t lba, uint32_t n, void* buf) { return bd_xfer((bind_t*)d->priv, KDRV_BLK_OP_READ, lba, n, buf, 0); }
static int bd_write(blk_dev_t* d, uint64_t lba, uint32_t n, const void* buf) { return bd_xfer((bind_t*)d->priv, KDRV_BLK_OP_WRITE, lba, n, 0, buf); }
static int bd_flush(blk_dev_t* d) { return bd_xfer((bind_t*)d->priv, KDRV_BLK_OP_FLUSH, 0, 0, 0, 0); }

static void deny_note(bind_t* b, const char* what, uint64_t v);

static int64_t sys_blkdev(bind_t* b, uint64_t op, uint64_t a1) {
    if (op == KDRV_BLK_KICK) {
        if (!__atomic_load_n(&b->blk_alive, __ATOMIC_ACQUIRE)) return KE_NOENT;
        irqwait_fire(&b->blk_iw);                                                    /* ответы лежат в кольце rsp: ядро, ждущее ответа, проснётся */
        return 0;
    }
    if (op != KDRV_BLK_REGISTER) return KE_ARG;
    kdrv_blkreg_t r;
    if (copy_from_user(&r, a1, sizeof r) != 0) return KE_ARG;
    int e = kdrv_blk_reg_check(&r);
    if (e != KDRV_BLK_OK) { deny_note(b, "регистрация диска с неверными параметрами, код", (uint64_t)(-e)); return KE_ARG; }
    if (b->bctl) return KE_EXIST;                                                    /* один диск на драйвер */
    if (blk_r3_check(r.name, r.sectors) != 0) return KE_EXIST;                       /* имя занято другим диском или диск с этим именем был другой ёмкости */
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        bind_t* o = &binds[i];
        if (o != b && o->blk_alive && streq(o->blk_id, r.name)) return KE_EXIST;      /* имя занято живым драйвером */
    }
    int dma = (b->granted & CAP_DMA) != 0;
    uint32_t pages = KDRV_BLK_BYTES / 4096;
    uint64_t data_bytes = (uint64_t)KDRV_BLK_SLOTS * KDRV_BLK_SLOT_BYTES;
    proc_space_lock();
    int64_t rc = 0;
    uint64_t pa = dma && r.dma_bits ? pmm_alloc_contiguous_below(pages, 1ull << r.dma_bits) : pmm_alloc_contiguous(pages);
    if (!pa) rc = KE_NOMEM;
    if (!rc && dma) {                                                                /* устройству нужен тот же адрес, что виден процессору, и в пределах его шины */
        uint64_t t;
        if (vmm_translate(pa, &t) != 0 || t != pa || vmm_translate(pa + (uint64_t)(pages - 1) * 4096, &t) != 0 || t != pa + (uint64_t)(pages - 1) * 4096) rc = KE_NOMEM;
        else if (r.dma_bits && pa + (uint64_t)pages * 4096 > (1ull << r.dma_bits)) rc = KE_NOMEM;
        if (rc) { pmm_free_contiguous(pa, pages); pa = 0; }
    }
    int iommu = 0;
    if (!rc) {
        uint8_t* z = (uint8_t*)(uintptr_t)pa;
        for (uint64_t i = 0; i < (uint64_t)pages * 4096; i++) z[i] = 0;               /* чужих данных драйвер не получит */
        kdrv_blkctl_t* c = (kdrv_blkctl_t*)(uintptr_t)pa;
        c->slots = KDRV_BLK_SLOTS; c->slot_sectors = KDRV_BLK_SLOT_SECTORS; c->flags = r.flags; c->sector_size = r.sector_size; c->sectors = r.sectors;
        c->data_phys = dma ? pa + KDRV_BLK_DATA_OFF : 0;                              /* адрес для устройства - только у драйвера с CAP_DMA */
        iommu = dma && iommu_device_isolated(b->dev);
        /* Устройство в своём домене VT-d видит только буферы данных: страница с кольцами ему закрыта. */
        if (iommu && iommu_map(b->dev, pa + KDRV_BLK_DATA_OFF, pa + KDRV_BLK_DATA_OFF, data_bytes, IOMMU_R | IOMMU_W) != 0) { pmm_free_contiguous(pa, pages); rc = KE_NOMEM; }
        else if (proc_map_phys(BLK_VA, pa, pages, VMM_WRITE | VMM_NOEXEC) != 0) {
            if (iommu) iommu_unmap(b->dev, pa + KDRV_BLK_DATA_OFF, data_bytes);
            pmm_free_contiguous(pa, pages); rc = KE_NOMEM;
        }
    }
    if (!rc) {
        uint64_t f = spin_lock_irqsave(&b->blk_lk);
        b->bctl = (kdrv_blkctl_t*)(uintptr_t)pa; b->blk_pa = pa; b->blk_pages = pages;
        b->req_head = 0; b->rsp_tail = 0; kdrv_blk_track_init(&b->trk);
        b->blk_flags = r.flags; b->blk_sectors = r.sectors;
        for (unsigned i = 0; i < sizeof b->blk_id; i++) b->blk_id[i] = r.name[i];
        b->blk_alive = 1;
        spin_unlock_irqrestore(&b->blk_lk, f);
        b->bd.sectors = r.sectors; b->bd.read = bd_read; b->bd.write = bd_write; b->bd.flush = bd_flush; b->bd.priv = b;
        const char* pre = "Ring 3 drv-"; unsigned k = 0;
        for (; pre[k]; k++) b->bd.name[k] = pre[k];
        for (unsigned i = 0; b->name[i] && k < sizeof b->bd.name - 1; i++) b->bd.name[k++] = b->name[i];
        b->bd.name[k] = 0;
        rc = (int64_t)BLK_VA;
    }
    proc_space_unlock();
    if (rc > 0) __atomic_store_n(&b->blk_reg_pending, 1, __ATOMIC_RELEASE);          /* в blk.c диск заведёт основной цикл: драйвер к тому времени уже ждёт запросов */
    return rc;
}

static void blk_reg_run(void) {
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        bind_t* b = &binds[i];
        if (b->state != B_RUN || !__atomic_load_n(&b->blk_reg_pending, __ATOMIC_ACQUIRE)) continue;
        if (!__atomic_exchange_n(&b->blk_reg_pending, 0, __ATOMIC_ACQ_REL)) continue;
        blk_dev_t* pub = blk_register_r3(&b->bd, b->blk_id, b->bd.name);
        uart_print("[KIRA DRV] "); uart_print(b->name);
        if (!pub) { uart_print(": диск "); uart_print(b->blk_id); uart_print(" не заведён в blk.c (имя занято или список дисков полон)\n"); continue; }
        uart_print(": диск "); uart_print(b->blk_id); uart_print(" заведён в blk.c: "); uart_print_dec(b->blk_sectors); uart_print(" секторов по ");
        uart_print_dec(KDRV_BLK_SECTOR); uart_print(" Б"); uart_print(b->blk_flags & KDRV_BLK_F_RO ? ", только чтение" : ""); uart_print(", кольца ");
        uart_print_dec(b->blk_pages); uart_print(" стр.\n");
    }
}

/* ---- политика записи в конфигурацию ---- */
static int overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }

static int cfg_deny(const pci_dev_t* d, uint32_t off, uint32_t w) {
    uint32_t end = off + w;
    if (overlap(off, end, 0x00, 0x08) || overlap(off, end, 0x08, 0x0C) || overlap(off, end, 0x0E, 0x10) ||
        overlap(off, end, 0x10, 0x3C) || overlap(off, end, 0x3D, 0x40)) return 1;     /* идентификаторы, статус, класс, BAR, ПЗУ, указатель capabilities */
    uint8_t id; uint16_t c = 0;
    for (int n = 0; n < 48 && (c = pci_next_cap(d, c, &id)) != 0; n++) {
        uint32_t len = 0;
        if (id == 0x05) { uint16_t ctl = pci_read16(d, (uint16_t)(c + 2)); len = ((ctl & 0x80) ? 14u : 10u) + ((ctl & 0x100) ? 10u : 0u); }   /* MSI: адрес/данные сообщения */
        else if (id == 0x11) len = 12;                                                  /* MSI-X: управление таблицей */
        else if (id == 0x10) len = 0x3C;                                                /* PCI Express: сброс функции, связь, питание слота */
        if (len && overlap(off, end, c, c + len)) return 1;
    }
    return 0;
}

static void deny_note(bind_t* b, const char* what, uint64_t v) {
    if (b->denied++ == 0) { uart_print("[KIRA DRV] drv-"); uart_print(b->name); uart_print(": "); uart_print(what); uart_print(" "); uart_print_hex(v); uart_print(" отклонено ядром\n"); }
}

/* Страница с началом/концом BAR не должна делить адреса с BAR другого устройства (иначе драйвер увидит чужие регистры). */
static int bar_page_exclusive(const pci_dev_t* me, int bar) {
    uint64_t lo = me->bar[bar].base & ~4095ull, hi = (me->bar[bar].base + me->bar[bar].size + 4095) & ~4095ull;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        for (int k = 0; k < PCI_BARS; k++) {
            if (d == me && k == bar) continue;
            if (d->bar[k].io || !d->bar[k].size) continue;
            if (d->bar[k].base < hi && d->bar[k].base + d->bar[k].size > lo) return 0;
        }
    }
    return 1;
}

static int64_t sys_mmio_map(bind_t* b, uint64_t bar, uint64_t off, uint64_t len) {
    const pci_dev_t* d = b->dev;
    if (bar >= PCI_BARS || (off & 4095) || !len || len > KDRV_MAX_MMIO_BYTES) return KE_ARG;
    if (d->bar[bar].io || !d->bar[bar].size || !d->bar[bar].base) return KE_NOENT;     /* порты ввода-вывода и пустые BAR не отображаются */
    uint64_t bsz = (d->bar[bar].size + 4095) & ~4095ull;
    if (off >= bsz || len > bsz - off) return KE_ARG;                                  /* только внутри BAR своего устройства */
    if ((d->bar[bar].base & 4095) || !bar_page_exclusive(d, (int)bar)) return KE_PERM;
    uint32_t pages = (uint32_t)((len + 4095) / 4096);
    proc_space_lock();
    int64_t rc;
    if (b->nmap >= KDRV_MAX_MMIO) rc = KE_LIMIT;
    else if (proc_map_phys(b->mmio_next, d->bar[bar].base + off, pages, VMM_WRITE | VMM_NOEXEC | VMM_NOCACHE) != 0) rc = KE_NOMEM;
    else {
        b->map[b->nmap].va = b->mmio_next; b->map[b->nmap].pages = pages; b->nmap++;
        rc = (int64_t)b->mmio_next;
        b->mmio_next += ((uint64_t)pages + 1) * 4096;                                  /* промежуток-страница после каждого отображения */
    }
    proc_space_unlock();
    return rc;
}

#if defined(__x86_64__) && !defined(KIRA_HOST_TEST)
static inline uint32_t kio_in(uint16_t p, uint32_t w) { uint32_t v = 0;
    if (w == 1) { uint8_t x; __asm__ volatile ("inb %1, %0" : "=a"(x) : "Nd"(p)); v = x; }
    else if (w == 2) { uint16_t x; __asm__ volatile ("inw %1, %0" : "=a"(x) : "Nd"(p)); v = x; }
    else __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p));
    return v; }
static inline void kio_out(uint16_t p, uint32_t w, uint32_t v) {
    if (w == 1) __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)v), "Nd"(p));
    else if (w == 2) __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)v), "Nd"(p));
    else __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
#define KIO_OK 1
#else
#define KIO_OK 0
static inline uint32_t kio_in(uint16_t p, uint32_t w) { (void)p; (void)w; return 0; }
static inline void kio_out(uint16_t p, uint32_t w, uint32_t v) { (void)p; (void)w; (void)v; }
#endif

/* Порты ввода-вывода I/O BAR своего устройства. Чужие порты, выход за BAR, неверная ширина - отказ; прав кроме CAP_MMIO не нужно, но декодирование
   портов включено только если право выдано (pci_enable при привязке). Прерывания не отключаются: одна операция - одна инструкция in/out. */
static int64_t sys_io(bind_t* b, uint64_t a1, uint64_t off, uint64_t val) {
    const pci_dev_t* d = b->dev;
    if (!KIO_OK) return KE_NOENT;                                                      /* портов нет на этой архитектуре */
    uint32_t bar = (uint32_t)(a1 & 0xFF), port, w, op, n;
    if (bar >= PCI_BARS) return KE_ARG;
    int rc = kdrv_io_check(a1, off, d->bar[bar].io, d->bar[bar].base, d->bar[bar].size, &port, &w, &op, &n);
    if (rc) return rc;
    if (op == KDRV_IO_READ) return (int64_t)kio_in((uint16_t)port, w);
    if (op == KDRV_IO_WRITE) { kio_out((uint16_t)port, w, (uint32_t)val); return 0; }
    uint8_t buf[KDRV_IO_MAX_BYTES];
    uint32_t bytes = n * w;
    if (op == KDRV_IO_READ_MULTI) {
        for (uint32_t i = 0; i < n; i++) { uint32_t v = kio_in((uint16_t)port, w); for (uint32_t k = 0; k < w; k++) buf[i * w + k] = (uint8_t)(v >> (8 * k)); }
        return copy_to_user(val, buf, bytes) ? KE_ARG : 0;
    }
    if (copy_from_user(buf, val, bytes)) return KE_ARG;
    for (uint32_t i = 0; i < n; i++) { uint32_t v = 0; for (uint32_t k = 0; k < w; k++) v |= (uint32_t)buf[i * w + k] << (8 * k); kio_out((uint16_t)port, w, v); }
    return 0;
}

static int64_t sys_dma_alloc(bind_t* b, uint64_t bytes, uint64_t bits, uint64_t out) {
    if (!bytes || bytes > KDRV_MAX_DMA_BYTES || (bits && (bits < 20 || bits > 63))) return KE_ARG;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
    proc_space_lock();
    int64_t rc = 0;
    uint64_t pa = 0;
    if (b->ndma >= KDRV_MAX_DMA || ((uint64_t)b->dma_pages + pages) * 4096 > KDRV_MAX_DMA_TOTAL) rc = KE_LIMIT;
    else if (!(pa = bits ? pmm_alloc_contiguous_below(pages, 1ull << bits) : pmm_alloc_contiguous(pages))) rc = KE_NOMEM;
    if (!rc) {
        uint64_t t;
        if (vmm_translate(pa, &t) != 0 || t != pa || vmm_translate(pa + (uint64_t)(pages - 1) * 4096, &t) != 0 || t != pa + (uint64_t)(pages - 1) * 4096) rc = KE_NOMEM;   /* устройству нужен тот же адрес, что виден процессору */
        else if (bits && pa + (uint64_t)pages * 4096 > (1ull << bits)) rc = KE_NOMEM;     /* устройство не достанет: предел его шины */
        if (rc) { pmm_free_contiguous(pa, pages); pa = 0; }
    }
    if (!rc) {
        uint8_t* z = (uint8_t*)(uintptr_t)pa;
        for (uint64_t i = 0; i < (uint64_t)pages * 4096; i++) z[i] = 0;                 /* чужих данных драйвер не получит */
        uint64_t va = b->dma_next;
        /* Устройство в своём домене VT-d видит только то, что выдано здесь: адрес устройства равен физическому. */
        int iommu = iommu_device_isolated(b->dev);
        if (iommu && iommu_map(b->dev, pa, pa, (uint64_t)pages * 4096, IOMMU_R | IOMMU_W) != 0) { pmm_free_contiguous(pa, pages); rc = KE_NOMEM; }
        else if (proc_map_phys(va, pa, pages, VMM_WRITE | VMM_NOEXEC) != 0) {
            if (iommu) iommu_unmap(b->dev, pa, (uint64_t)pages * 4096);
            pmm_free_contiguous(pa, pages); rc = KE_NOMEM;
        }
        else {
            kdrv_dma_t r = { va, pa, (uint64_t)pages * 4096 };
            if (copy_to_user(out, &r, sizeof r) != 0) {
                proc_unmap_pages(va, pages);
                if (iommu) iommu_unmap(b->dev, pa, (uint64_t)pages * 4096);
                pmm_free_contiguous(pa, pages); rc = KE_ARG;
            }
            else {
                b->dma[b->ndma].pa = pa; b->dma[b->ndma].pages = pages; b->ndma++; b->dma_pages += pages;
                b->dma_next += ((uint64_t)pages + 1) * 4096;
            }
        }
    }
    proc_space_unlock();
    return rc;
}

static int64_t sys_irq_wait(bind_t* b, uint64_t t10) {
    if (__atomic_exchange_n(&b->waiting, 1, __ATOMIC_ACQ_REL)) return KE_LIMIT;       /* ждать может один поток: так устроено ядро (irqwait_t) */
    uint64_t until = t10 == 0xFFFFFFFFull || t10 == ~0ULL ? ~0ULL : timer_get_ticks() + (t10 > 100000 ? 100000 : t10) * TIMER_PER_10MS;
    int64_t rc;
    intx_rearm(b);                                                                    /* драйвер вернулся к ожиданию: причина снята, устройство снова может прерывать */
    for (;;) {
        uint32_t mark = irqwait_mark(&b->iw);
        uint32_t ev = 0;
        if (b->irq_on && __atomic_load_n(&b->irq_count, __ATOMIC_ACQUIRE) != b->irq_seen) { b->irq_seen = __atomic_load_n(&b->irq_count, __ATOMIC_ACQUIRE); ev |= KDRV_EV_IRQ; }
        if (__atomic_exchange_n(&b->tx_pending, 0, __ATOMIC_ACQ_REL)) ev |= KDRV_EV_TX;
        if (__atomic_exchange_n(&b->blk_pending, 0, __ATOMIC_ACQ_REL)) ev |= KDRV_EV_BLK;
        if (ev) { rc = ev; break; }
        if (proc_dying()) { rc = KE_INTR; break; }
        if (!b->irq_on && !b->net_alive && !(b->granted & (CAP_NETDEV | CAP_BLKDEV))) { rc = KE_NOENT; break; }   /* ждать нечего: у устройства нет прерывания */
        uint64_t now = timer_get_ticks();
        if (now >= until) { rc = KE_TIMEOUT; break; }
        uint64_t left = until == ~0ULL ? 25 : until - now;
        irqwait_wait(&b->iw, mark, left > 25 ? 25 : left);                              /* кусками: завершение процесса замечается быстро */
    }
    __atomic_store_n(&b->waiting, 0, __ATOMIC_RELEASE);
    return rc;
}

/* Только чтение: кто занимает каждое устройство PCI. Права проверены в syscall.c (CAP_PCILIST); драйверы Ring 3 этим вызовом не пользуются. */
static int64_t sys_pcilist(uint64_t buf, uint64_t max, uint64_t first) {
    if (max == 0 || max > KDRV_PCILIST_MAX) return KE_ARG;
    kdrv_pcient_t* out = (kdrv_pcient_t*)kmalloc(sizeof *out * (size_t)max);
    if (!out) return KE_ARG;
    int total = pci_count();
    uint64_t n = 0;
    for (uint64_t i = first; i < (uint64_t)total && n < max; i++, n++) {
        const pci_dev_t* d = pci_get((int)i);
        kdrv_pcient_t* e = &out[n];
        uint8_t* z = (uint8_t*)e; for (unsigned k = 0; k < sizeof *e; k++) z[k] = 0;
        e->vendor = d->vendor; e->device = d->device; e->cls = cls_of(d);
        e->bus = d->bus; e->dev = d->dev; e->fn = d->fn;
        e->owner = d->class_code == 0x06 ? KDRV_OWN_BRIDGE : builtin_owned(d) ? KDRV_OWN_KERNEL : KDRV_OWN_NONE;
        if (e->owner == KDRV_OWN_NONE && d->class_code == 0x03 && fb_available()) {      /* экран держится на буфере этой видеокарты (включён загрузчиком) */
            const fb_info_t* fi = fb_get_info();
            for (int k = 0; k < PCI_BARS && fi; k++)
                if (!d->bar[k].io && d->bar[k].size && fi->phys >= d->bar[k].base && fi->phys - d->bar[k].base < d->bar[k].size) { e->owner = KDRV_OWN_FB; break; }
        }
        bind_t* b = e->owner == KDRV_OWN_NONE ? find_slot(d, 0) : 0;
        if (b && b->state == B_RUN) {
            e->owner = KDRV_OWN_RING3;
            for (unsigned k = 0; b->name[k] && k < sizeof e->name - 1; k++) e->name[k] = b->name[k];
        }
    }
    int64_t rc = (int64_t)n;
    if (n && copy_to_user(buf, out, sizeof *out * (size_t)n)) rc = KE_ARG;
    kfree(out);
    return rc;
}

int64_t kdrv_sys(unsigned nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (nr == KDRV_SYS_PCILIST) return sys_pcilist(a1, a2, a3);
    uint32_t slot = proc_current_drv();
    if (!slot || slot > KDRV_SLOTS) return proc_cap_denied(CAP_PCIDEV, nr);          /* не драйвер: даже если бы право было, устройства нет */
    bind_t* b = &binds[slot - 1];
    if (b->state != B_RUN) return KE_INTR;
    switch (nr) {
    case KDRV_SYS_INFO: {
        if (a2 < sizeof(kdrv_info_t)) return KE_ARG;
        kdrv_info_t i;
        uint8_t* z = (uint8_t*)&i; for (unsigned k = 0; k < sizeof i; k++) z[k] = 0;
        i.size = sizeof i; i.vendor = b->dev->vendor; i.device = b->dev->device; i.cls = cls_of(b->dev);
        i.granted = b->granted; i.version = b->version; i.flags = b->irq_on ? (b->intx ? KDRV_INFO_INTX : KDRV_INFO_MSI) : 0;
        for (unsigned k = 0; k < sizeof i.name && b->name[k]; k++) i.name[k] = b->name[k];
        for (int k = 0; k < PCI_BARS; k++) {
            i.bar[k].size = b->dev->bar[k].size;
            i.bar[k].flags = (b->dev->bar[k].io ? KDRV_BAR_IO : 0) | (b->dev->bar[k].mem64 ? KDRV_BAR_MEM64 : 0) | (b->dev->bar[k].prefetch ? KDRV_BAR_PREFETCH : 0);
        }
        return copy_to_user(a1, &i, sizeof i) ? KE_ARG : 0;
    }
    case KDRV_SYS_CFG_READ: {
        if ((a2 != 1 && a2 != 2 && a2 != 4) || a1 >= 256 || (a1 % a2)) return KE_ARG;
        if (overlap((uint32_t)a1, (uint32_t)(a1 + a2), 0x10, 0x28) || overlap((uint32_t)a1, (uint32_t)(a1 + a2), 0x30, 0x34)) return 0;   /* адреса BAR и ПЗУ ядро не показывает */
        return a2 == 1 ? pci_read8(b->dev, (uint16_t)a1) : a2 == 2 ? pci_read16(b->dev, (uint16_t)a1) : (int64_t)pci_read32(b->dev, (uint16_t)a1);
    }
    case KDRV_SYS_CFG_WRITE: {
        if ((a2 != 1 && a2 != 2 && a2 != 4) || a1 >= 256 || (a1 % a2)) return KE_ARG;
        if (a1 == 0x04) {                                                              /* команда: только её разрешённые биты и только словом */
            uint32_t ok = PCI_CMD_INTX_OFF | 0x40 | 0x100;
            if (b->granted & CAP_MMIO) ok |= PCI_CMD_MEM | PCI_CMD_IO;
            if (a2 != 2 || (a3 & ~(uint64_t)ok)) { deny_note(b, "запись в регистр команд PCI значения", a3); return KE_PERM; }
            uint16_t cur = pci_read16(b->dev, 0x04);
            pci_write16(b->dev, 0x04, (uint16_t)((cur & ~ok) | (uint16_t)a3));
            return 0;
        }
        if (cfg_deny(b->dev, (uint32_t)a1, (uint32_t)a2)) { deny_note(b, "запись в защищённую область конфигурации, смещение", a1); return KE_PERM; }
        if (a2 == 1) { uint16_t o = (uint16_t)(a1 & ~3u); uint32_t sh = (uint32_t)(a1 & 3) * 8; pci_write32(b->dev, o, (pci_read32(b->dev, o) & ~(0xFFu << sh)) | ((uint32_t)(a3 & 0xFF) << sh)); }
        else if (a2 == 2) pci_write16(b->dev, (uint16_t)a1, (uint16_t)a3);
        else pci_write32(b->dev, (uint16_t)a1, (uint32_t)a3);
        return 0;
    }
    case KDRV_SYS_MMIO_MAP: return sys_mmio_map(b, a1, a2, a3);
    case KDRV_SYS_DMA_ALLOC: return sys_dma_alloc(b, a1, a2, a3);
    case KDRV_SYS_IO: return sys_io(b, a1, a2, a3);
    case KDRV_SYS_IRQ_WAIT: return sys_irq_wait(b, a1);
    case KDRV_SYS_NETDEV: return sys_netdev(b, a1, a2);
    case KDRV_SYS_BLKDEV: return sys_blkdev(b, a1, a2);
    default: return KE_ARG;
    }
}

/* ---- консоль ---- */
static void show(void) {
    list_t* l = (list_t*)kmalloc(sizeof *l);
    if (!l) return;
    int r = list_pkgs(l);
    if (r) { uart_print("каталога /drv нет (пакеты драйверов: /drv/ИМЯ.kdrv и /drv/ИМЯ.rec)\n"); }
    else {
        uart_print("пакеты в /drv: "); uart_print_dec(l->n); uart_print("\n");
        for (unsigned i = 0; i < l->n; i++) {
            pkg_t p;
            int rc = pkg_load(l->names[i], &p);
            uart_print("  "); uart_print(l->names[i]);
            if (rc != KDRV_OK) { uart_print(": ОТКЛОНЁН ("); uart_print(kdrv_strerror(rc)); uart_print(")\n"); continue; }
            uart_print(": версия "); uart_print_dec(p.h.version); uart_print(", SHA-256 сверен, запрошено:"); print_caps(p.h.caps);
            uart_print(", подтверждено:"); print_caps(p.r.caps_ok); uart_print(", источник: "); uart_print(p.r.source); uart_print("\n");
            kfree(p.buf);
        }
    }
    kfree(l);
    unsigned used = 0;
    for (unsigned i = 0; i < KDRV_SLOTS; i++) {
        bind_t* b = &binds[i];
        if (!b->dev) continue;
        used++;
        uart_print("  устройство "); print_bdf(b->dev); uart_print(" "); print_id(b->dev); uart_print(": ");
        if (b->state == B_FREE) { uart_print(b->name[0] ? "драйвер " : "свободно"); uart_print(b->name); if (b->name[0]) uart_print(" не работает"); }
        else { uart_print(b->state == B_RUN ? "drv-" : "освобождается, было drv-"); uart_print(b->name); uart_print(" pid "); uart_print_dec(b->pid); }
        uart_print(", DMA "); uart_print_dec(b->dma_pages); uart_print(" стр., отображений "); uart_print_dec(b->nmap);
        uart_print(", прерываний "); uart_print_dec(b->irq_count);
        if (b->nd_registered) { uart_print(", карта: передано "); uart_print_dec(b->net_tx); uart_print(", принято "); uart_print_dec(b->net_rx); uart_print(", битых "); uart_print_dec(b->net_bad); }
        if (b->blk_id[0]) { uart_print(", диск "); uart_print(b->blk_id); uart_print(": запросов "); uart_print_dec(b->blk_ops); uart_print(", битых ответов "); uart_print_dec(b->blk_bad); }
        uart_print("\n");
    }
    if (!used) uart_print("  драйверам Ring 3 устройства пока не отданы\n");
}

/* ---- консоль: состояние защиты от отката ---- */
static int parse_u32(const char* t, uint32_t* out) {
    uint64_t v = 0; unsigned n = 0;
    for (; t[n] >= '0' && t[n] <= '9'; n++) { v = v * 10 + (uint64_t)(t[n] - '0'); if (v > 0xFFFFFFFFu) return -1; }
    if (n == 0 || (t[n] && t[n] != ' ')) return -1;
    *out = (uint32_t)v;
    return 0;
}

static void state_show(void) {
    kmutex_lock(&state_mx);
    state_load_locked();
    if (gst.status != KDRV_STATE_OK) {
        uart_print(gst.status == KDRV_STATE_CORRUPT ? "[KIRA DRV] " KDRV_STATE_PATH " ПОВРЕЖДЁН: драйверы отклоняются; после проверки диска - drv state-reset\n"
                                                     : "[KIRA DRV] " KDRV_STATE_PATH " не удалось прочитать: драйверы отклоняются, повторите позже\n");
        kmutex_unlock(&state_mx);
        return;
    }
    uart_print("защита от отката: максимум времени "); uart_print_dec(gst.max_time); uart_print(", полов "); uart_print_dec(gst.nfloor);
    uart_print(", отозванных ключей "); uart_print_dec(gst.nrevoked); uart_print(gst.dirty ? " (есть несохранённое)\n" : "\n");
    for (uint32_t i = 0; i < gst.nfloor; i++) { uart_print("  пол "); uart_print(gst.floor[i].name); uart_print(": "); uart_print_dec(gst.floor[i].floor); uart_print("\n"); }
    for (uint32_t i = 0; i < gst.nrevoked; i++) {
        char id[KDRV_SIG_KEY_ID_SIZE + 1];
        for (unsigned k = 0; k < KDRV_SIG_KEY_ID_SIZE; k++) id[k] = gst.revoked[i][k] >= 0x20 && gst.revoked[i][k] < 0x7F ? (char)gst.revoked[i][k] : '?';
        id[KDRV_SIG_KEY_ID_SIZE] = 0;
        uart_print("  отозван ключ "); uart_print(id); uart_print("\n");
    }
    kmutex_unlock(&state_mx);
}

/* Команды, меняющие состояние: floor ИМЯ N (только вверх), revoke ИДКЛЮЧА(16 знаков), time-reset, state-reset. Возвращают 1, если команда была ими. */
static int state_cmd(const char* verb, const char* arg) {
    if (streq(verb, "state")) { state_show(); return 1; }
    if (!streq(verb, "floor") && !streq(verb, "revoke") && !streq(verb, "time-reset") && !streq(verb, "state-reset")) return 0;
    if (!kfs_mounted()) { uart_print("[KIRA DRV] нет файловой системы\n"); return 1; }
    kmutex_lock(&state_mx);
    state_load_locked();
    if (streq(verb, "state-reset")) {
        kdrv_state_init(&gst); gst_loaded = 1; gst.dirty = 1;
        audit_event(AUD_K_CONFIG, audit_cmd_source(), AUD_R_OK, 0, 0, 0, "drv", "state-reset (rollback floors, time max, revocations)");
        if (state_save_locked() == 0) uart_print("[KIRA DRV] состояние защиты от отката СБРОШЕНО явной командой: полы версий, максимум времени и отзывы из файла очищены (ключи, отозванные в самом ядре, остаются отозванными)\n");
        kmutex_unlock(&state_mx);
        return 1;
    }
    if (gst.status != KDRV_STATE_OK) { uart_print("[KIRA DRV] состояние недоступно или повреждено - сначала drv state-reset\n"); kmutex_unlock(&state_mx); return 1; }
    if (streq(verb, "time-reset")) {
        rtc_time_t t;
        if (rtc_now(0, &t) != 0) uart_print("[KIRA DRV] часов нет: максимум времени не меняю\n");
        else {
            kdrv_state_reset_time(&gst, (uint64_t)rtc_to_epoch(&t));
            audit_event(AUD_K_CONFIG, audit_cmd_source(), AUD_R_OK, 0, 0, 0, "drv", "time-reset");
            if (state_save_locked() == 0) { uart_print("[KIRA DRV] максимум времени := текущие часы (явная команда); убедитесь, что часы верны\n"); }
        }
    } else if (streq(verb, "revoke")) {
        unsigned n = 0; while (arg[n] && arg[n] != ' ') n++;
        if (n != KDRV_SIG_KEY_ID_SIZE) uart_print("использование: drv revoke ИДКЛЮЧА (ровно 16 знаков, как в kdrv_trust.h)\n");
        else {
            int r = kdrv_state_revoke_key(&gst, (const uint8_t*)arg);
            { char d[40] = "revoke key "; for (unsigned q = 0; q < KDRV_SIG_KEY_ID_SIZE && q < 16; q++) d[11 + q] = arg[q]; d[27] = 0; audit_event(AUD_K_DRV_REMOVE, audit_cmd_source(), r < 0 ? AUD_R_FAIL : AUD_R_OK, 0, 0, 0, "drv", d); }
            if (r < 0) uart_print("[KIRA DRV] список отзывов полон\n");
            else { uart_print(r ? "[KIRA DRV] ключ отозван; пакеты с его подписью больше не принимаются\n" : "[KIRA DRV] ключ уже был отозван\n"); (void)state_save_locked(); }
        }
    } else {
        char name[16]; unsigned n = 0;
        while (arg[n] && arg[n] != ' ' && n < sizeof name - 1) { name[n] = arg[n]; n++; }
        name[n] = 0;
        const char* num = arg + n; while (*num == ' ') num++;
        uint32_t v;
        if (!name_valid(name) || parse_u32(num, &v) != 0) uart_print("использование: drv floor ИМЯ ВЕРСИЯ (пол можно только поднять)\n");
        else {
            int r = kdrv_state_raise_floor(&gst, name, v);
            if (r < 0) uart_print("[KIRA DRV] таблица полов полна\n");
            else if (r == 0) { uart_print("[KIRA DRV] пол не изменён: он уже не ниже "); uart_print_dec(v); uart_print(" (понизить можно только drv state-reset)\n"); }
            else { { char d[40] = "floor "; unsigned q = 6; for (unsigned z = 0; name[z] && q < 30; z++) d[q++] = name[z]; d[q++] = ' '; for (unsigned z = 0; num[z] && q < 39; z++) d[q++] = num[z]; d[q] = 0; audit_event(AUD_K_CONFIG, audit_cmd_source(), AUD_R_OK, v, 0, 0, "drv", d); }
                   uart_print("[KIRA DRV] пол версий "); uart_print(name); uart_print(" := "); uart_print_dec(v); uart_print("\n"); (void)state_save_locked(); }
        }
    }
    kmutex_unlock(&state_mx);
    return 1;
}

void kdrv_cmd(const char* arg) {
    while (*arg == ' ') arg++;
    char verb[12]; unsigned n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof verb - 1) { verb[n] = arg[n]; n++; }
    verb[n] = 0;
    arg += n; while (*arg == ' ') arg++;
    if (!verb[0] || streq(verb, "list")) { show(); return; }
    if (streq(verb, "rescan")) { scan(); return; }
    if (state_cmd(verb, arg)) return;
    char name[16]; n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof name - 1) { name[n] = arg[n]; n++; }
    name[n] = 0;
    const char* rest = arg + n; while (*rest == ' ') rest++;
    if (!name_valid(name)) { uart_print("использование: drv | drv start ИМЯ [аргументы] | drv stop ИМЯ | drv rescan | drv state | drv floor ИМЯ N | drv revoke ИДКЛЮЧА | drv time-reset | drv state-reset\n"); return; }
    if (streq(verb, "stop")) {
        for (unsigned i = 0; i < KDRV_SLOTS; i++) {
            bind_t* b = &binds[i];
            if (b->state == B_RUN && streq(b->name, name)) { uart_print("[KIRA DRV] останавливаю drv-"); uart_print(name); uart_print("\n"); proc_kill(b->pid); return; }
        }
        uart_print("драйвер "); uart_print(name); uart_print(" не работает\n");
        return;
    }
    if (!streq(verb, "start")) { uart_print("использование: drv | drv start ИМЯ [аргументы] | drv stop ИМЯ | drv rescan | drv state | drv floor ИМЯ N | drv revoke ИДКЛЮЧА | drv time-reset | drv state-reset\n"); return; }
    if (!kfs_mounted()) { uart_print("[KIRA DRV] нет файловой системы\n"); return; }
    for (int tries = 0; tries < 20; tries++) {                                         /* только что остановленный драйвер: ждём возврата памяти (до ~0.4 с) */
        int busy = 0;
        for (int i = 0; i < pci_count(); i++) {
            const pci_dev_t* d = pci_get(i);
            bind_t* b = find_slot(d, 0);
            if (b && b->state == B_DEAD && streq(b->name, name)) busy = 1;
        }
        if (!busy) break;
        kdrv_poll();
        sched_sleep_ticks(TIMER_HZ / 50);
    }
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        if (builtin_owned(d) || !pkg_wants(name, d)) continue;
        bind_t* b = find_slot(d, 0);
        if (b && b->state != B_FREE) continue;
        (void)start(name, d, rest);
        return;
    }
    uart_print("[KIRA DRV] "); uart_print(name); uart_print(": нет пакета /drv/"); uart_print(name); uart_print(".kdrv или нет свободного подходящего устройства\n");
}
