#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

static uint32_t cfg[5][64];

static uint32_t model_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (bus || dev >= 5 || fn || off >= sizeof cfg[0]) return 0xFFFFFFFFu;
    return cfg[dev][off / 4u];
}

static void model_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    if (bus || dev >= 5 || fn || off >= sizeof cfg[0]) return;
    if (off >= 0x10 && off < 0x28) return; /* BARs are absent in this synthetic device model. */
    if (off == 4) {
        /* PCI status is write-one-to-clear; the driver's pci_write16 masks it to zero. */
        cfg[dev][1] = value & 0xFFFFu;
    } else {
        cfg[dev][off / 4u] = value;
    }
}

static void set_device(unsigned dev, uint32_t id, uint32_t class_rev, uint32_t header) {
    cfg[dev][0] = id;
    cfg[dev][1] = PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;
    cfg[dev][2] = class_rev;
    cfg[dev][3] = header;
}

int main(void) {
    pci_backend_t backend = { model_read, model_write };
    memset(cfg, 0, sizeof cfg);

    set_device(0, 0x11118086u, 0x06000001u, 0x00000000u); /* host bridge: sanitize */
    set_device(1, 0x22228086u, 0x06040001u, 0x00010000u); /* PCI-to-PCI bridge: preserve forwarding */
    cfg[1][6] = 0x00010100u;                              /* primary 0, secondary 1, subordinate 1 */
    set_device(2, 0x33338086u, 0x06010001u, 0x00000000u); /* ISA bridge: sanitize */
    set_device(3, 0x44448086u, 0x06070001u, 0x00020000u); /* CardBus bridge: preserve forwarding */
    set_device(4, 0x12348086u, 0x02000001u, 0x00000000u); /* endpoint: sanitize */
    pci_set_backend(&backend);

    assert(pci_init() == 5);
    const pci_dev_t *host = pci_get(0);
    const pci_dev_t *bridge = pci_get(1);
    const pci_dev_t *isa = pci_get(2);
    const pci_dev_t *cardbus = pci_get(3);
    const pci_dev_t *dev = pci_get(4);
    assert(host && bridge && isa && cardbus && dev);

    assert((pci_read16(host, 0x04) & PCI_CMD_MASTER) == 0);
    assert((pci_read16(bridge, 0x04) & PCI_CMD_MASTER) != 0);
    assert((pci_read16(isa, 0x04) & PCI_CMD_MASTER) == 0);
    assert((pci_read16(cardbus, 0x04) & PCI_CMD_MASTER) != 0);

    uint16_t command = pci_read16(dev, 0x04);
    assert((command & PCI_CMD_MASTER) == 0);
    assert((command & (PCI_CMD_IO | PCI_CMD_MEM)) == (PCI_CMD_IO | PCI_CMD_MEM));

    /* Bus mastering remains available only when the owning endpoint driver explicitly enables it. */
    pci_enable(dev, PCI_CMD_MASTER);
    assert((pci_read16(dev, 0x04) & PCI_CMD_MASTER) != 0);

    puts("pci enumeration: firmware bus mastering cleared on host/ISA/endpoints; bridge forwarding preserved");
    return 0;
}
