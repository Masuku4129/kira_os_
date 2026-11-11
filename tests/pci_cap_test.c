#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

static uint32_t cfg[64];
static uint32_t rd(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    (void)bus; (void)dev; (void)fn;
    return off < 256 ? cfg[off >> 2] : 0xFFFFFFFFu;
}
static void wr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    (void)bus; (void)dev; (void)fn;
    if (off < 256) cfg[off >> 2] = value;
}
static void put8(uint16_t off, uint8_t v) {
    unsigned sh = (off & 3u) * 8u;
    cfg[off >> 2] = (cfg[off >> 2] & ~(0xFFu << sh)) | ((uint32_t)v << sh);
}
static void put16(uint16_t off, uint16_t v) {
    unsigned sh = (off & 2u) * 8u;
    cfg[off >> 2] = (cfg[off >> 2] & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
}
int main(void) {
    pci_backend_t backend = { rd, wr };
    pci_dev_t dev;
    uint8_t id = 0xA5;
    memset(&dev, 0, sizeof dev);
    memset(cfg, 0, sizeof cfg);
    pci_set_backend(&backend);
    if (pci_next_cap(0, 0, &id) || id != 0xA5) return 1;
    if (pci_next_cap(&dev, 0, 0)) return 2;
    if (pci_next_cap(&dev, 0, &id) || id != 0xA5) return 3;
    put16(0x06, 0x10);
    put8(0x34, 0x20);
    put8(0x20, 0x05);
    put8(0x21, 0x40);
    put8(0x40, 0x11);
    put8(0x41, 0);
    if (pci_next_cap(&dev, 0, &id) != 0x40 || id != 0x11) return 4;
    put8(0x21, 0x40);
    if (pci_next_cap(&dev, 0x40, &id)) return 5;
    if (pci_next_cap(&dev, 0x41, &id)) return 6;
    put8(0x21, 0xFC);
    put8(0xFC, 0xFF);
    if (pci_next_cap(&dev, 0x40, &id)) return 7;
    put8(0x21, 0x44);
    put8(0x44, 0);
    if (pci_next_cap(&dev, 0x40, &id)) return 8;
    put8(0x21, 0);
    if (pci_next_cap(&dev, 0x40, &id)) return 9;
    put16(0x06, 0);
    if (pci_next_cap(&dev, 0, &id)) return 10;
    puts("pci_cap_test: PASS");
    return 0;
}
