#ifndef KIRA_PCI_H
#define KIRA_PCI_H
#include <stdint.h>

/* PCI: перечисление устройств, BAR, список capabilities. Конфигурационное пространство читается
   через порты 0xCF8/0xCFC (есть на любом x86-ПК; первые 256 байт - всё, что нужно virtio).
   Все функции вызываются из одного потока при старте. */

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
int pci_use_ecam(volatile void* base, uint64_t size, uint8_t bus_start, uint8_t bus_end); /* ECAM; отказ, если отображения не хватает на все шины */
typedef struct { uint64_t pci_base, size, next; int64_t cpu_off; } pci_window_t;   /* окно адресов моста */
#define PCI_MAX_WINDOWS 8
typedef struct { pci_window_t windows[PCI_MAX_WINDOWS]; unsigned count; } pci_window_set_t;
int  pci_window_set_add(pci_window_set_t* set, const pci_window_t* window); /* reject overlap/capacity/overflow */
int  pci_window_set_alloc(pci_window_set_t* set, uint64_t size, uint64_t* pci_addr, uint64_t* cpu_addr);
int  pci_window_alloc(pci_window_t* w, uint64_t size, uint64_t* pci_addr, uint64_t* cpu_addr); /* bounded, overflow-safe BAR window allocation */
int  pci_assign_multi(pci_window_set_t* mem32, pci_window_set_t* mem64);
int  pci_assign(pci_window_t* mem32, pci_window_t* mem64); /* compatibility wrapper */   /* назначить пустые BAR (после pci_init); число BAR */
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
void pci_enable(const pci_dev_t* d, uint16_t cmd_bits);    /* включает decode памяти/портов и bus mastering; гасит INTx-маску не трогая */

/* Список capabilities: первый вызов с prev = 0, дальше - с предыдущим смещением. 0 - конец. */
uint16_t pci_next_cap(const pci_dev_t* d, uint16_t prev, uint8_t* id);
int pci_quiesce_interrupts(const pci_dev_t* d); /* mask and verify MSI, MSI-X and legacy INTx */

/* Адрес области BAR для обращений процессора (0 - BAR портов, не назначен или выход за размер). Только в ядре. */
volatile void* pci_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len);

void pci_print(void);                                   /* список устройств в консоль (только в ядре) */

#endif
