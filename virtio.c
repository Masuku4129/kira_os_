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

void* platform_dma_pool(uint32_t bytes, uint64_t* phys, int below_4g) __attribute__((weak));
void platform_dma_free(void* cpu, uint64_t phys, uint32_t bytes) __attribute__((weak));
void platform_dma_sync_for_device(void* cpu, uint64_t phys, uint32_t bytes) __attribute__((weak));
void platform_dma_sync_for_cpu(void* cpu, uint64_t phys, uint32_t bytes) __attribute__((weak));
int platform_dma_noncoherent(void) __attribute__((weak));

/* Track ownership explicitly: a CPU pointer may equal a bus address by chance,
   so pointer/address comparison cannot identify which allocator owns a buffer. */
#define DMA_ALLOCATION_SLOTS 64
enum {
    DMA_OWNER_HOST = 1,
    DMA_OWNER_PLATFORM = 2,
    DMA_OWNER_PMM = 3,
    DMA_OWNER_RELEASING = 0xFF
};
typedef struct {
    void* cpu;
    uint64_t phys;
    uint32_t bytes;
    uint32_t pages;
    uint8_t owner;
} dma_allocation_t;
static dma_allocation_t dma_allocations[DMA_ALLOCATION_SLOTS];
static volatile uint32_t dma_registry_lock;

static void dma_lock(void) {
    while (__sync_lock_test_and_set(&dma_registry_lock, 1)) CPU_RELAX();
}
static void dma_unlock(void) { __sync_lock_release(&dma_registry_lock); }

static int dma_register(void* cpu, uint64_t phys, uint32_t bytes,
                        uint32_t pages, uint8_t owner) {
    int result = -1;
    dma_lock();
    for (unsigned i = 0; i < DMA_ALLOCATION_SLOTS; i++) {
        if (!dma_allocations[i].cpu) {
            dma_allocations[i] = (dma_allocation_t){ cpu, phys, bytes, pages, owner };
            result = 0;
            break;
        }
    }
    dma_unlock();
    return result;
}

#ifndef KIRA_HOST_TEST
static void dma_release_unregistered(void* cpu, uint64_t phys,
                                     uint32_t bytes, uint32_t pages, uint8_t owner) {
    if (owner == DMA_OWNER_PLATFORM) {
        if (platform_dma_free) platform_dma_free(cpu, phys, bytes);
    } else if (owner == DMA_OWNER_PMM) {
        (void)pmm_free_contiguous(phys, pages);
    }
}
#endif

void* virtio_dma_alloc(uint32_t bytes, uint64_t* phys) {
    if (!bytes || !phys || bytes > UINT32_MAX - 4095u) return 0;
    *phys = 0;
    uint32_t pages = (bytes + 4095u) / 4096u;
#ifdef KIRA_HOST_TEST
    size_t rounded = (size_t)pages * 4096u;
    void* cpu = aligned_alloc(4096, rounded);
    if (!cpu) return 0;
    zero(cpu, rounded);
    uint64_t device_addr = (uint64_t)(uintptr_t)cpu;
    if (dma_register(cpu, device_addr, bytes, pages, DMA_OWNER_HOST) != 0) {
        free(cpu);
        return 0;
    }
#else
    void* cpu = 0;
    uint64_t device_addr = 0;
    uint8_t owner = DMA_OWNER_PMM;
    if (platform_dma_pool) {
        cpu = platform_dma_pool(bytes, &device_addr, 0);
        if (cpu) {
            uint64_t rounded = (uint64_t)pages * 4096u;
            int aligned = (((uintptr_t)cpu & 4095u) == 0) &&
                          ((device_addr & 4095u) == 0);
            int in_range = device_addr != 0 &&
                           device_addr <= UINT64_MAX - rounded;
            if (aligned && in_range) {
                owner = DMA_OWNER_PLATFORM;
                zero(cpu, bytes);
            } else {
                /* Reject malformed pool results; return them when the pool
                   provides a release hook. Without one, the pool owns the
                   rejected block and must reclaim it internally. */
                if (platform_dma_free)
                    platform_dma_free(cpu, device_addr, bytes);
                cpu = 0;
                device_addr = 0;
            }
        }
    }
    if (!cpu) {
        /* On a non-coherent platform, cacheable PMM pages are never a safe
           substitute for the uncached DMA pool. Fail closed if that pool
           cannot provide a valid block. */
        if (platform_dma_noncoherent && platform_dma_noncoherent()) return 0;
        uint64_t pa = pmm_alloc_contiguous(pages);
        if (!pa) return 0;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t va = pa + (uint64_t)i * 4096u, translated = 0;
            if (vmm_translate(va, &translated) != 0 || translated != va) {
                (void)pmm_free_contiguous(pa, pages);
                return 0;
            }
        }
        cpu = (void*)(uintptr_t)pa;
        device_addr = pa;
        zero(cpu, (size_t)pages * 4096u);
    }
    if (dma_register(cpu, device_addr, bytes, pages, owner) != 0) {
        dma_release_unregistered(cpu, device_addr, bytes, pages, owner);
        return 0;
    }
