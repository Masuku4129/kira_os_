#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

static uint32_t cfg[64];

static uint32_t model_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (bus || dev || fn || off >= sizeof cfg) return 0xFFFFFFFFu;
    return cfg[off / 4u];
}

static void model_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    if (bus || dev || fn || off >= sizeof cfg) return;
    if (off == 4) {
        /* PCI status is write-one-to-clear; the driver's pci_write16 masks it to zero. */
        cfg[1] = value & 0xFFFFu;
    } else {
        cfg[off / 4u] = value;
    }
}

int main(void) {
    pci_backend_t backend = { model_read, model_write };
    memset(cfg, 0, sizeof cfg);
    cfg[0] = 0x12348086u;              /* vendor 8086, device 1234 */
    cfg[1] = PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;
    cfg[2] = 0x02000001u;              /* network controller, revision 1 */
    cfg[3] = 0;                        /* normal header type */
    pci_set_backend(&backend);

    assert(pci_init() == 1);
    const pci_dev_t *dev = pci_get(0);
    assert(dev != 0);
    uint16_t command = pci_read16(dev, 0x04);
    assert((command & PCI_CMD_MASTER) == 0);
    assert((command & (PCI_CMD_IO | PCI_CMD_MEM)) == (PCI_CMD_IO | PCI_CMD_MEM));

    /* Bus mastering remains available only when the owning driver explicitly enables it. */
    pci_enable(dev, PCI_CMD_MASTER);
    assert((pci_read16(dev, 0x04) & PCI_CMD_MASTER) != 0);

    puts("pci enumeration: firmware bus mastering cleared before driver ownership");
    return 0;
}
