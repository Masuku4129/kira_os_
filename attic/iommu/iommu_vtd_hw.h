#ifndef KIRA_IOMMU_VTD_HW_H
#define KIRA_IOMMU_VTD_HW_H
#include <stdint.h>
#include "iommu_vtd.h"

typedef struct {
    uint32_t version;
    uint8_t nd, gaw, sagaw_mask;
    uint8_t supports_queued_invalidation;
    uint8_t supports_interrupt_remapping;
    uint8_t supports_posted_interrupts;
    uint8_t supports_device_tlb;
    uint8_t supports_page_selective_invalidation;
    uint8_t supports_write_buffer_flush;
    uint8_t supports_read_drain;
    uint8_t supports_write_drain;
    uint8_t supports_translation;
    uint8_t supports_pass_through;
    uint8_t supports_scalable_mode;
    uint8_t supports_first_level;
} vtd_hw_caps_t;

/* Read-only capability discovery. regs must be a valid, UC-mapped 4-KiB
 * register page for the DRHD unit. No command or control register is written. */
int vtd_hw_probe(const volatile void *regs, vtd_hw_caps_t *caps);

#endif
