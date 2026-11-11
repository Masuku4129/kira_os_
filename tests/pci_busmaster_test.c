#include "pci.h"
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__) || defined(__x86_64__)
static int test_iommu_ready;
int iommu_device_ready(const pci_dev_t* dev)
{
    (void)dev;
    return test_iommu_ready;
}
#endif

static uint32_t config[2][64];
static uint8_t ecam_window[1u << 20] __attribute__((aligned(1u << 20)));

static uint32_t read_cfg(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    if (bus != 0 || (dev != 1 && dev != 2) || fn != 0 || off >= 256u)
        return 0xffffffffu;
    return config[dev - 1u][off >> 2];
}

static void write_cfg(uint8_t bus, uint8_t dev, uint8_t fn,
                      uint16_t off, uint32_t value)
{
    if (bus == 0 && (dev == 1 || dev == 2) && fn == 0 && off < 256u)
        config[dev - 1u][off >> 2] = value;
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
    config[0][0] = (0x1001u << 16) | 0x1af4u;
    config[0][1] = 0x0007u; /* Endpoint firmware left bus mastering enabled. */
    config[0][2] = 0x01000001u;
    config[1][0] = (0x1234u << 16) | 0x8086u;
    config[1][1] = 0x0003u; /* Bridge starts with forwarding disabled. */
    config[1][2] = 0x06040001u;
    config[1][3] = 0x00010000u; /* Type-1 header. */
    config[1][6] = 0x00050500u; /* Secondary/subordinate bus 5. */
    pci_set_backend(&backend);

    CHECK(pci_init() == 2);
    dev = pci_get(0);
    const pci_dev_t* bridge = pci_get(1);
    CHECK(dev != 0 && dev->vendor == 0x1af4u && dev->device == 0x1001u);
    CHECK(bridge != 0 && bridge->class_code == 0x06 && bridge->subclass == 0x04);
    CHECK(pci_is_bridge(bridge));
    CHECK(!pci_is_bridge(dev));
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
    CHECK((pci_read16(bridge, 0x04) & PCI_CMD_MASTER) != 0);
    CHECK((pci_read16(dev, 0x04) & (PCI_CMD_IO | PCI_CMD_MEM)) ==
          (PCI_CMD_IO | PCI_CMD_MEM));
#if defined(__aarch64__) || defined(__x86_64__)
    test_iommu_ready = 1; /* Legacy fixture represents an already isolated requester. */
#endif
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
    pci_ecam_config_t invalid_ecam = ecam;
    invalid_ecam.size = (1u << 20) - 1u;
    CHECK(pci_configure_ecam(&invalid_ecam) != 0);
    invalid_ecam = ecam;
    invalid_ecam.base = 0;
    CHECK(pci_configure_ecam(&invalid_ecam) != 0);
    invalid_ecam = ecam;
    invalid_ecam.base = ecam_window + 1u;
    CHECK(pci_configure_ecam(&invalid_ecam) != 0);
    CHECK(pci_init() == 1);
    dev = pci_get(0);
    CHECK(dev != 0 && dev->bus == 5 && dev->dev == 1);
    CHECK(dev->vendor == 0x1234u && dev->device == 0x2000u);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
#if defined(__aarch64__) || defined(__x86_64__)
    /* The ARM64 PCI backend must not enable bus mastering for a bypass/unready
       stream, but must allow it after the IOMMU reports a private domain. */
    test_iommu_ready = 0;
    pci_enable(dev, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
    pci_write16(dev, 0x04, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
    pci_write32(dev, 0x04, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) == 0);
    test_iommu_ready = 1;
    pci_enable(dev, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) != 0);
    pci_disable_bus_master(dev);
    pci_write32(dev, 0x04, PCI_CMD_MASTER);
    CHECK((pci_read16(dev, 0x04) & PCI_CMD_MASTER) != 0);
#endif

    ecam.first_bus = 6;
    ecam.last_bus = 5;
    CHECK(pci_configure_ecam(&ecam) != 0);
    puts("PCI: legacy backend and validated ECAM enumeration/bus-master gating PASS");
    return 0;
}
