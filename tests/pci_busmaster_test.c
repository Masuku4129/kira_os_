#include "pci.h"
#include <stdio.h>
#include <string.h>

static uint32_t config[64];

static uint32_t read_cfg(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    if (bus != 0 || dev != 1 || fn != 0 || off >= 256u)
        return 0xffffffffu;
    return config[off >> 2];
}

static void write_cfg(uint8_t bus, uint8_t dev, uint8_t fn,
                      uint16_t off, uint32_t value)
{
    if (bus == 0 && dev == 1 && fn == 0 && off < 256u)
        config[off >> 2] = value;
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "pci bus-master test failed at line %d: %s\\n", __LINE__, #x); \
    return 1; \
} } while (0)

int main(void)
{
    pci_backend_t backend = { read_cfg, write_cfg };
    const pci_dev_t* dev;

    memset(config, 0, sizeof(config));
    config[0] = (0x1001u << 16) | 0x1af4u;
    config[1] = 0x0007u; /* I/O, memory and bus mastering were left enabled. */
    config[2] = 0x01000001u;
    pci_set_backend(&backend);

    CHECK(pci_init() == 1);
    dev = pci_get(0);
    CHECK(dev != 0 && dev->vendor == 0x1af4u && dev->device == 0x1001u);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
    CHECK((pci_read16(dev, 0x04) & (PCI_CMD_IO | PCI_CMD_MEM)) ==
          (PCI_CMD_IO | PCI_CMD_MEM));

    pci_enable(dev, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) != 0);
    pci_set_backend(0);
    puts("PCI: bus mastering disabled during enumeration and explicitly re-enabled by driver");
    return 0;
}
