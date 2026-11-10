#include "pci.h"
#include <stdio.h>
#include <string.h>

static uint32_t config[64];
static uint8_t ecam_window[1u << 20] __attribute__((aligned(1u << 20)));

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

    /* A one-bus ECAM aperture starts at bus 5. Device 1/function 0 lives at
       offset 0x8000; enumeration must use bus-relative, not absolute bus, math. */
    memset(ecam_window, 0xff, sizeof(ecam_window));
    *(uint32_t*)(ecam_window + 0x8000) = (0x2000u << 16) | 0x1234u;
    *(uint32_t*)(ecam_window + 0x8004) = 0x0007u;
    pci_ecam_config_t ecam = {
        .base = ecam_window, .size = sizeof(ecam_window),
        .first_bus = 5, .last_bus = 5
    };
    uint8_t first = 0, last = 0;
    CHECK(pci_ecam_bus_range(sizeof(ecam_window), 1, 5, 5,
                             &first, &last) == 0);
    CHECK(first == 5 && last == 5);
    CHECK(pci_ecam_bus_range(sizeof(ecam_window), 1, 5, 6,
                             &first, &last) != 0);
    CHECK(pci_configure_ecam(&ecam) == 0);
    CHECK(pci_init() == 1);
    dev = pci_get(0);
    CHECK(dev != 0 && dev->bus == 5 && dev->dev == 1);
    CHECK(dev->vendor == 0x1234u && dev->device == 0x2000u);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);

    ecam.first_bus = 6;
    ecam.last_bus = 5;
    CHECK(pci_configure_ecam(&ecam) != 0);
    puts("PCI: legacy backend and validated ECAM enumeration/bus-master gating PASS");
    return 0;
}
