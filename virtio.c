#include "virtio.h"
#include "uart.h"

#ifndef KIRA_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "cpu.h"
/* Ожидание ответа устройства. Сначала кручение до 1 мс по TSC (обычно ответ приходит за микросекунды), потом поток
   засыпает на тик между проверками: ядро уходит в hlt и не отнимает процессор у остальных (в эмуляторе - у потока,
   который как раз обрабатывает запрос устройства; при всех ядрах в нагрузке кручение растягивало запрос на секунды).
   Срок - по времени, а не по числу проверок. До запуска планировщика и с выключенными прерываниями - только кручение. */
#include "msi.h"
#include "iommu.h"
/* С прерыванием: короткое ожидание на месте (ответ часто приходит за микросекунды), затем сон до прерывания
   очереди; срок сна - тик, так что потерянное сообщение стоит не больше прежнего опроса. */
#define IW_MARK()       uint32_t iw_mark = q->iw ? irqwait_mark(q->iw) : 0
#define IW_READY(t0)    (q->iw && clock_tsc_khz() && rdtsc() - (t0) > clock_tsc_khz() / 50 && sched_can_sleep())
#define IW_SLEEP()      do { irqwait_wait(q->iw, iw_mark, 1); iw_mark = irqwait_mark(q->iw); } while (0)
#define WAIT_T0()       rdtsc()
#define WAIT_SPUN(t0)   (clock_tsc_khz() && rdtsc() - (t0) > clock_tsc_khz())   /* 1 мс */
#define WAIT_SLEEP_OK() sched_can_sleep()
#define WAIT_SLEEP()    sched_sleep_ticks(1)
#define WAIT_NOW()      timer_get_ticks()
#define WAIT_LIMIT      (10ULL * TIMER_HZ)
#else
#define IW_MARK()       ((void)0)
#define IW_READY(t0)    ((void)(t0), 0)
#define IW_SLEEP()      ((void)0)
#define WAIT_T0()       0ULL
#define WAIT_SPUN(t0)   ((void)(t0), 0)
#define WAIT_SLEEP_OK() 0
#define WAIT_SLEEP()    ((void)0)
#define WAIT_NOW()      0ULL
#define WAIT_LIMIT      0ULL
#include <stdlib.h>
#include <string.h>
#endif

#ifndef SPIN_LIMIT
#define SPIN_LIMIT 200000000u
#endif
#define VIRTQ_MAX 128

static void zero(void* p, size_t n) { volatile uint8_t* q = (volatile uint8_t*)p; while (n--) *q++ = 0; }
static void barrier(void) { __sync_synchronize(); }

void* platform_dma_pool(uint32_t bytes, uint64_t* phys, int below_4g) __attribute__((weak));   /* ARM64: пул без кэша для плат без согласованности */
void* virtio_dma_alloc(uint32_t bytes, uint64_t* phys) {
#ifdef KIRA_HOST_TEST
    void* p = aligned_alloc(4096, (bytes + 4095) & ~4095u);
    if (!p) return 0;
    memset(p, 0xA5, bytes);                                  /* не нули: драйвер обязан обнулять сам */
    zero(p, bytes);
    *phys = (uint64_t)(uintptr_t)p;
    return p;
#else
    if (platform_dma_pool) { void* q = platform_dma_pool(bytes, phys, 0); if (q) return q; }
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t pa = pmm_alloc_contiguous(pages);
    if (!pa) return 0;
    for (uint32_t i = 0; i < pages; i++) {                   /* устройство и процессор должны видеть один и тот же адрес */
        uint64_t t;
        if (vmm_translate(pa + (uint64_t)i * 4096, &t) != 0 || t != pa + (uint64_t)i * 4096) { pmm_free_contiguous(pa, pages); return 0; }
    }
    zero((void*)(uintptr_t)pa, (size_t)pages * 4096);
    *phys = pa;
    return (void*)(uintptr_t)pa;
#endif
}

/* ---- Транспорт PCI ---- */

enum { CAP_COMMON = 1, CAP_NOTIFY = 2, CAP_ISR = 3, CAP_DEVICE = 4 };

