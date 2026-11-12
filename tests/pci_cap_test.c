#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

static uint32_t cfg[64];
static uint16_t blocked_write_off = 0xFFFFu;
static uint32_t rd(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    (void)bus; (void)dev; (void)fn;
    return off < 256 ? cfg[off >> 2] : 0xFFFFFFFFu;
}
static void wr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    (void)bus; (void)dev; (void)fn;
    if (off < 256 && off != blocked_write_off) cfg[off >> 2] = value;
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
    put16(0x06, 0x10);
    put8(0x34, 0x40); put8(0x40, 0x05); put8(0x41, 0x44);
    put8(0x44, 0x11); put8(0x45, 0);
    put16(0x42, 1); put16(0x46, 0x8000); put16(0x04, 0);
    if (!pci_quiesce_interrupts(&dev)) return 15;
    if ((pci_read16(&dev, 0x42) & 1u) || pci_read16(&dev, 0x46) != 0x4000u ||
        !(pci_read16(&dev, 0x04) & (1u << 10))) return 16;
    put16(0x42, 1); put16(0x46, 0x8000); put16(0x04, 0);
    blocked_write_off = 0x40;                    /* MSI enable refuses to clear */
    if (pci_quiesce_interrupts(&dev)) return 17;
    blocked_write_off = 0xFFFFu;
    put8(0x34, 0x3C);                             /* malformed list must fail closed */
    if (pci_quiesce_interrupts(&dev)) return 18;
    put8(0x34, 0x40); put8(0x41, 0x40);           /* cyclic list must fail closed */
    if (pci_quiesce_interrupts(&dev)) return 19;
    put8(0x41, 0x44); put8(0x45, 0); put16(0x06, 0);
    {
        pci_backend_t incomplete = { rd, 0 };
        pci_set_backend(&incomplete);
        if (pci_read32(&dev, 0) != 0xFFFFFFFFu) return 12;
        pci_set_backend(&backend);
        if (pci_use_ecam((volatile void*)(uintptr_t)1, 1u << 20, 0, 0) == 0) return 13;
        if (pci_use_ecam((volatile void*)(uintptr_t)0x1000, (1u << 20) - 1, 0, 0) == 0) return 14;
    }
    puts("pci_cap_test: PASS");
    return 0;
}