#endif
    *phys = device_addr;
    return cpu;
}

static int dma_validate_range(void* cpu, uint64_t phys, uint32_t bytes) {
    if (!cpu || !phys || !bytes) return 0;
    int valid = 0;
    dma_lock();
    for (unsigned i = 0; i < DMA_ALLOCATION_SLOTS; i++) {
        dma_allocation_t* a = &dma_allocations[i];
        uintptr_t base = (uintptr_t)a->cpu;
        uintptr_t current = (uintptr_t)cpu;
        if (!a->cpu || current < base || a->owner == DMA_OWNER_RELEASING)
            continue;
        uint64_t offset = (uint64_t)(current - base);
        if (offset > a->bytes || bytes > a->bytes - offset ||
            a->phys > UINT64_MAX - offset || a->phys + offset != phys)
            continue;
        valid = 1;
        break;
    }
    dma_unlock();
    return valid;
}

int virtio_dma_sync_for_device(void* cpu, uint64_t phys, uint32_t bytes) {
    if (!dma_validate_range(cpu, phys, bytes)) return -1;
#ifndef KIRA_HOST_TEST
    if (platform_dma_sync_for_device)
        platform_dma_sync_for_device(cpu, phys, bytes);
#endif
    barrier();
    return 0;
}

int virtio_dma_sync_for_cpu(void* cpu, uint64_t phys, uint32_t bytes) {
    if (!dma_validate_range(cpu, phys, bytes)) return -1;
    barrier();
#ifndef KIRA_HOST_TEST
    if (platform_dma_sync_for_cpu)
        platform_dma_sync_for_cpu(cpu, phys, bytes);
#endif
    barrier();
    return 0;
}

/* Final CI snapshot: virtio DMA lifecycle, ownership sync and device quiesce. */
/* Exact-size release prevents accidental freeing with a mismatched allocation
   contract. Platform pools without a matching free hook remain allocated. */
int virtio_dma_free(void* cpu, uint64_t phys, uint32_t bytes) {
    if (!cpu || !phys || !bytes) return -1;

    dma_allocation_t saved = {0};
    unsigned slot = DMA_ALLOCATION_SLOTS;
    dma_lock();
    for (unsigned i = 0; i < DMA_ALLOCATION_SLOTS; i++) {
        dma_allocation_t* a = &dma_allocations[i];
        if (a->cpu != cpu || a->phys != phys) continue;
        if (a->owner == DMA_OWNER_RELEASING || a->bytes != bytes) {
            dma_unlock();
            return -1;
        }
#ifdef KIRA_HOST_TEST
        if (a->owner != DMA_OWNER_HOST) {
            dma_unlock();
            return -1;
        }
#else
        if (a->owner == DMA_OWNER_PLATFORM && !platform_dma_free) {
            dma_unlock();
            return -2;
        }
        if (a->owner != DMA_OWNER_PLATFORM && a->owner != DMA_OWNER_PMM) {
            dma_unlock();
            return -1;
        }
#endif
        saved = *a;
        a->owner = DMA_OWNER_RELEASING;
        slot = i;
        break;
    }
    dma_unlock();
    if (slot == DMA_ALLOCATION_SLOTS) return -1;

    int rc = 0;
#ifdef KIRA_HOST_TEST
    free(saved.cpu);
#else
    if (saved.owner == DMA_OWNER_PLATFORM)
        platform_dma_free(saved.cpu, saved.phys, saved.bytes);
    else
        rc = pmm_free_contiguous(saved.phys, saved.pages);
#endif

    dma_lock();
    dma_allocation_t* current = &dma_allocations[slot];
    /* Do not inspect the freed CPU pointer; the slot and device address
       identify the allocation while its owner is RELEASING. */
    if (current->phys == saved.phys &&
        current->owner == DMA_OWNER_RELEASING) {
        if (rc == 0) *current = (dma_allocation_t){0};
        else *current = saved;
    } else if (rc == 0) {
        /* Registry corruption must not turn a successful physical release
           into a reusable live entry. Keep the slot unavailable for diagnosis. */
        rc = -3;
    }
    dma_unlock();
    return rc;
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

static void virtio_pci_quiesce(virtio_dev_t* v) {
    if (!v || !v->pci) return;
    uint8_t id;
    uint16_t cap = 0;
    for (unsigned n = 0; n < 48 && (cap = pci_next_cap(v->pci, cap, &id)) != 0; n++) {
        if (id == 0x05) {                                      /* MSI */
            uint16_t ctl = pci_read16(v->pci, (uint16_t)(cap + 2));
            if (ctl & 1u) pci_write16(v->pci, (uint16_t)(cap + 2), (uint16_t)(ctl & ~1u));
        } else if (id == 0x11) {                               /* MSI-X: mask function, then disable */
            uint16_t ctl = pci_read16(v->pci, (uint16_t)(cap + 2));
            ctl = (uint16_t)((ctl | 0x4000u) & ~0x8000u);
            pci_write16(v->pci, (uint16_t)(cap + 2), ctl);
        }
    }
    uint16_t command = pci_read16(v->pci, 0x04);
    if (command & PCI_CMD_MASTER)
        pci_write16(v->pci, 0x04, (uint16_t)(command & ~PCI_CMD_MASTER));
}

static int reset(virtio_dev_t* v) {
    if (!v || !v->common) return -1;
    status_set(v, 0);
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if (status_get(v) == 0) { barrier(); return 0; }
        CPU_RELAX();
    }
    return -1;
}