#ifdef KIRA_HOST_TEST
extern volatile void* host_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len);
#define MAP_BAR host_map_bar
#else
#define MAP_BAR pci_map_bar
#endif

int virtio_attach(virtio_dev_t* v, const pci_dev_t* d) {
    v->pci = d; v->common = v->notify = v->isr = v->devcfg = 0; v->notify_mult = 0; v->features = 0; v->msix_vec = 0xFFFF;
    uint8_t id; uint16_t cap = 0;
    for (int guard = 0; guard < 48; guard++) {
        cap = pci_next_cap(d, cap, &id);
        if (!cap) break;
        if (id != 0x09) continue;                                 /* vendor-specific */
        uint8_t type = pci_read8(d, (uint16_t)(cap + 3)), bar = pci_read8(d, (uint16_t)(cap + 4));
        uint32_t off = pci_read32(d, (uint16_t)(cap + 8)), len = pci_read32(d, (uint16_t)(cap + 12));
        uint8_t clen = pci_read8(d, (uint16_t)(cap + 2));
        uint64_t need = 0;
        switch (type) {
        case CAP_COMMON: need = 56; break;
        case CAP_NOTIFY: need = 2; if (clen < 20) continue; break;
        case CAP_ISR:    need = 1; break;
        case CAP_DEVICE: need = 1; break;
        default: continue;
        }
        if (len < need || bar >= PCI_BARS) continue;
        volatile uint8_t* p = (volatile uint8_t*)MAP_BAR(d, bar, off, len);
        if (!p) { uart_print("[KIRA VIRTIO] cannot map BAR region\n"); return -2; }
        switch (type) {
        case CAP_COMMON: if (!v->common) v->common = p; break;
        case CAP_NOTIFY: if (!v->notify) { v->notify = p; v->notify_mult = pci_read32(d, (uint16_t)(cap + 16)); } break;
        case CAP_ISR:    if (!v->isr) v->isr = p; break;
        case CAP_DEVICE: if (!v->devcfg) v->devcfg = p; break;
        }
    }
    if (!v->common || !v->notify || !v->isr) return -1;
    pci_enable(d, PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF);
    return 0;
}

/* Регистры common cfg */
#define R_DFSEL 0
#define R_DF 4
#define R_GFSEL 8
#define R_GF 12
#define R_NUMQ 18
#define R_STATUS 20
#define R_QSEL 22
#define R_QSIZE 24
#define R_QMSIX 26
#define R_QEN 28
#define R_QNOFF 30
#define R_QDESC 32
#define R_QAVAIL 40
#define R_QUSED 48

static uint8_t status_get(virtio_dev_t* v) { return MMIO_R8(v->common + R_STATUS); }
static void status_set(virtio_dev_t* v, uint8_t s) { MMIO_W8(v->common + R_STATUS, s); }

static int reset(virtio_dev_t* v) {
    status_set(v, 0);
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) { if (status_get(v) == 0) return 0; CPU_RELAX(); }
    return -1;
}

void virtio_fail(virtio_dev_t* v) {
    status_set(v, status_get(v) | VIRTIO_ST_FAILED);
    (void)reset(v);
}

int virtio_begin(virtio_dev_t* v, uint64_t want) {
    if (reset(v)) return -1;
    status_set(v, VIRTIO_ST_ACK);
    status_set(v, VIRTIO_ST_ACK | VIRTIO_ST_DRIVER);
    MMIO_W32(v->common + R_DFSEL, 0); uint64_t offered = MMIO_R32(v->common + R_DF);
    MMIO_W32(v->common + R_DFSEL, 1); offered |= (uint64_t)MMIO_R32(v->common + R_DF) << 32;
    if (!(offered & VIRTIO_F_VERSION_1)) { virtio_fail(v); return -2; }       /* только современный интерфейс */
    v->features = VIRTIO_F_VERSION_1 | (want & offered);
    MMIO_W32(v->common + R_GFSEL, 0); MMIO_W32(v->common + R_GF, (uint32_t)v->features);
    MMIO_W32(v->common + R_GFSEL, 1); MMIO_W32(v->common + R_GF, (uint32_t)(v->features >> 32));
    status_set(v, VIRTIO_ST_ACK | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(status_get(v) & VIRTIO_ST_FEATURES_OK)) { virtio_fail(v); return -3; }    /* устройство отвергло набор */
    return 0;
}

