#include "iommu_vtd_hw.h"
#include <stddef.h>

#define VTD_REG_VER   0x00
#define VTD_REG_CAP   0x08
#define VTD_REG_ECAP  0x10
#define VTD_REG_GSTS  0x1C
#define VTD_REG_PAGE  4096

static uint64_t mmio_read64(const volatile uint8_t *p, uint32_t off) {
    const volatile uint32_t *r = (const volatile uint32_t *)(p + off);
    uint32_t lo = r[0];
    uint32_t hi = r[1];
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
static uint32_t mmio_read32(const volatile uint8_t *p, uint32_t off) {
    return *(const volatile uint32_t *)(p + off);
}

int vtd_hw_probe(const volatile void *registers, vtd_hw_caps_t *out) {
    if (!registers || !out) return -1;
    const volatile uint8_t *p = (const volatile uint8_t *)registers;
    uint32_t ver = mmio_read32(p, VTD_REG_VER);
    uint64_t cap = mmio_read64(p, VTD_REG_CAP);
    uint64_t ecap = mmio_read64(p, VTD_REG_ECAP);
    uint32_t gsts = mmio_read32(p, VTD_REG_GSTS);
    (void)gsts; /* observed only; never infer translation is active from capabilities */

    /* A zero version and all-zero capability block is not a functioning unit.
       Version register: major in bits 7:4, minor in bits 3:0. */
    if ((ver & 0xFFu) == 0 || cap == 0 || ecap == 0) return -2;
    uint8_t major = (uint8_t)((ver >> 4) & 0xF);
    if (major == 0 || major > 3) return -3;

    vtd_hw_caps_t c;
    c.version = ver;
    c.nd = (uint8_t)(cap & 7u);
    c.gaw = (uint8_t)((cap >> 16) & 0x3Fu);
    c.sagaw_mask = (uint8_t)((cap >> 8) & 0x1Fu);
    c.supports_write_buffer_flush = (uint8_t)((cap >> 4) & 1u);
    c.supports_read_drain = (uint8_t)((cap >> 55) & 1u);
    c.supports_write_drain = (uint8_t)((cap >> 54) & 1u);
    c.supports_page_selective_invalidation = (uint8_t)((cap >> 39) & 1u);
    c.supports_queued_invalidation = (uint8_t)((ecap >> 1) & 1u);
    c.supports_device_tlb = (uint8_t)((ecap >> 2) & 1u);
    c.supports_interrupt_remapping = (uint8_t)((ecap >> 3) & 1u);
    c.supports_pass_through = (uint8_t)((ecap >> 6) & 1u);
    c.supports_scalable_mode = (uint8_t)((ecap >> 24) & 1u);
    c.supports_first_level = (uint8_t)((ecap >> 28) & 1u);
    c.supports_posted_interrupts = (uint8_t)((ecap >> 59) & 1u);

    /* Only 39/48/57-bit guest address widths are currently understood by
       this kernel's page-table builders. Do not silently pick an unsupported one. */
    if (!(c.sagaw_mask & ((1u << 1) | (1u << 2) | (1u << 3)))) return -4;
    if (c.gaw < 39) return -5;
    *out = c;
    return 0;
}
