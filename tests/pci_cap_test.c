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
    put8(0x34, 0x40);
    put8(0x40, 0x05);
    put8(0x41, 0x44);
    put8(0x44, 0x11);
    put8(0x45, 0);
    if (pci_next_cap(&dev, 0, &id) != 0x40 || id != 0x05) return 4;
    if (pci_next_cap(&dev, 0x40, &id) != 0x44 || id != 0x11) return 5;

    put8(0x45, 0x44);                         /* direct self-loop */
    if (pci_next_cap(&dev, 0x44, &id) || id != 0x11) return 6;
    if (pci_next_cap(&dev, 0x41, &id)) return 7; /* unaligned previous offset */

    put8(0x45, 0xFC); put8(0xFC, 0xFF);      /* terminal slot with reserved ID */
    if (pci_next_cap(&dev, 0x44, &id)) return 8;
    put8(0x45, 0x48); put8(0x48, 0);         /* zero capability ID */
    if (pci_next_cap(&dev, 0x44, &id)) return 9;
    put8(0x34, 0x3C); put8(0x3C, 0x05);      /* pointer below the capability area */
    if (pci_next_cap(&dev, 0, &id)) return 10;
    put16(0x06, 0);                           /* capability-list status bit clear */
    if (pci_next_cap(&dev, 0, &id)) return 11;
    puts("pci_cap_test: PASS");
    return 0;
}
