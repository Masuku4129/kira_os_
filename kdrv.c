/* Менеджер драйверов Ring 3 (kdrv.h, kdrv_format.h, docs/DRIVERS.md). Одно устройство PCI - одна привязка (слот) на всё время работы
   ядра: в слоте живут прерывание устройства (вектор MSI выдаётся один раз и не отдаётся обратно) и карта драйвера для сетевого стека
   (стек не умеет забывать карты). Все обращения драйвера к устройству идут через вызовы KDRV_SYS_* ниже, и каждый из них смотрит
   на привязку ТЕКУЩЕГО процесса: другого устройства драйвер не видит, потому что вызовы его не называют. */
#include "kdrv.h"
#include "kdrv_format.h"
#include "pci.h"
#include "msi.h"
#include "proc.h"
#include "kobj.h"
#include "kfs.h"
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
    uint64_t dead_at;
} bind_t;

static bind_t binds[KDRV_SLOTS];
static spinlock_t klock = SPINLOCK_INIT;
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
typedef struct { uint8_t* buf; uint32_t len; kdrv_header_t h; kdrv_rec_t r; } pkg_t;

static void path_of(char* out, const char* name, const char* ext) {
    unsigned n = 0;
    for (const char* p = "/drv/"; *p; p++) out[n++] = *p;
    for (const char* p = name; *p; p++) out[n++] = *p;
    for (const char* p = ext; *p; p++) out[n++] = *p;
    out[n] = 0;
}

static int name_valid(const char* n) {
    return kdrv_name_check(n, KDRV_NAME_MAX + 1);
}

/* Читает пакет и ВСЕГДА сверяет хеши: образа ELF и всего файла .kdrv - с /drv/ИМЯ.rec. Ошибка - код KDRV_E_* или KFS-код (< -100 не бывает). */
static int pkg_load(const char* name, pkg_t* p) {
    p->buf = 0; p->len = 0;
    char path[48], rpath[48];
    path_of(path, name, ".kdrv"); path_of(rpath, name, ".rec");
    kfs_stat_t st, rs;
    if (kfs_stat(path, &st) != 0 || st.type != KFS_FILE) return KDRV_E_SHORT;
    if (st.size < sizeof(kdrv_header_t) || st.size > sizeof(kdrv_header_t) + KDRV_ELF_MAX) return KDRV_E_SIZE;
    if (kfs_stat(rpath, &rs) != 0 || rs.type != KFS_FILE || rs.size != sizeof(kdrv_rec_t)) return KDRV_E_UNCONFIRMED;   /* нет подтверждения установки */
    uint8_t* b = (uint8_t*)kmalloc((size_t)st.size);
    if (!b) return KDRV_E_SIZE;
    kdrv_rec_t rec;
    if (kfs_read(path, 0, b, (size_t)st.size) != (int64_t)st.size || kfs_read(rpath, 0, &rec, sizeof rec) != (int64_t)sizeof rec) { kfree(b); return KDRV_E_SHORT; }
    int rc = kdrv_header_check(b, (uint32_t)st.size, &p->h);
    if (rc == KDRV_OK && !streq(p->h.name, name)) rc = KDRV_E_NAME;            /* имя в заголовке = имя файла: переименованный пакет не годится */
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
    if (rc != KDRV_OK) { kfree(b); return rc; }
    p->buf = b; p->len = (uint32_t)st.size;
    return KDRV_OK;
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

static void irq_enable(bind_t* b) {
    const pci_dev_t* d = b->dev;
    if (!find_cap(d, 0x05) && !find_cap(d, 0x11)) return;                          /* ни MSI, ни MSI-X: прерывания драйверу не будет */
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
    pci_write16(d, 0x04, (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER)));
    return pci_read16(d, 0x04);
}

/* ---- запуск ---- */
static void bind_reset(bind_t* b) {
    b->granted = b->version = b->pid = 0;
    b->irq_count = 0; b->irq_seen = 0; b->irq_on = 0; b->waiting = 0; b->tx_pending = 0; b->denied = 0;
    b->nmap = 0; b->mmio_next = MMIO_VA; b->ndma = 0; b->dma_pages = 0; b->dma_next = DMA_VA;
    b->net_alive = 0; b->ctl = 0; b->net_pa = 0; b->net_pages = 0; b->tx_head = b->rx_tail = 0;
    b->net_tx = b->net_rx = b->net_bad = 0;
}

