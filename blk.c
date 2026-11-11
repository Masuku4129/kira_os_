#include "blk.h"
#include "pci.h"
#include "uart.h"
#include "kmutex.h"
#include "cpu.h"
#include "kfs.h"
#include "store.h"
#include "devhealth.h"
#include "clock.h"
#include "sched.h"
#include "msi.h"
#include "iommu.h"

/* Диски системы. Порядок опроса: virtio-blk (виртуальные машины), NVMe, AHCI (SATA), и только если на шине есть
   IDE-контроллер (или PCI нет вообще) - ATA PIO по старым портам: в режиме AHCI те же диски через порты не видны,
   а в «совмещённом» режиме один диск иначе появился бы дважды. */

blk_dev_t* virtio_blk_probe(void);
blk_dev_t* ata_probe(void);
void ahci_probe(void);
void nvme_probe(void);

typedef struct {
    blk_dev_t pub;               /* вход для всех: операции по одной под мьютексом */
    blk_dev_t* raw;              /* драйвер */
    kmutex_t mx;
    blk_info_t info;
} slot_t;
static slot_t slots[BLK_MAX];
static int nslots;
static blk_dev_t* primary;
static volatile uint64_t req_count, req_tsc, req_bytes;      /* для Разума: число операций и суммарное время (такты TSC) */

void blk_stats(uint64_t* requests, uint64_t* tsc_total) { *requests = req_count; *tsc_total = req_tsc; }
uint64_t blk_bytes(void) { return req_bytes; }

static volatile uint64_t fake_us;
void blk_udelay(uint32_t us) { fake_us += us; while (us--) cpu_io_delay(); }

static uint64_t now_us(int real) { return real ? clock_ns() / 1000 : fake_us; }
void blk_wait_begin(blk_wait_t* w, uint32_t ms) {
    w->real = clock_ready() && clock_tsc_khz() != 0;
    w->start = now_us(w->real);
    w->end = w->start + (uint64_t)ms * 1000;
    w->iw = 0; w->mark = 0;
}
void blk_wait_irq(blk_wait_t* w, struct irqwait* iw) { w->iw = iw; w->mark = irqwait_mark(iw); }
int blk_wait_step(blk_wait_t* w) {
    uint64_t t = now_us(w->real);
    if (t >= w->end) return 0;
    if (!w->real) blk_udelay(t - w->start < 200 ? 2 : 20);
    else if (w->iw && t - w->start >= 20 && sched_can_sleep()) {          /* прерывание есть: спать до него */
        irqwait_wait(w->iw, w->mark, 1);
        w->mark = irqwait_mark(w->iw);
    }
    else if (t - w->start < 200) { for (int i = 0; i < 64; i++) cpu_relax(); }
    else if (sched_can_sleep()) sched_yield();                         /* долгая команда: процессор - другим */
    else for (int i = 0; i < 256; i++) cpu_relax();
    return 1;
}

static int g_read(blk_dev_t* d, uint64_t lba, uint32_t n, void* buf) {
    slot_t* s = (slot_t*)d->priv;
    if (lba + n < lba || lba + n > s->raw->sectors) return -1;
    kmutex_lock(&s->mx);
    uint64_t t0 = rdtsc();
    int r = s->raw->read(s->raw, lba, n, buf);
    __atomic_add_fetch(&req_tsc, rdtsc() - t0, __ATOMIC_RELAXED); __atomic_add_fetch(&req_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&req_bytes, (uint64_t)n * BLK_SECTOR, __ATOMIC_RELAXED);
    kmutex_unlock(&s->mx);
    if (r) devhealth_error(s->info.id);
    return r;
}
static int g_write(blk_dev_t* d, uint64_t lba, uint32_t n, const void* buf) {
    slot_t* s = (slot_t*)d->priv;
    if (lba + n < lba || lba + n > s->raw->sectors) return -1;
    kmutex_lock(&s->mx);
    uint64_t t0 = rdtsc();
    int r = s->raw->write(s->raw, lba, n, buf);
    __atomic_add_fetch(&req_tsc, rdtsc() - t0, __ATOMIC_RELAXED); __atomic_add_fetch(&req_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&req_bytes, (uint64_t)n * BLK_SECTOR, __ATOMIC_RELAXED);
    kmutex_unlock(&s->mx);
    if (r) devhealth_error(s->info.id);
    return r;
}
static int g_flush(blk_dev_t* d) {
    slot_t* s = (slot_t*)d->priv;
    kmutex_lock(&s->mx);
    int r = s->raw->flush ? s->raw->flush(s->raw) : 0;
    kmutex_unlock(&s->mx);
    if (r) devhealth_error(s->info.id);
    return r;
}