int virtio_reset_device(virtio_dev_t* v) {
    if (!v || !v->common) return -1;
    virtio_pci_quiesce(v);
    return reset(v);
}

void virtio_fail(virtio_dev_t* v) {
    if (!v || !v->common) return;
    virtio_pci_quiesce(v);
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

int virtio_queue_setup(virtio_dev_t* v, virtq_t* q, uint16_t index, uint16_t max_size) {
    if (!v || !q || !v->common || !max_size ||
        q->mem || q->mem_phys || q->mem_bytes) return -1;
    *q = (virtq_t){0};
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
    q->mem_bytes = q->used_off + 6 + 8 * (uint32_t)n;
    q->mem = (uint8_t*)virtio_dma_alloc(q->mem_bytes, &q->mem_phys);
    if (!q->mem) { q->mem_bytes = 0; q->mem_phys = 0; q->dev = 0; return -2; }
    if (((uintptr_t)q->mem & 4095u) || (q->mem_phys & 4095u) ||
        q->mem_phys > UINT64_MAX - q->mem_bytes) {
        (void)virtio_dma_free(q->mem, q->mem_phys, q->mem_bytes);
        *q = (virtq_t){0};
        return -3;
    }
    MMIO_W16(v->common + R_QSIZE, n);
    MMIO_W16(v->common + R_QMSIX, v->msix_vec);                  /* запись MSI-X очереди или 0xFFFF - без прерываний */
    q->iw = 0;
    if (v->msix_vec != 0xFFFF && MMIO_R16(v->common + R_QMSIX) != v->msix_vec) MMIO_W16(v->common + R_QMSIX, 0xFFFF);
    MMIO_W32(v->common + R_QDESC, (uint32_t)q->mem_phys);              MMIO_W32(v->common + R_QDESC + 4, (uint32_t)(q->mem_phys >> 32));
    MMIO_W32(v->common + R_QAVAIL, (uint32_t)(q->mem_phys + q->avail_off)); MMIO_W32(v->common + R_QAVAIL + 4, (uint32_t)((q->mem_phys + q->avail_off) >> 32));
    MMIO_W32(v->common + R_QUSED, (uint32_t)(q->mem_phys + q->used_off));   MMIO_W32(v->common + R_QUSED + 4, (uint32_t)((q->mem_phys + q->used_off) >> 32));
    if (virtio_dma_sync_for_device(q->mem, q->mem_phys, q->mem_bytes) != 0) {
        (void)virtio_dma_free(q->mem, q->mem_phys, q->mem_bytes);
        *q = (virtq_t){0};
        return -4;
    }
    uint16_t noff = MMIO_R16(v->common + R_QNOFF);
    MMIO_W16(v->common + R_QEN, 1);
    q->notify_addr = v->notify + (uint64_t)noff * v->notify_mult;
    return 0;
}

int virtio_queue_release(virtq_t* q) {
    if (!q || !q->mem || !q->mem_phys || !q->mem_bytes || !q->dev)
        return -1;
    /* Queue memory is device-writable. Never return it to PMM while the
       device may still own the queue or have outstanding DMA transactions. */
    if (status_get(q->dev) != 0) return -2;
    int rc = virtio_dma_free(q->mem, q->mem_phys, q->mem_bytes);
    if (rc != 0) return rc;
    *q = (virtq_t){0};
    return 0;
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
void virtq_kick(virtq_t* q) {
    if (!q || !q->mem ||
        virtio_dma_sync_for_device(q->mem, q->mem_phys, q->mem_bytes) != 0)
        return;
    MMIO_W16(q->notify_addr, q->index);
}
int virtq_take(virtq_t* q, uint16_t* id, uint32_t* len) {
    if (!q || !q->mem || !id || !len) return 0;
    if (virtio_dma_sync_for_cpu(q->mem, q->mem_phys, q->mem_bytes) != 0) return 0;
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
    if (virtio_dma_sync_for_device(q->mem, q->mem_phys, q->mem_bytes) != 0) return -3;
    barrier();
    IW_MARK();
    MMIO_W16(q->notify_addr, q->index);
    uint64_t start = WAIT_NOW(), t0 = WAIT_T0();
    for (uint32_t i = 0; ; i++) {
        if (virtio_dma_sync_for_cpu(q->mem, q->mem_phys, q->mem_bytes) != 0) return -4;
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