static int start(const char* name, const pci_dev_t* d, const char* args) {
    pkg_t p;
    int rc = pkg_load(name, &p);
    if (rc != KDRV_OK) {
        uart_print("[KIRA DRV] "); uart_print(name);
        if (rc == KDRV_E_HASH) { uart_print(": SHA-256 не совпал с записью /drv/"); uart_print(name); uart_print(".rec"); }
        else { uart_print(": пакет отклонён ("); uart_print(kdrv_strerror(rc)); uart_print(")"); }
        uart_print(" - драйвер НЕ запущен\n");
        life_note("drv-denied", name, rc);
        return rc;
    }
    if (!kdrv_match_dev(&p.h, d->vendor, d->device, cls_of(d))) { kfree(p.buf); uart_print("[KIRA DRV] "); uart_print(name); uart_print(": не подходит этому устройству\n"); return -1; }
    uint32_t granted = p.h.caps & p.r.caps_ok;                                      /* запрошено в пакете И подтверждено пользователем */
    /* Fail closed: сейчас ядро не создаёт VT-d/AMD-Vi/SMMU domain для этого BDF.
       DMA и MSI требуют bus mastering и могут записывать в произвольную физическую память.
       Не запускать такой Ring 3 драйвер, пока iommu_device_isolated() не будет реализован
       и подтверждён тестом для конкретного устройства. */
    if (!kdrv_dma_policy_allow(granted, 0)) {
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
    /* Firmware or a previous owner may have left Bus Master enabled. Clear it
       before exposing any BAR or starting this unisolated driver, then verify the
       config-space write actually took effect. */
    uint16_t pci_command = pci_read16(d, 0x04);
    pci_write16(d, 0x04, kdrv_pci_command_clear_master(pci_command));
    uint16_t pci_after = pci_read16(d, 0x04);
    if (!kdrv_pci_busmaster_handoff_safe(pci_command, pci_after)) {
        (void)quiesce(d);
        b->state = B_FREE;
        kfree(p.buf);
        uart_print("[KIRA DRV] "); uart_print(name);
        uart_print(": inherited or uncleared bus mastering; device is not safe to hand off, driver NOT started\n");
        life_note("drv-denied-busmaster", name, KDRV_E_DMA_UNSAFE);
        return KDRV_E_DMA_UNSAFE;
    }
    if (granted & CAP_MMIO) pci_enable(d, PCI_CMD_MEM);
    /* Эти действия достижимы только после успешной проверки IOMMU policy выше. */
    if (granted & (CAP_DMA | CAP_IRQ)) pci_enable(d, PCI_CMD_MASTER);
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
    uint16_t cmd = quiesce(b->dev);
    b->dead_at = timer_get_ticks();
    f = spin_lock_irqsave(&klock);
    b->state = B_DEAD;
    spin_unlock_irqrestore(&klock, f);
    __atomic_add_fetch(&ndead, 1, __ATOMIC_ACQ_REL);
    uart_print("[KIRA DRV] drv-"); uart_print(b->name); uart_print(" (pid "); uart_print_dec(pid); uart_print(") завершён: устройство ");
    print_bdf(b->dev); uart_print(" остановлено (команда PCI "); uart_print_hex(cmd); uart_print(", MSI выключено), DMA ");
    uart_print_dec(b->dma_pages); uart_print(" стр. в "); uart_print_dec(b->ndma); uart_print(" блок(ах) и кольца ");
    uart_print_dec(b->net_pages); uart_print(" стр. вернутся через 100 мс\n");
}

static void release(bind_t* b) {
    uint32_t got = 0, want = b->dma_pages + b->net_pages;
    for (uint32_t i = 0; i < b->ndma; i++) if (pmm_free_contiguous(b->dma[i].pa, b->dma[i].pages) == 0) got += b->dma[i].pages;
    if (b->net_pages && pmm_free_contiguous(b->net_pa, b->net_pages) == 0) got += b->net_pages;
    uart_print("[KIRA DRV] "); uart_print(b->name); uart_print(": возвращено памяти DMA и колец "); uart_print_dec(got); uart_print(" из ");
    uart_print_dec(want); uart_print(" стр., отображений устройства и DMA снято "); uart_print_dec(b->nmap + b->ndma);
    uart_print(got == want ? ", утечек нет\n" : ", УТЕЧКА\n");
    b->ndma = 0; b->dma_pages = 0; b->net_pages = 0; b->nmap = 0; b->ctl = 0;
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

void kdrv_poll(void) {
    static int scanned;
    if (!scanned) { scanned = 1; scan(); }
    else if (__atomic_exchange_n(&rescan_req, 0, __ATOMIC_ACQ_REL)) scan();               /* drvget установил пакет */
    ctl_run();                                                                             /* окно «Драйверы»: запуск и остановка по просьбе */
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
    uint64_t lo, hi;
    if (!me || bar < 0 || bar >= PCI_BARS ||
        kdrv_mmio_page_bounds(me->bar[bar].base, me->bar[bar].size, &lo, &hi) != 0) return 0;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        if (!d) return 0;
        for (int k = 0; k < PCI_BARS; k++) {
            if (d == me && k == bar) continue;
            if (d->bar[k].io || !d->bar[k].size || !d->bar[k].base) continue;
            uint64_t other_lo, other_hi;
            if (kdrv_mmio_page_bounds(d->bar[k].base, d->bar[k].size, &other_lo, &other_hi) != 0)
                return 0;
            if (other_lo < hi && lo < other_hi) return 0;
        }
    }
    return 1;
}

static int64_t sys_mmio_map(bind_t* b, uint64_t bar, uint64_t off, uint64_t len) {
    const pci_dev_t* d = b->dev;
    if (bar >= PCI_BARS || (off & 4095) || !len || len > KDRV_MAX_MMIO_BYTES) return KE_ARG;
    if (d->bar[bar].io || !d->bar[bar].size || !d->bar[bar].base) return KE_NOENT;     /* порты ввода-вывода и пустые BAR не отображаются */
    uint64_t physical = 0;
    uint32_t pages = 0;
    if (kdrv_mmio_map_bounds(d->bar[bar].base, d->bar[bar].size, off, len,
                             &physical, &pages) != 0) return KE_ARG;
    if (!bar_page_exclusive(d, (int)bar)) return KE_PERM;
    proc_space_lock();
    int64_t rc;
    if (b->nmap >= KDRV_MAX_MMIO) rc = KE_LIMIT;
    else if (proc_map_phys(b->mmio_next, physical, pages, VMM_WRITE | VMM_NOEXEC | VMM_NOCACHE) != 0) rc = KE_NOMEM;
    else {
        b->map[b->nmap].va = b->mmio_next; b->map[b->nmap].pages = pages; b->nmap++;
        rc = (int64_t)b->mmio_next;
        b->mmio_next += ((uint64_t)pages + 1) * 4096;                                  /* промежуток-страница после каждого отображения */
    }
    proc_space_unlock();
    return rc;
}

static int64_t sys_dma_alloc(bind_t* b, uint64_t bytes, uint64_t bits, uint64_t out) {
    if (!bytes || bytes > KDRV_MAX_DMA_BYTES || (bits && (bits < 20 || bits > 63))) return KE_ARG;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
    proc_space_lock();
    int64_t rc = 0;
    uint64_t pa = 0;
    if (b->ndma >= KDRV_MAX_DMA || ((uint64_t)b->dma_pages + pages) * 4096 > KDRV_MAX_DMA_TOTAL) rc = KE_LIMIT;
    else if (!(pa = pmm_alloc_contiguous(pages))) rc = KE_NOMEM;
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
        if (proc_map_phys(va, pa, pages, VMM_WRITE | VMM_NOEXEC) != 0) { pmm_free_contiguous(pa, pages); rc = KE_NOMEM; }
        else {
            kdrv_dma_t r = { va, pa, (uint64_t)pages * 4096 };
            if (copy_to_user(out, &r, sizeof r) != 0) { proc_unmap_pages(va, pages); pmm_free_contiguous(pa, pages); rc = KE_ARG; }
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
    for (;;) {
        uint32_t mark = irqwait_mark(&b->iw);
        uint32_t ev = 0;
        if (b->irq_on && __atomic_load_n(&b->irq_count, __ATOMIC_ACQUIRE) != b->irq_seen) { b->irq_seen = __atomic_load_n(&b->irq_count, __ATOMIC_ACQUIRE); ev |= KDRV_EV_IRQ; }
        if (__atomic_exchange_n(&b->tx_pending, 0, __ATOMIC_ACQ_REL)) ev |= KDRV_EV_TX;
        if (ev) { rc = ev; break; }
        if (proc_dying()) { rc = KE_INTR; break; }
        if (!b->irq_on && !b->net_alive && !(b->granted & CAP_NETDEV)) { rc = KE_NOENT; break; }   /* ждать нечего: у устройства нет прерывания */
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
        i.granted = b->granted; i.version = b->version; i.flags = b->irq_on ? KDRV_INFO_MSI : 0;
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
            if (b->granted & CAP_MMIO) ok |= PCI_CMD_MEM;
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
    case KDRV_SYS_IRQ_WAIT: return sys_irq_wait(b, a1);
    case KDRV_SYS_NETDEV: return sys_netdev(b, a1, a2);
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
        uart_print("\n");
    }
    if (!used) uart_print("  драйверам Ring 3 устройства пока не отданы\n");
}

void kdrv_cmd(const char* arg) {
    while (*arg == ' ') arg++;
    char verb[12]; unsigned n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof verb - 1) { verb[n] = arg[n]; n++; }
    verb[n] = 0;
    arg += n; while (*arg == ' ') arg++;
    if (!verb[0] || streq(verb, "list")) { show(); return; }
    if (streq(verb, "rescan")) { scan(); return; }
    char name[16]; n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof name - 1) { name[n] = arg[n]; n++; }
    name[n] = 0;
    const char* rest = arg + n; while (*rest == ' ') rest++;
    if (!name_valid(name)) { uart_print("использование: drv | drv start ИМЯ [аргументы] | drv stop ИМЯ | drv rescan\n"); return; }
    if (streq(verb, "stop")) {
        for (unsigned i = 0; i < KDRV_SLOTS; i++) {
            bind_t* b = &binds[i];
            if (b->state == B_RUN && streq(b->name, name)) { uart_print("[KIRA DRV] останавливаю drv-"); uart_print(name); uart_print("\n"); proc_kill(b->pid); return; }
        }
        uart_print("драйвер "); uart_print(name); uart_print(" не работает\n");
        return;
    }
    if (!streq(verb, "start")) { uart_print("использование: drv | drv start ИМЯ [аргументы] | drv stop ИМЯ | drv rescan\n"); return; }
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