static void scopy(char* d, const char* s, unsigned cap) { unsigned i = 0; while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

/* Что лежит на диске - только чтением. */
static void classify(slot_t* s) {
    static uint8_t sec[2][BLK_SECTOR];
    blk_dev_t* d = &s->pub;
    s->info.kira = s->info.foreign = 0;
    if (d->sectors < 64) return;
    if (store_detect(d) || kfs_detect(d, KFS_BASE_LBA) == 0) { s->info.kira = 1; return; }
    if (d->read(d, 0, 2, sec) != 0) { s->info.foreign = 1; return; }       /* не читается - тем более не трогаем */
    if (sec[0][510] == 0x55 && sec[0][511] == 0xAA) s->info.foreign = 1;  /* MBR, загрузочный сектор FAT/NTFS/exFAT */
    const char* efi = "EFI PART";
    int gpt = 1; for (int i = 0; i < 8; i++) if (sec[1][i] != (uint8_t)efi[i]) gpt = 0;
    if (gpt) s->info.foreign = 1;
    for (int k = 0; k < 2 && !s->info.foreign; k++)                       /* любые данные в начале - чужие */
        for (int i = 0; i < BLK_SECTOR; i++) if (sec[k][i]) { s->info.foreign = 1; break; }
}

void blk_register(blk_dev_t* raw, const char* id, const char* model) {
    if (nslots >= BLK_MAX || !raw || raw->sectors == 0) return;
    slot_t* s = &slots[nslots++];
    s->raw = raw;
    s->pub = *raw;
    s->pub.read = g_read; s->pub.write = g_write; s->pub.flush = g_flush; s->pub.priv = s;
    s->mx = (kmutex_t)KMUTEX_INIT;
    s->info.dev = &s->pub;
    scopy(s->info.id, id, sizeof s->info.id);
    scopy(s->info.model, model, sizeof s->info.model);
    classify(s);
}

#if defined(__x86_64__)
static int has_ide(void) {
    if (pci_count() == 0) return 1;
    for (int i = 0; i < pci_count(); i++) { const pci_dev_t* p = pci_get(i); if (p->class_code == 0x01 && p->subclass == 0x01) return 1; }
    return 0;
}
#endif

void blk_probe(void) {
    nslots = 0; primary = 0;
    uart_print("[KIRA DISK] PCI enumeration starting.\\n");
    int n = pci_count();
    if (n == 0) n = pci_init();
    uart_print("[KIRA DISK] PCI enumeration complete: "); uart_print_dec((uint64_t)n); uart_print(" function(s).\\n");
    uart_print("[KIRA DISK] IOMMU policy initialization starting.\\n");
    (void)iommu_init();      /* idempotent; early kernel init also covers the virtio display path */
    uart_print("[KIRA DISK] IOMMU policy initialization complete.\\n");
    uart_print("[KIRA DISK] Probing virtio-blk.\\n");
    blk_dev_t* v = virtio_blk_probe();
    if (v) blk_register(v, "vda", "virtio-blk");
    uart_print("[KIRA DISK] Probing NVMe.\\n");
    nvme_probe();
    uart_print("[KIRA DISK] Probing AHCI.\\n");
    ahci_probe();
#if defined(__x86_64__)
    uart_print("[KIRA DISK] Probing legacy IDE.\\n");
    if (has_ide()) { blk_dev_t* a = ata_probe(); if (a) blk_register(a, "ata0", a->name); }   /* порты IDE - только у ПК */
#endif
    for (int i = 0; i < nslots; i++) if (slots[i].info.kira) { primary = &slots[i].pub; break; }
    if (nslots && !primary) {
        int blank = -1;
        for (int i = 0; i < nslots; i++) if (!slots[i].info.foreign) { blank = i; break; }
        uart_print("[KIRA DISK] No disk with Kira data: nothing is written to disks. ");
        if (blank >= 0) { uart_print("Empty disk "); uart_print(slots[blank].info.id); uart_print(" can be prepared: format "); uart_print(slots[blank].info.id); uart_print(" yes\n"); }
        else uart_print("All disks hold other systems' data (see 'disk').\n");
    }
    for (int i = 0; i < nslots; i++) {
        slot_t* s = &slots[i];
        uart_print("[KIRA DISK] "); uart_print(s->info.id); uart_print(": "); uart_print(s->info.model); uart_print(", ");
        uart_print_dec(s->pub.sectors / 2048); uart_print(" MB");
        uart_print(&s->pub == primary ? " - system disk\n" : s->info.kira ? " - Kira data (spare)\n" : s->info.foreign ? " - other system's data, read-only for Kira\n" : " - empty\n");
    }
}

int blk_count(void) { return nslots; }
const blk_info_t* blk_info(int i) { return i >= 0 && i < nslots ? &slots[i].info : 0; }
blk_dev_t* blk_find(const char* id) {
    for (int i = 0; i < nslots; i++) {
        const char* a = slots[i].info.id; int k = 0;
        while (a[k] && a[k] == id[k]) k++;
        if (!a[k] && !id[k]) return &slots[i].pub;
    }
    return 0;
}
blk_dev_t* blk_primary(void) { return primary; }
int blk_set_primary(blk_dev_t* d) {
    for (int i = 0; i < nslots; i++) if (&slots[i].pub == d) { primary = d; slots[i].info.kira = 1; slots[i].info.foreign = 0; return 0; }
    return -1;
}

int blk_flush_all(void) {
    int bad = 0;
    for (int i = 0; i < nslots; i++) if (slots[i].raw && slots[i].raw->flush && slots[i].raw->flush(slots[i].raw) != 0) bad++;
    return bad;
}

void blk_print(void) {
    if (!nslots) { uart_print("дисков нет\n"); return; }
    for (int i = 0; i < nslots; i++) {
        slot_t* s = &slots[i];
        uart_print("  "); uart_print(s->info.id); uart_print("  ");
        uint64_t mb = s->pub.sectors / 2048;
        if (mb >= 10240) { uart_print_dec(mb / 1024); uart_print(" ГБ  "); } else { uart_print_dec(mb); uart_print(" МБ  "); }
        uart_print(s->info.model);
        uart_print(&s->pub == primary ? "  [системный]" : s->info.kira ? "  [данные Киры]" : s->info.foreign ? "  [чужие данные: только чтение]" : "  [пустой]");
        uart_print("\n");
    }
    uint64_t fired, woken;
    irqwait_stats(&fired, &woken);
    uart_print("прерываний устройств (MSI): "); uart_print_dec(fired); uart_print(", из них разбудили ждущий поток: ");
    uart_print_dec(woken); uart_print("\n");
}