int virtio_dma_grant(virtio_dev_t* v, uint64_t phys, uint32_t bytes) {
#ifdef KIRA_HOST_TEST
    (void)v; (void)phys; (void)bytes;
    return 0;
#else
    if (!iommu_device_isolated(v->pci)) return 0;
    return iommu_map(v->pci, phys, phys, bytes, IOMMU_R | IOMMU_W);
#endif
}

/* granted == 0 только для проверки принуждения (virtio_iommu_check): память очереди остаётся невыданной. */
static int queue_setup(virtio_dev_t* v, virtq_t* q, uint16_t index, uint16_t max_size, int granted) {
    MMIO_W16(v->common + R_QSEL, index);
    uint16_t dev_max = MMIO_R16(v->common + R_QSIZE);
    if (dev_max == 0) return -1;                                  /* очереди нет */
    if (MMIO_R16(v->common + R_QEN)) return -1;
    uint16_t n = dev_max < max_size ? dev_max : max_size;
    if (n > VIRTQ_MAX) n = VIRTQ_MAX;
    while (n & (n - 1)) n &= (uint16_t)(n - 1);                   /* степень двойки */
    if (n < 4) return -1;
    q->dev = v; q->index = index; q->size = n; q->last_used = 0; q->avail_idx = 0;
    q->avail_off = (uint32_t)n * 16;
    q->used_off = (q->avail_off + 6 + 2 * (uint32_t)n + 3) & ~3u;
    q->mem = (uint8_t*)virtio_dma_alloc(q->used_off + 6 + 8 * (uint32_t)n, &q->mem_phys);
    if (!q->mem) return -2;
    if (granted && virtio_dma_grant(v, q->mem_phys, q->used_off + 6 + 8 * (uint32_t)n) != 0) return -3;
    MMIO_W16(v->common + R_QSIZE, n);
    MMIO_W16(v->common + R_QMSIX, v->msix_vec);                  /* запись MSI-X очереди или 0xFFFF - без прерываний */
    q->iw = 0;
    if (v->msix_vec != 0xFFFF && MMIO_R16(v->common + R_QMSIX) != v->msix_vec) MMIO_W16(v->common + R_QMSIX, 0xFFFF);
    MMIO_W32(v->common + R_QDESC, (uint32_t)q->mem_phys);              MMIO_W32(v->common + R_QDESC + 4, (uint32_t)(q->mem_phys >> 32));
    MMIO_W32(v->common + R_QAVAIL, (uint32_t)(q->mem_phys + q->avail_off)); MMIO_W32(v->common + R_QAVAIL + 4, (uint32_t)((q->mem_phys + q->avail_off) >> 32));
    MMIO_W32(v->common + R_QUSED, (uint32_t)(q->mem_phys + q->used_off));   MMIO_W32(v->common + R_QUSED + 4, (uint32_t)((q->mem_phys + q->used_off) >> 32));
    uint16_t noff = MMIO_R16(v->common + R_QNOFF);
    MMIO_W16(v->common + R_QEN, 1);
    q->notify_addr = v->notify + (uint64_t)noff * v->notify_mult;
    return 0;
}

int virtio_queue_setup(virtio_dev_t* v, virtq_t* q, uint16_t index, uint16_t max_size) {
    return queue_setup(v, q, index, max_size, 1);
}

void virtio_driver_ok(virtio_dev_t* v) {
    status_set(v, status_get(v) | VIRTIO_ST_DRIVER_OK);
}

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } desc_t;

