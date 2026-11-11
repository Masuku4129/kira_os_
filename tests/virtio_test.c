// PCI (перечисление, мосты, BAR, capabilities) + virtio-blk драйвер против программной модели устройства.
// Модель проверяет порядок инициализации по спецификации virtio 1.0 и обрабатывает заявки по кольцам в памяти.
#include "virtio.h"
#include "pci.h"
#include "blk.h"
#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

/* ---------- модель шины PCI ---------- */
typedef struct { int present, multi; uint8_t cfg[256]; uint64_t bar_size[6]; int bar_io[6], bar_64[6]; } fdev_t;
static fdev_t bus_dev[4][32][8];
static int viol;                           /* нарушения спецификации драйвером */
static void put32(fdev_t* d, int off, uint32_t v) { memcpy(d->cfg + off, &v, 4); }
static void put16(fdev_t* d, int off, uint16_t v) { memcpy(d->cfg + off, &v, 2); }
static uint32_t get32(fdev_t* d, int off) { uint32_t v; memcpy(&v, d->cfg + off, 4); return v; }
static fdev_t* F(int b, int d, int f) { return &bus_dev[b][d][f]; }

static uint32_t cfg_read(uint8_t b, uint8_t d, uint8_t f, uint16_t off) {
    if (b > 3 || d > 31 || f > 7 || !bus_dev[b][d][f].present) return 0xFFFFFFFFu;
    return get32(&bus_dev[b][d][f], off & 0xFC);
}
static void cfg_write(uint8_t b, uint8_t d, uint8_t f, uint16_t off, uint32_t v) {
    if (b > 3 || d > 31 || f > 7 || !bus_dev[b][d][f].present) return;
    fdev_t* x = &bus_dev[b][d][f];
    off &= 0xFC;
    if (off == 4) { put16(x, 4, (uint16_t)v); return; }                      /* команда; старшая часть (статус) - только чтение в модели */
    if (off >= 0x10 && off <= 0x24) {
        int i = (off - 0x10) / 4;
        if (x->bar_size[i] == 0) {
            /* вторая половина 64-битного BAR */
            if (i > 0 && x->bar_64[i - 1]) { uint64_t m = ~(x->bar_size[i - 1] - 1); put32(x, off, v & (uint32_t)(m >> 32)); }
            return;
        }
        uint64_t m = ~(x->bar_size[i] - 1);
        if (x->bar_io[i]) put32(x, off, (v & (uint32_t)m & ~3u) | 1);
        else put32(x, off, (v & (uint32_t)m & ~0xFu) | (x->bar_64[i] ? 0xC : 0));
        return;
    }
    if (off == 0x18 && (x->cfg[0x0E] & 0x7F) == 1) { put32(x, off, v); return; }    /* шины моста */
    put32(x, off, v);
}

/* ---------- модель virtio-blk ---------- */
#define DISK_SECT 4096
static uint8_t vdisk[DISK_SECT * 512];
static uint8_t mmio[0x4000];
typedef struct { int no_v1, reject_features, ro, size_max, drop, no_flush, legacy_only, small_disk; } vopt_t;
static vopt_t opt;
static struct {
    uint32_t dfsel, gfsel; uint32_t gf[2]; uint8_t status; uint16_t qsel, qsize, qen; uint64_t desc, avail, used;
    uint16_t last_avail; int kick_wait, nreq, nflush, nread, nwrite, max_len; int features_written, queue_programmed;
} dv;
#define DEVF(opt) ((opt.no_v1 ? 0 : (1ULL << 32)) | (opt.no_flush ? 0 : (1ULL << 9)) | (1ULL << 2) | (opt.size_max ? (1ULL << 1) : 0) | (opt.ro ? (1ULL << 5) : 0))

volatile void* host_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len) {
    if (d->bar[bar].io || !d->bar[bar].base || bar != 4 || off + len > sizeof mmio) return 0;
    return mmio + off;
}
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }

static void process(void) {
    uint8_t* mem = (uint8_t*)(uintptr_t)dv.desc;
    volatile uint16_t* avail = (volatile uint16_t*)(uintptr_t)dv.avail;
    volatile uint16_t* used = (volatile uint16_t*)(uintptr_t)dv.used;
    while (dv.last_avail != avail[1]) {
        uint16_t head = avail[2 + (dv.last_avail & (dv.qsize - 1))];
        typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } desc_t;
        desc_t* d = (desc_t*)mem; desc_t chain[8]; int n = 0; uint16_t cur = head;
        for (;;) { CHECK(n < 8 && cur < dv.qsize); chain[n] = d[cur]; n++; if (!(d[cur].flags & 1)) break; cur = d[cur].next; }
        dv.nreq++;
        CHECK(chain[0].len == 16 && !(chain[0].flags & 2));
        uint32_t type; uint64_t sector; memcpy(&type, (void*)(uintptr_t)chain[0].addr, 4); memcpy(&sector, (void*)(uintptr_t)(chain[0].addr + 8), 8);
        desc_t* st = &chain[n - 1];
        CHECK(st->len == 1 && (st->flags & 2));
        uint8_t* sp = (uint8_t*)(uintptr_t)st->addr; uint32_t written = 1;
        if (type == 4) { CHECK(n == 2); dv.nflush++; *sp = 0; }
        else if (type == 0 || type == 1) {
            CHECK(n == 3 && chain[1].len % 512 == 0 && chain[1].len > 0);
            CHECK(((chain[1].flags & 2) != 0) == (type == 0));            /* чтение: устройство пишет в буфер */
            if (opt.size_max) CHECK(chain[1].len <= (uint32_t)opt.size_max);
            if ((int)chain[1].len > dv.max_len) dv.max_len = (int)chain[1].len;
            uint64_t cnt = chain[1].len / 512;
            if (sector + cnt > DISK_SECT) *sp = 1;
            else if (type == 0) { memcpy((void*)(uintptr_t)chain[1].addr, vdisk + sector * 512, chain[1].len); *sp = 0; written += chain[1].len; dv.nread++; }
            else { if (opt.ro) *sp = 1; else { memcpy(vdisk + sector * 512, (void*)(uintptr_t)chain[1].addr, chain[1].len); *sp = 0; dv.nwrite++; } }
        } else *sp = 2;
        uint16_t ui = used[1];
        uint32_t* ring = (uint32_t*)(used + 2);
        ring[2 * (ui & (dv.qsize - 1))] = head; ring[2 * (ui & (dv.qsize - 1)) + 1] = written;
        __sync_synchronize();
        used[1] = (uint16_t)(ui + 1);
        dv.last_avail++;
    }
}

uint64_t host_mmio_read(const volatile void* a, int size) {
    uint64_t o = (const uint8_t*)a - mmio, v = 0;
    if (o < 0x38) {
        switch (o) {
        case 0: return dv.dfsel;
        case 4: { uint64_t f = DEVF(opt); return dv.dfsel == 0 ? (uint32_t)f : (uint32_t)(f >> 32); }
        case 8: return dv.gfsel;
        case 12: return dv.gf[dv.gfsel & 1];
        case 18: return 1;
        case 20: return dv.status;
        case 22: return dv.qsel;
        case 24: return dv.qsel == 0 ? 256 : 0;                 /* устройство предлагает 256: драйвер обязан ограничить */
        case 28: return dv.qen;
        case 30: return 1;                                         /* смещение уведомления - 1 (проверка множителя) */
        }
        return 0;
    }
    memcpy(&v, mmio + o, size); return v;
}

