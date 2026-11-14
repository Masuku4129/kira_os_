#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

_Alignas(4096) static uint8_t ecam[2u * 1024u * 1024u];

static void put32(uint8_t bus, uint8_t dev, uint16_t off, uint32_t value) {
    uint64_t pos = ((uint64_t)(bus - 0x20) << 20) |
                   ((uint64_t)dev << 15) | (off & 0xFFCu);
    *(uint32_t*)(void*)(ecam + pos) = value;
}

static void device(uint8_t bus, uint8_t dev, uint16_t vendor, uint16_t id,
                   uint32_t class_reg, uint8_t header_type) {
    put32(bus, dev, 0x00, ((uint32_t)id << 16) | vendor);
    put32(bus, dev, 0x08, class_reg);
    put32(bus, dev, 0x0C, (uint32_t)header_type << 16);
}

int main(void) {
    memset(ecam, 0xFF, sizeof ecam);

    /* The MCFG segment starts at bus 0x20, not bus 0. Its root function
       is a PCI-to-PCI bridge whose secondary bus is 0x21. */
    device(0x20, 0, 0x1234, 0x0001, 0x06040000u, 1);
    put32(0x20, 0, 0x18, 0x00002100u); /* primary=0, secondary=0x21 */
    device(0x21, 0, 0x1234, 0x1000, 0x02000000u, 0);

    pci_use_ecam(ecam, 0x20, 0x21);
    int count = pci_init();
    if (count != 2) {
        printf("FAIL: expected root bridge + downstream NIC, got %d\n", count);
        return 1;
    }
    const pci_dev_t *root = pci_get(0);
    const pci_dev_t *nic = pci_find(0x1234, 0x1000);
    if (!root || root->bus != 0x20 || root->dev != 0 ||
        !nic || nic->bus != 0x21 || nic->class_code != 0x02) {
        puts("FAIL: ECAM root/subordinate bus identity was not preserved");
        return 1;
    }
    puts("PCI ECAM nonzero root bus + bridge traversal: PASS");
    return 0;
}
