#ifndef KIRA_PCI_H
#define KIRA_PCI_H
#include <stdint.h>

/* PCI: перечисление устройств, BAR и capabilities.
   x86 legacy: CF8/CFC; PCIe: ECAM (4 KiB на функцию, 1 MiB на шину).
   ECAM address = base + ((bus - first_bus) << 20) + (device << 15)
                       + (function << 12) + register_offset.
   The ECAM aperture must cover every bus in [first_bus, last_bus].
   Configuration RMW operations are serialized by pci.c. */

#define PCI_MAX_DEVS 1024         /* функций на всех шинах: у ПК 40-80, у серверов - сотни */
#define PCI_BARS 6

typedef struct {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision, header_type;
    struct { uint64_t base; uint64_t size; uint8_t io, mem64, prefetch; } bar[PCI_BARS];
} pci_dev_t;

typedef struct {                 /* способ доступа к конфигурации; в тестах подменяется моделью шины */
    uint32_t (*read32)(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
    void (*write32)(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t val);
} pci_backend_t;

void pci_set_backend(const pci_backend_t* b);      /* NULL - legacy CF8/CFC backend (x86 only) */
typedef struct {
    volatile void* base;          /* mapped uncached/device memory; virtual base must be 4 KiB aligned */
    uint64_t size;                 /* mapped aperture bytes */
    uint8_t first_bus;
    uint8_t last_bus;
} pci_ecam_config_t;
/* Validate DT bus-range and aperture size (one MiB per bus). */
int pci_ecam_bus_range(uint64_t ecam_size, int has_bus_range, uint32_t bus_start,
                       uint32_t bus_end, uint8_t* first, uint8_t* last);
/* Install ECAM only after validating base, bus order and aperture size. Returns 0 on success. */
int pci_configure_ecam(const pci_ecam_config_t* config);
/* Compatibility wrapper for callers that already validated the aperture. */
void pci_use_ecam(volatile void* base, uint8_t bus_start, uint8_t bus_end);
typedef struct { uint64_t pci_base, size, next; int64_t cpu_off; } pci_window_t;   /* окно адресов моста */
int  pci_assign(pci_window_t* mem32, pci_window_t* mem64);   /* назначить пустые BAR (после pci_init); число BAR */
int  pci_init(void);                               /* число найденных устройств */
int  pci_count(void);
const pci_dev_t* pci_get(int i);
const pci_dev_t* pci_find(uint16_t vendor, uint16_t device);

uint8_t  pci_read8(const pci_dev_t* d, uint16_t off);
uint16_t pci_read16(const pci_dev_t* d, uint16_t off);
uint32_t pci_read32(const pci_dev_t* d, uint16_t off);
void     pci_write16(const pci_dev_t* d, uint16_t off, uint16_t v);
void     pci_write32(const pci_dev_t* d, uint16_t off, uint32_t v);

#define PCI_CMD_IO     0x1
#define PCI_CMD_MEM    0x2
#define PCI_CMD_MASTER 0x4
#define PCI_CMD_INTX_OFF 0x400   /* без прерываний INTx: драйверы работают опросом */
void pci_enable(const pci_dev_t* d, uint16_t cmd_bits);    /* bus mastering разрешается только после готовности DMA/IOMMU */
void pci_disable_bus_master(const pci_dev_t* d);            /* атомарно запрещает DMA от PCI-функции */

/* Список capabilities: первый вызов с prev = 0, дальше - с предыдущим смещением. 0 - конец. */
uint16_t pci_next_cap(const pci_dev_t* d, uint16_t prev, uint8_t* id);

/* Адрес области BAR для обращений процессора (0 - BAR портов, не назначен или выход за размер). Только в ядре. */
volatile void* pci_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len);

void pci_print(void);                                   /* список устройств в консоль (только в ядре) */

#endif