/* Очередь событий (устройство пишет, ядро забирает без ожидания): дескриптор i всегда указывает на буфер i. */
void virtq_post(virtq_t* q, uint16_t id, uint64_t addr, uint32_t len, int device_writes) {
    desc_t* desc = (desc_t*)q->mem;
    desc[id].addr = addr; desc[id].len = len; desc[id].flags = (uint16_t)(device_writes ? 2 : 0); desc[id].next = 0;
    volatile uint16_t* avail = (volatile uint16_t*)(q->mem + q->avail_off);
    avail[2 + (q->avail_idx & (q->size - 1))] = id;
    barrier();
    q->avail_idx++;
    avail[1] = q->avail_idx;
    barrier();
}
void virtq_kick(virtq_t* q) { MMIO_W16(q->notify_addr, q->index); }
int virtq_take(virtq_t* q, uint16_t* id, uint32_t* len) {
    volatile uint16_t* used = (volatile uint16_t*)(q->mem + q->used_off);
    barrier();
    if (used[1] == q->last_used) return 0;
    volatile uint32_t* ring = (volatile uint32_t*)(q->mem + q->used_off + 4);
    uint32_t slot = q->last_used & (q->size - 1);
    *id = (uint16_t)ring[slot * 2]; *len = ring[slot * 2 + 1];
    q->last_used++;
    barrier();
    return 1;
}

int virtq_run(virtq_t* q, const virtq_buf_t* bufs, int n) {
    if (n < 1 || n > 8 || n > q->size) return -2;
    desc_t* desc = (desc_t*)q->mem;
    for (int i = 0; i < n; i++) {
        desc[i].addr = bufs[i].addr; desc[i].len = bufs[i].len;
        desc[i].flags = (uint16_t)((bufs[i].device_writes ? 2 : 0) | (i + 1 < n ? 1 : 0));
        desc[i].next = (uint16_t)(i + 1);
    }
    volatile uint16_t* avail = (volatile uint16_t*)(q->mem + q->avail_off);          /* flags, idx, ring[] */
    volatile uint16_t* used = (volatile uint16_t*)(q->mem + q->used_off);            /* flags, idx, ring[] (id u32, len u32) */
    avail[2 + (q->avail_idx & (q->size - 1))] = 0;                                    /* голова цепочки - дескриптор 0 */
    barrier();
    q->avail_idx++;
    avail[1] = q->avail_idx;
    barrier();
    IW_MARK();
    MMIO_W16(q->notify_addr, q->index);
    uint64_t start = WAIT_NOW(), t0 = WAIT_T0();
    for (uint32_t i = 0; ; i++) {
        barrier();
        if (used[1] != q->last_used) { q->last_used++; barrier(); return 0; }
        if (IW_READY(t0)) {                                      /* прерывание очереди есть: спать до него */
            IW_SLEEP();
            if (WAIT_NOW() - start > WAIT_LIMIT) return -1;
        } else if (WAIT_SPUN(t0) && WAIT_SLEEP_OK()) {
            WAIT_SLEEP();
            if (WAIT_NOW() - start > WAIT_LIMIT) return -1;
        } else {
            if (i >= SPIN_LIMIT) return -1;
            CPU_RELAX();
        }
    }
}

int virtio_iommu_check(virtio_dev_t* v) {
#ifdef KIRA_HOST_TEST
    (void)v;
    return 0;
#else
    if (virtio_begin(v, VIRTIO_F_ACCESS_PLATFORM) != 0) return 0;
    if (!(v->features & VIRTIO_F_ACCESS_PLATFORM)) { virtio_fail(v); return -1; }
    uint32_t before = iommu_fault_count(v->pci);
    virtq_t q;
    if (queue_setup(v, &q, 0, 16, 0) != 0) { virtio_fail(v); return 0; }
    virtio_driver_ok(v);
    virtq_post(&q, 0, q.mem_phys, 16, 1);
    virtq_kick(&q);
    int seen = 0;
    for (uint32_t i = 0; i < 200000 && !seen; i++) {
        (void)iommu_fault_poll();
        seen = iommu_fault_count(v->pci) > before;
        if (!seen) CPU_RELAX();
    }
    virtio_fail(v);                                              /* сброс: устройство больше не обращается к памяти */
    pmm_free_contiguous(q.mem_phys, (q.used_off + 6 + 8 * (uint32_t)q.size + 4095) / 4096);
    return seen;
#endif
}