void host_mmio_write(volatile void* a, int size, uint64_t v) {
    uint64_t o = (uint8_t*)a - mmio;
    if (o < 0x38) {
        switch (o) {
        case 0: dv.dfsel = (uint32_t)v; return;
        case 8: dv.gfsel = (uint32_t)v; return;
        case 12: dv.gf[dv.gfsel & 1] = (uint32_t)v; dv.features_written++; return;
        case 20: {
            uint8_t s = (uint8_t)v;
            if (s == 0) { memset(&dv, 0, sizeof dv); return; }                        /* сброс */
            if (s & 0x80) { dv.status = s; return; }
            if (!(s & 1) || (dv.status & ~s)) viol++;                                   /* биты только добавляются, начиная с ACK */
            if ((s & 8) && !(dv.status & 8)) {
                if (!(s & 2) || dv.features_written < 2) viol++;                        /* признаки пишутся до FEATURES_OK */
                if (!(dv.gf[1] & 1)) viol++;                                            /* VERSION_1 обязателен */
                if (opt.reject_features) { dv.status = (uint8_t)(s & ~8); return; }
            }
            if ((s & 4) && !(dv.status & 4)) { if (!(s & 8) || !dv.queue_programmed) viol++; }
            dv.status = s; return;
        }
        case 22: dv.qsel = (uint16_t)v; return;
        case 24: dv.qsize = (uint16_t)v; if (v > 256 || (v & (v - 1))) viol++; return;
        case 26: return;
        case 28: if (v == 1) { if (!dv.desc || !dv.avail || !dv.used || !dv.qsize || (dv.desc & 15) || (dv.avail & 1) || (dv.used & 3) || !(dv.status & 8) || (dv.status & 4)) viol++; dv.qen = 1; dv.queue_programmed = 1; } return;
        case 32: dv.desc = (dv.desc & ~0xFFFFFFFFull) | (uint32_t)v; return;
        case 36: dv.desc = (dv.desc & 0xFFFFFFFFull) | (v << 32); return;
        case 40: dv.avail = (dv.avail & ~0xFFFFFFFFull) | (uint32_t)v; return;
        case 44: dv.avail = (dv.avail & 0xFFFFFFFFull) | (v << 32); return;
        case 48: dv.used = (dv.used & ~0xFFFFFFFFull) | (uint32_t)v; return;
        case 52: dv.used = (dv.used & 0xFFFFFFFFull) | (v << 32); return;
        }
        viol++; return;
    }
    if (o == 0x1004) {                                                                  /* уведомление: смещение 1 * множитель 4 */
        if (!(dv.status & 4) || v != 0) viol++;
        dv.kick_wait = opt.drop ? -1 : 3; return;
    }
    if (o >= 0x1000 && o < 0x1100) { viol++; return; }                                  /* уведомление не по тому адресу */
    memcpy(mmio + o, &v, size);
}

void host_relax(void) {
    if (dv.kick_wait > 0 && --dv.kick_wait == 0) process();                           /* устройство отвечает не мгновенно */
}

/* ---------- сборка модели ---------- */
static void mkhdr(fdev_t* d, uint16_t ven, uint16_t dev, uint8_t cls, uint8_t sub, uint8_t hdr) {
    memset(d, 0, sizeof *d); d->present = 1;
    put16(d, 0, ven); put16(d, 2, dev); d->cfg[0x0B] = cls; d->cfg[0x0A] = sub; d->cfg[0x0E] = hdr;
}
static void vcap(fdev_t* d, int at, int next, int type, int bar, uint32_t off, uint32_t len, int clen) {
    d->cfg[at] = 9; d->cfg[at + 1] = (uint8_t)next; d->cfg[at + 2] = (uint8_t)clen; d->cfg[at + 3] = (uint8_t)type; d->cfg[at + 4] = (uint8_t)bar;
    put32(d, at + 8, off); put32(d, at + 12, len);
}
static uint8_t* vblk_cfg_space;

