#ifndef KIRA_PCI_H
#define KIRA_PCI_H
#include <stdint.h>

/* PCI: перечисление устройств, BAR, список capabilities. Конфигурационное пространство читается
   через порты 0xCF8/0xCFC (есть на любом x86-ПК; первые 256 байт - всё, что нужно virtio).
   Чтение/запись конфигурации поддерживает многопоточный доступ. */

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

void pci_set_backend(const pci_backend_t* b);      /* 0 - порты 0xCF8/0xCFC (только x86) */
void pci_use_ecam(volatile void* base, uint8_t bus_start, uint8_t bus_end);
/* Validate the ECAM aperture against its advertised bus range. */
int pci_ecam_bus_range(uint64_t ecam_size, int has_bus_range, uint32_t bus_start,
                       uint32_t bus_end, uint8_t* first, uint8_t* last);   /* конфигурация через ECAM */
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
