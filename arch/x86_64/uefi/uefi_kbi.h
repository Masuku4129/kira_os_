#ifndef KIRA_UEFI_KBI_H
#define KIRA_UEFI_KBI_H

#include <efi.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t format;
    uint64_t rsdp;
    uint64_t kernel_base;
    uint64_t kernel_size;
} kira_uefi_kbi_info;

int kira_uefi_gop_geometry_valid(uint64_t base, uint64_t size,
                                uint32_t width, uint32_t height,
                                uint32_t pixels_per_scanline,
                                uint32_t format, uint32_t *pitch_out);

/* 0 = last build accepted; 1/2 = bad firmware descriptor; negative = kbi_validate() code. */
int kira_uefi_kbi_reject_reason(void);

EFI_STATUS kira_uefi_build_kbi(const void *efi_map,
                               UINTN map_size,
                               UINTN descriptor_size,
                               UINT32 descriptor_version,
                               const kira_uefi_kbi_info *info,
                               void *output,
                               UINT32 output_capacity,
                               UINT32 *output_size);

#endif
