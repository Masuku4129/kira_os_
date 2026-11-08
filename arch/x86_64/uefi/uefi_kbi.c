/*
 * UEFI memory-map to loader-neutral KIRA BootInfo v1 adapter.
 *
 * The resulting blob is self-contained: offsets are relative to its start,
 * memory-map records are copied, and physical addresses remain physical.
 * The caller must allocate output from EFI LoaderData and retain it across
 * ExitBootServices.
 */

#include "uefi_kbi.h"
#include "../../../bootinfo.h"

static uint32_t kbi_mem_type(UINT32 type)
{
    switch (type) {
    case EfiConventionalMemory:
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        /* Keep loader-owned pages reserved until the kernel explicitly reclaims them. */
        return KBI_MEM_BOOTLOADER;
    case EfiACPIReclaimMemory:
        return KBI_MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:
        return KBI_MEM_ACPI_NVS;
    case EfiUnusableMemory:
        return KBI_MEM_BAD;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
    case EfiReservedMemoryType:
    default:
        return KBI_MEM_RESERVED;
    }
}

int kira_uefi_gop_geometry_valid(uint64_t base, uint64_t size,
                                uint32_t width, uint32_t height,
                                uint32_t pixels_per_scanline,
                                uint32_t format, uint32_t *pitch_out)
{
    uint64_t pitch, required;

    if (!pitch_out || !base || !size || !width || !height ||
        width > 16384u || height > 16384u ||
        pixels_per_scanline < width || (format != 1u && format != 2u) ||
        size > UINT64_MAX - base)
        return 0;

    pitch = (uint64_t)pixels_per_scanline * 4u;
    if (pitch > UINT32_MAX || height > UINT64_MAX / pitch)
        return 0;
    required = pitch * height;
    if (required > size)
        return 0;

    *pitch_out = (uint32_t)pitch;
    return 1;
}

static int add_overflow_u64(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a;
}

EFI_STATUS kira_uefi_build_kbi(const void *efi_map,
                               UINTN map_size,
                               UINTN descriptor_size,
                               UINT32 descriptor_version,
                               const kira_uefi_kbi_info *info,
                               void *output,
                               UINT32 output_capacity,
                               UINT32 *output_size)
{
    const uint8_t *source = (const uint8_t *)efi_map;
    uint8_t *blob = (uint8_t *)output;
    kbi_header_t *header;
    kbi_mmap_entry_t *entries;
    UINTN count, i;
    UINT32 mmap_offset, total_size;

    (void)descriptor_version;

    if (!efi_map || !info || !output || !output_size ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 || map_size % descriptor_size != 0)
        return EFI_INVALID_PARAMETER;

    count = map_size / descriptor_size;
    if (count > KIRA_BOOTINFO_MAX_MMAP)
        return EFI_OUT_OF_RESOURCES;

    mmap_offset = (UINT32)sizeof(kbi_header_t);
    if (count > (UINT32_MAX - mmap_offset) / sizeof(kbi_mmap_entry_t))
        return EFI_BAD_BUFFER_SIZE;

    total_size = mmap_offset + (UINT32)count * sizeof(kbi_mmap_entry_t);
    if (total_size > output_capacity || total_size > KIRA_BOOTINFO_MAX_SIZE)
        return EFI_BUFFER_TOO_SMALL;

    for (i = 0; i < total_size; ++i)
        blob[i] = 0;

    header = (kbi_header_t *)blob;
    entries = (kbi_mmap_entry_t *)(blob + mmap_offset);

    header->magic = KIRA_BOOTINFO_MAGIC;
    header->version = KIRA_BOOTINFO_VERSION;
    header->header_size = sizeof(*header);
    header->total_size = total_size;
    header->arch = KBI_ARCH_X86_64;
    header->boot_mode = KBI_BOOT_UEFI;
    header->mmap_off = mmap_offset;
    header->mmap_count = (uint32_t)count;
    header->mmap_entry_size = sizeof(kbi_mmap_entry_t);
    header->rsdp_phys = info->rsdp;
    header->kernel_phys_base = info->kernel_base;
    header->kernel_size = info->kernel_size;

    if (info->rsdp)
        header->flags |= KBI_F_ACPI;

    /* GOP PixelBitMask and PixelBltOnly need their own conversion path.
       Do not publish a half-filled framebuffer record for an unsupported mode:
       the kernel must still boot headless instead of rejecting all BootInfo. */
    if (info->framebuffer_base && info->framebuffer_size &&
        info->width && info->height && info->pitch &&
        (info->format == 1 || info->format == 2)) {
        header->fb_phys = info->framebuffer_base;
        header->fb_size = info->framebuffer_size;
        header->fb_width = info->width;
        header->fb_height = info->height;
        header->fb_pitch = info->pitch;
        header->fb_bpp = 32;
        if (info->format == 1) {
            header->fb_red_pos = 0;
            header->fb_red_size = 8;
            header->fb_green_pos = 8;
            header->fb_green_size = 8;
            header->fb_blue_pos = 16;
            header->fb_blue_size = 8;
        } else if (info->format == 2) {
            header->fb_red_pos = 16;
            header->fb_red_size = 8;
            header->fb_green_pos = 8;
            header->fb_green_size = 8;
            header->fb_blue_pos = 0;
            header->fb_blue_size = 8;
        }
        header->flags |= KBI_F_FRAMEBUFFER;
    }

    for (i = 0; i < count; ++i) {
        EFI_MEMORY_DESCRIPTOR descriptor;
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(source + i * descriptor_size);

        descriptor = *d;
        /* Check multiplication before computing the byte length. A hostile or
           corrupt descriptor must not wrap NumberOfPages * PAGE_SIZE to a
           small range and make reserved physical memory look usable. */
        if (descriptor.NumberOfPages == 0 ||
            descriptor.NumberOfPages > UINT64_MAX / 4096ull)
            return EFI_LOAD_ERROR;
        if (add_overflow_u64(descriptor.PhysicalStart,
                             descriptor.NumberOfPages * 4096ull))
            return EFI_LOAD_ERROR;

        entries[i].base = descriptor.PhysicalStart;
        entries[i].length = descriptor.NumberOfPages * 4096ull;
        entries[i].type = kbi_mem_type(descriptor.Type);
        entries[i].attributes = (uint32_t)(descriptor.Attribute & 0xffffffffu);
    }

    /* Validate the complete handoff before exposing it to the kernel.
       This also rejects overlapping/out-of-order firmware descriptors and
       malformed framebuffer geometry at the loader boundary. */
    if (kbi_validate(blob, total_size) != 0)
        return EFI_LOAD_ERROR;

    *output_size = total_size;
    return EFI_SUCCESS;
}