static void model_init(vopt_t o, uint16_t devid) {
    /* Previous probe must relinquish its device DMA before the model memory is reset. */
    virtio_blk_shutdown();
    opt = o; memset(&dv, 0, sizeof dv); memset(bus_dev, 0, sizeof bus_dev); memset(mmio, 0, sizeof mmio); viol = 0;
    mkhdr(F(0, 0, 0), 0x8086, 0x1237, 6, 0, 0);                                         /* хост-мост */
    mkhdr(F(0, 1, 0), 0x8086, 0x1111, 6, 4, 1); F(0, 1, 0)->cfg[0x19] = 1;              /* мост на шину 1 */
    mkhdr(F(0, 2, 0), 0x8086, 0x7010, 1, 1, 0x80);                                       /* многофункциональное */
    F(0, 2, 0)->bar_size[4] = 32; F(0, 2, 0)->bar_io[4] = 1; put32(F(0, 2, 0), 0x20, 0xC041);
    mkhdr(F(0, 2, 1), 0x8086, 0x7011, 12, 3, 0);
    fdev_t* v = F(1, 0, 0);
    mkhdr(v, 0x1AF4, devid, 1, 0, 0);
    put16(v, 6, 0x10); v->cfg[0x34] = 0x40;
    v->bar_size[4] = 0x4000; v->bar_64[4] = 1; put32(v, 0x20, 0xFE000000u | 0xC); put32(v, 0x24, 0);
    if (!o.legacy_only) {
        vcap(v, 0x40, 0x54, 1, 4, 0x0000, 0x38, 16);
        vcap(v, 0x54, 0x68, 2, 4, 0x1000, 0x100, 20); put32(v, 0x54 + 16, 4);
        vcap(v, 0x68, 0x78, 3, 4, 0x2000, 4, 16);
        vcap(v, 0x78, 0, 4, 4, 0x3000, 0x20, 16);
    } else { v->cfg[0x40] = 9; v->cfg[0x41] = 0; v->cfg[0x43] = 5; }                      /* только PCI cfg access cap */
    (void)vblk_cfg_space;
    uint64_t cap = o.small_disk ? 8 : DISK_SECT; memcpy(mmio + 0x3000, &cap, 8);
    uint32_t smax = (uint32_t)o.size_max; memcpy(mmio + 0x3008, &smax, 4);
    static const pci_backend_t be = { cfg_read, cfg_write };
    pci_set_backend(&be);
}

static uint8_t buf[300 * 512], ref[300 * 512];
static uint8_t ecam_probe[1u << 20] __attribute__((aligned(1u << 20)));
static void rnd(uint8_t* p, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (uint8_t)(seed >> 16); } }

