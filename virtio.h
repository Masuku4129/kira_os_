#ifndef KIRA_VIRTIO_H
#define KIRA_VIRTIO_H
#include <stdint.h>
#include "cpu.h"
#include <stddef.h>
#include "pci.h"

/* virtio 1.0 over PCI (modern capabilities + MMIO BAR).
   DMA allocations expose a CPU pointer separately from the device-visible address.
   The generic fallback currently requires low-memory identity mapping; it is not
   an IOMMU implementation. Platform DMA pools may provide a different address. */

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_F_VERSION_1 (1ULL << 32)

enum { VIRTIO_ST_ACK = 1, VIRTIO_ST_DRIVER = 2, VIRTIO_ST_DRIVER_OK = 4, VIRTIO_ST_FEATURES_OK = 8,
       VIRTIO_ST_NEEDS_RESET = 0x40, VIRTIO_ST_FAILED = 0x80 };

typedef struct {
    const pci_dev_t* pci;
    volatile uint8_t* common;
    volatile uint8_t* notify;
    uint32_t notify_mult;
    volatile uint8_t* isr;
    volatile uint8_t* devcfg;
    uint64_t features;               /* согласованные */
    uint16_t msix_vec;               /* запись MSI-X для очередей (0xFFFF - без прерываний, опрос) */
} virtio_dev_t;

typedef struct {
    virtio_dev_t* dev;
    uint16_t index, size, last_used, avail_idx;
    volatile uint8_t* notify_addr;
    uint8_t* mem;                    /* desc | avail | used */
    uint64_t mem_phys;                /* device-visible DMA address; not necessarily equal to mem */
    uint32_t avail_off, used_off;
    struct irqwait* iw;              /* ожидание сном до прерывания очереди (0 - опрос) */
} virtq_t;

typedef struct { uint64_t addr; uint32_t len; uint8_t device_writes; } virtq_buf_t;

/* 0 - готово; <0 - у устройства нет нужных capabilities / нет отображения BAR */
int  virtio_attach(virtio_dev_t* v, const pci_dev_t* pci);
/* Сброс, ACK, DRIVER, согласование признаков (нужен VERSION_1; из want берутся только предложенные), FEATURES_OK. */
int  virtio_begin(virtio_dev_t* v, uint64_t want);
int  virtio_queue_setup(virtio_dev_t* v, virtq_t* q, uint16_t index, uint16_t max_size);
void virtio_driver_ok(virtio_dev_t* v);
void virtio_fail(virtio_dev_t* v);                      /* FAILED + сброс: устройство больше не пишет в нашу память */
/* Отправляет цепочку буферов (до 8) и ждёт завершения. 0 - готово, -1 - таймаут. */
int  virtq_run(virtq_t* q, const virtq_buf_t* bufs, int n);

/* Без ожидания (очереди событий): положить буфер под дескриптор id, сообщить устройству, забрать готовый. */
void virtq_post(virtq_t* q, uint16_t id, uint64_t addr, uint32_t len, int device_writes);
void virtq_kick(virtq_t* q);
int  virtq_take(virtq_t* q, uint16_t* id, uint32_t* len);   /* 1 - взят */

/* DMA buffer (zeroed). CPU pointer and device-visible *phys are distinct values.
   Generic fallback requires identity-mapped contiguous RAM; 0 means failure. */
void* virtio_dma_alloc(uint32_t bytes, uint64_t* phys);
/* Frees only an allocation returned by virtio_dma_alloc with the exact size.
   -2: platform DMA pool has no release hook; buffer remains owned by that pool. */
int   virtio_dma_free(void* cpu, uint64_t phys, uint32_t bytes);

/* Блочное устройство virtio-blk. NULL - такого устройства нет или оно не поднялось. */
struct blk_dev;
struct blk_dev* virtio_blk_probe(void);

#ifdef KIRA_HOST_TEST
uint64_t host_mmio_read(const volatile void* a, int size);
void host_mmio_write(volatile void* a, int size, uint64_t v);
void host_relax(void);
#define MMIO_R8(a)  ((uint8_t)host_mmio_read((a), 1))
#define MMIO_R16(a) ((uint16_t)host_mmio_read((a), 2))
#define MMIO_R32(a) ((uint32_t)host_mmio_read((a), 4))
#define MMIO_W8(a, v)  host_mmio_write((a), 1, (v))
#define MMIO_W16(a, v) host_mmio_write((a), 2, (v))
#define MMIO_W32(a, v) host_mmio_write((a), 4, (v))
#define CPU_RELAX() host_relax()
#else
#define MMIO_R8(a)  (*(volatile uint8_t*)(a))
#define MMIO_R16(a) (*(volatile uint16_t*)(a))
#define MMIO_R32(a) (*(volatile uint32_t*)(a))
#define MMIO_W8(a, v)  (*(volatile uint8_t*)(a) = (uint8_t)(v))
#define MMIO_W16(a, v) (*(volatile uint16_t*)(a) = (uint16_t)(v))
#define MMIO_W32(a, v) (*(volatile uint32_t*)(a) = (uint32_t)(v))
#define CPU_RELAX() cpu_relax()
#endif

#endif