int main(void) {
    /* DMA allocator contract: reject invalid sizes/outputs, page-align and zero host buffers. */
    uint64_t dma_phys = 0;
    CHECK(virtio_dma_alloc(0, &dma_phys) == 0);
    CHECK(virtio_dma_alloc(4096, 0) == 0);
    uint8_t* dma = (uint8_t*)virtio_dma_alloc(4097, &dma_phys);
    CHECK(dma && dma_phys == (uint64_t)(uintptr_t)dma);
    CHECK((((uintptr_t)dma) & 4095u) == 0);
    for (unsigned i = 0; i < 4097; i++) CHECK(dma[i] == 0);
    CHECK(virtio_dma_free(0, dma_phys, 4097) == -1);
    CHECK(virtio_dma_free(dma, dma_phys + 4096, 4097) == -1);
    CHECK(virtio_dma_free(dma, dma_phys, 4096) == -1); /* размер должен совпадать */
    CHECK(virtio_dma_sync_for_device(dma + 4096, dma_phys + 4096, 1) == 0);
    CHECK(virtio_dma_sync_for_cpu(dma + 4096, dma_phys + 4096, 1) == 0);
    CHECK(virtio_dma_sync_for_device(dma + 4096, dma_phys + 4096, 2) == -1);
    CHECK(virtio_dma_free(dma, dma_phys, 4097) == 0);
    CHECK(virtio_dma_free(dma, dma_phys, 4097) == -1); /* double free отвергается */

    /* Registry capacity is bounded; freeing a slot makes it reusable. */
    void* dma_slots[64];
    uint64_t dma_addresses[64];
    for (unsigned i = 0; i < 64; i++) {
        dma_slots[i] = virtio_dma_alloc(1, &dma_addresses[i]);
        CHECK(dma_slots[i] != 0 && dma_addresses[i] != 0);
    }
    uint64_t overflow_phys = 0;
    CHECK(virtio_dma_alloc(1, &overflow_phys) == 0 && overflow_phys == 0);
    for (unsigned i = 0; i < 64; i++)
        CHECK(virtio_dma_free(dma_slots[i], dma_addresses[i], 1) == 0);
    uint64_t reuse_phys = 0;
    void* reuse = virtio_dma_alloc(1, &reuse_phys);
    CHECK(reuse != 0 && reuse_phys != 0);
    CHECK(virtio_dma_free(reuse, reuse_phys, 1) == 0);

    vopt_t o0; memset(&o0, 0, sizeof o0);
    /* --- PCI BAR window arithmetic: invalid ranges must not mutate the allocator. --- */
    pci_window_t win = { .pci_base = 0x10000000u, .size = 0x10000u, .next = 0x10000000u, .cpu_off = 0x20000000 };
    uint64_t pa = 0, ca = 0;
    CHECK(pci_window_alloc(&win, 0x1000, &pa, &ca) == 0 && pa == 0x10000000u && ca == 0x30000000u);
    CHECK(win.next == 0x10001000u);
    pci_window_t neg = { .pci_base = 0x1000, .size = 0x4000, .next = 0x1000, .cpu_off = -0x800 };
    CHECK(pci_window_alloc(&neg, 0x1000, &pa, &ca) == 0 && pa == 0x1000 && ca == 0x800);
    pci_window_t ov_end = { .pci_base = UINT64_MAX - 0xFFF, .size = 0x2000, .next = UINT64_MAX - 0xFFF, .cpu_off = 0 };
    uint64_t old_next = ov_end.next;
    CHECK(pci_window_alloc(&ov_end, 0x1000, &pa, &ca) == -1 && ov_end.next == old_next);
    pci_window_t ov_cpu = { .pci_base = UINT64_MAX - 0x1FFF, .size = 0x1000, .next = UINT64_MAX - 0x1FFF, .cpu_off = 0x2000 };
    old_next = ov_cpu.next;
    CHECK(pci_window_alloc(&ov_cpu, 0x1000, &pa, &ca) == -1 && ov_cpu.next == old_next);
    pci_window_t under_cpu = { .pci_base = 0x1000, .size = 0x1000, .next = 0x1000, .cpu_off = -0x2000 };
    old_next = under_cpu.next;
    CHECK(pci_window_alloc(&under_cpu, 0x1000, &pa, &ca) == -1 && under_cpu.next == old_next);
    pci_window_t misaligned = { .pci_base = 0x1000, .size = 0x3000, .next = 0x1001, .cpu_off = 0 };
    CHECK(pci_window_alloc(&misaligned, 0x1000, &pa, &ca) == 0 && pa == 0x2000 && ca == 0x2000);
    CHECK(pci_window_alloc(&win, 0x1800, &pa, &ca) == -1);
    /* Multiple disjoint host-bridge windows: exhaust the first, then allocate from the second. */
    pci_window_set_t windows = {0};
    pci_window_t wsmall = { .pci_base = 0x40000000u, .size = 0x1000, .next = 0x40000000u, .cpu_off = 0x10000000 };
    pci_window_t wlarge = { .pci_base = 0x50000000u, .size = 0x8000, .next = 0x50000000u, .cpu_off = -0x10000000 };
    CHECK(pci_window_set_add(&windows, &wsmall) == 0);
    CHECK(pci_window_set_add(&windows, &wlarge) == 0);
    CHECK(pci_window_set_alloc(&windows, 0x2000, &pa, &ca) == 0 &&
          pa == 0x50000000u && ca == 0x40000000u);
    pci_window_t overlap = { .pci_base = 0x50001000u, .size = 0x1000, .next = 0x50001000u, .cpu_off = 0 };
    CHECK(pci_window_set_add(&windows, &overlap) == -1 && windows.count == 2);
    pci_window_t cpu_alias = { .pci_base = 0x70000000u, .size = 0x1000,
                               .next = 0x70000000u, .cpu_off = -0x30000000 };
    CHECK(pci_window_set_add(&windows, &cpu_alias) == -1 && windows.count == 2);
    pci_window_set_t full = {0};
    for (unsigned wi = 0; wi < PCI_MAX_WINDOWS; wi++) {
        pci_window_t item = { .pci_base = 0x60000000u + (uint64_t)wi * 0x2000u,
                              .size = 0x1000, .next = 0x60000000u + (uint64_t)wi * 0x2000u, .cpu_off = 0 };
        CHECK(pci_window_set_add(&full, &item) == 0);
    }
    CHECK(pci_window_set_add(&full, &wsmall) == -1 && full.count == PCI_MAX_WINDOWS);

    /* Exercise the real BAR programming path: a 64-bit BAR must skip an undersized
       first window, use the second window, and program both address halves. */
    memset(bus_dev, 0, sizeof bus_dev);
    mkhdr(F(0, 0, 0), 0x1234, 0x5678, 2, 0, 0);
    F(0, 0, 0)->bar_size[0] = 0x2000;
    F(0, 0, 0)->bar_64[0] = 1;
    put32(F(0, 0, 0), 0x10, 0xC);
    static const pci_backend_t assign_be = { cfg_read, cfg_write };
    pci_set_backend(&assign_be);
    CHECK(pci_init() == 1);
    const pci_dev_t* assigned = pci_get(0);
    CHECK(assigned && assigned->bar[0].mem64 && assigned->bar[0].size == 0x2000 && assigned->bar[0].base == 0);
    pci_window_set_t assign32 = {0}, assign64 = {0};
    pci_window_t too_small = { .pci_base = 0x100000000ull, .size = 0x1000,
                               .next = 0x100000000ull, .cpu_off = 0 };
    pci_window_t usable = { .pci_base = 0x200000000ull, .size = 0x8000,
                            .next = 0x200000000ull, .cpu_off = -0x10000000 };
    CHECK(pci_window_set_add(&assign64, &too_small) == 0);
    CHECK(pci_window_set_add(&assign64, &usable) == 0);
    CHECK(pci_assign_multi(&assign32, &assign64) == 1);
    CHECK(assigned->bar[0].base == 0x1F0000000ull);
    CHECK(pci_read32(assigned, 0x10) == 0x0000000Cu);
    CHECK(pci_read32(assigned, 0x14) == 0x00000002u);

    /* --- PCI --- */
    model_init(o0, 0x1042);
    CHECK(pci_use_ecam(ecam_probe, 1u << 20, 0, 1) == -1); /* advertised two buses do not fit; backend stays unchanged */
    int n = pci_init();
    CHECK(n == 5);
    const pci_dev_t* v = pci_find(0x1AF4, 0x1042);
    CHECK(v && v->bus == 1 && v->dev == 0 && v->bar[4].mem64 && v->bar[4].size == 0x4000 && v->bar[4].base == 0xFE000000u && v->bar[4].prefetch);
    const pci_dev_t* io = pci_find(0x8086, 0x7010);
    CHECK(io && io->bar[4].io && io->bar[4].size == 32 && io->bar[4].base == 0xC040);
    CHECK(pci_find(0x8086, 0x7011) && !pci_find(0x1234, 5));
    CHECK(pci_read16(v, 4) == 0);                                          /* зонд BAR вернул команду на место */
    uint8_t id; uint16_t c = pci_next_cap(v, 0, &id); int caps = 0;
    while (c && caps < 10) { CHECK(id == 9); caps++; c = pci_next_cap(v, c, &id); }
    CHECK(caps == 4);

    /* --- virtio-blk: нормальный путь --- */
    blk_dev_t* d = virtio_blk_probe();
    CHECK(d && d->sectors == DISK_SECT && viol == 0);
    CHECK(dv.status == 15 && (pci_read16(v, 4) & 0x406) == 0x406);          /* ACK|DRIVER|FEATURES_OK|DRIVER_OK; память+мастер+INTx отключён */
    CHECK(dv.qsize == 64);                                                 /* очередь ограничена драйвером */
    rnd(buf, sizeof buf, 1); memcpy(ref, buf, sizeof buf);
    static const uint32_t counts[] = { 1, 2, 127, 128, 129, 300 };
    uint64_t lba = 3;
    for (unsigned i = 0; i < 6; i++) {
        rnd(buf, counts[i] * 512, 10 + i); memcpy(ref, buf, counts[i] * 512);
        int w0 = dv.nwrite, r0 = dv.nread;
        CHECK(d->write(d, lba, counts[i], buf) == 0);
        CHECK(memcmp(vdisk + lba * 512, ref, counts[i] * 512) == 0);
        CHECK(dv.nwrite - w0 == (int)((counts[i] + 127) / 128));            /* кусками по 64 КБ */
        memset(buf, 0, sizeof buf);
        CHECK(d->read(d, lba, counts[i], buf) == 0 && memcmp(buf, ref, counts[i] * 512) == 0);
        CHECK(dv.nread - r0 == (int)((counts[i] + 127) / 128));
        lba += 301;
    }
    CHECK(dv.max_len == 128 * 512);
    CHECK(d->flush(d) == 0 && dv.nflush == 1);
    CHECK(d->read(d, DISK_SECT - 1, 2, buf) == -1 && d->write(d, DISK_SECT, 1, buf) == -1);
    CHECK(d->read(d, DISK_SECT - 1, 1, buf) == 0);                          /* последний сектор */
    CHECK(viol == 0);

    /* --- KiraFS поверх virtio --- */
    memset(vdisk, 0, sizeof vdisk);
    CHECK(kfs_format(d, 16) == 0);
    rnd(buf, 70000, 99);
    CHECK(kfs_write("/f", buf, 70000) == 0);
    CHECK(kfs_mount(d, 16) == 0);
    memset(ref, 0, sizeof ref);
    CHECK(kfs_read("/f", 69000, ref, 1000) == 1000 && memcmp(ref, buf + 69000, 1000) == 0);

    /* --- size_max ограничивает заявки --- */
    vopt_t o1 = o0; o1.size_max = 4096; model_init(o1, 0x1001);              /* «переходный» идентификатор */
    pci_init(); d = virtio_blk_probe(); CHECK(d && viol == 0);
    rnd(buf, 40 * 512, 5);
    CHECK(d->write(d, 100, 40, buf) == 0 && dv.max_len == 4096 && dv.nwrite == 5);
    memset(ref, 0, sizeof ref); CHECK(d->read(d, 100, 40, ref) == 0 && memcmp(ref, buf, 40 * 512) == 0);

    /* --- без flush: сброс - пустая операция, и заявка не уходит --- */
    vopt_t o2 = o0; o2.no_flush = 1; model_init(o2, 0x1042); pci_init(); d = virtio_blk_probe();
    CHECK(d && d->flush(d) == 0 && dv.nflush == 0);

    /* --- только чтение --- */
    vopt_t o3 = o0; o3.ro = 1; model_init(o3, 0x1042); pci_init(); d = virtio_blk_probe();
    CHECK(d && d->write(d, 0, 1, buf) == -6 && dv.nwrite == 0 && d->read(d, 0, 1, buf) == 0);

    /* --- отказы согласования --- */
    vopt_t o4 = o0; o4.no_v1 = 1; model_init(o4, 0x1042); pci_init();
    CHECK(virtio_blk_probe() == 0 && (dv.status == 0));                      /* FAILED и сброс */
    vopt_t o5 = o0; o5.reject_features = 1; model_init(o5, 0x1042); pci_init();
    CHECK(virtio_blk_probe() == 0 && dv.status == 0);
    vopt_t o6 = o0; o6.legacy_only = 1; model_init(o6, 0x1000); pci_init();
    CHECK(pci_find(0x1AF4, 0x1000) && virtio_blk_probe() == 0);

    /* Repeated late probe failures must release both queue and bounce DMA allocations. */
    vopt_t o8 = o0; o8.small_disk = 1;
    for (unsigned i = 0; i < 70; i++) {
        model_init(o8, 0x1042); pci_init();
        CHECK(virtio_blk_probe() == 0);
        if (i == 0) {
            const pci_dev_t* failed = pci_find(0x1AF4, 0x1042);
            CHECK(failed && !(pci_read16(failed, 4) & PCI_CMD_MASTER));
        }
    }

    /* --- устройство молчит: таймаут, отключение, дальше ошибки без зависания --- */
    vopt_t o7 = o0; o7.drop = 1; model_init(o7, 0x1042); pci_init(); d = virtio_blk_probe();
    CHECK(d);
    CHECK(d->read(d, 0, 1, buf) == -4);
    CHECK(d->read(d, 0, 1, buf) == -5 && d->write(d, 0, 1, buf) == -5);
    CHECK(dv.status == 0);                                                    /* устройство сброшено: в нашу память больше не пишет */
    CHECK(viol == 0);

    /* A timed-out device must release its DMA ownership after reset and permit a clean reprobe. */
    model_init(o0, 0x1042); pci_init();
    d = virtio_blk_probe();
    CHECK(d && d->read(d, 0, 1, buf) == 0);
    vopt_t o9 = o0; o9.small_disk = 1;
    model_init(o9, 0x1042); pci_init();
    CHECK(virtio_blk_probe() == 0);

    /* ECAM access is bounded by the mapped byte length and the advertised bus range. */
    memset(ecam_probe, 0, sizeof ecam_probe);
    uint32_t ecam_id = 0x56781234u, got = 0;
    memcpy(ecam_probe, &ecam_id, sizeof ecam_id);
    CHECK(pci_use_ecam(ecam_probe, sizeof ecam_probe, 5, 5) == 0);
    pci_dev_t ep; memset(&ep, 0, sizeof ep); ep.bus = 5;
    CHECK(pci_read32(&ep, 0) == ecam_id);
    ep.bus = 6; CHECK(pci_read32(&ep, 0) == 0xFFFFFFFFu);
    pci_write32(&ep, 0, 0xAABBCCDDu);
    memcpy(&got, ecam_probe, sizeof got); CHECK(got == ecam_id);
    ep.bus = 5; ep.dev = 32; CHECK(pci_read32(&ep, 0) == 0xFFFFFFFFu);
    ep.dev = 0; ep.fn = 8; CHECK(pci_read32(&ep, 0) == 0xFFFFFFFFu);
    ep.fn = 0; CHECK(pci_read32(&ep, 4096) == 0xFFFFFFFFu);
    CHECK(pci_use_ecam(ecam_probe, sizeof ecam_probe, 6, 5) == -1);

    printf("virtio: PCI bus/bridge/BAR/capabilities, bounded ECAM, virtio 1.0 init order, rings, chunking, size_max, RO, flush, timeout, KiraFS over virtio passed\n");
    return 0;
}
