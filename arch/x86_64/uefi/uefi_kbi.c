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

#define KBI_EFI_PAGE_SIZE 4096ull
#define KBI_PIXEL_BYTES   4ull

static uint32_t kbi_mem_type(UINT32 type)
{
    switch (type) {
    case EfiConventionalMemory:
        return KBI_MEM_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        /* Keep loader-owned pages reserved until the kernel copies the handoff. */
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

static int add_overflow_u64(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a;
}

static int mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (a != 0 && b > UINT64_MAX / a)
        return 1;
    *result = a * b;
    return 0;
}

static void copy_descriptor(EFI_MEMORY_DESCRIPTOR *destination,
                            const uint8_t *source)
{
    uint8_t *out = (uint8_t *)destination;
    UINTN i;

    for (i = 0; i < sizeof(*destination); ++i)
        out[i] = source[i];
}

static int framebuffer_valid(const kira_uefi_kbi_info *info)
{
    uint64_t required;

    if (!info->framebuffer_base && !info->framebuffer_size &&
        !info->width && !info->height && !info->pitch)
        return 1;

    if (!info->framebuffer_base || !info->framebuffer_size ||
        !info->width || !info->height || info->pitch < info->width ||
        info->format > 2 ||
        add_overflow_u64(info->framebuffer_base, info->framebuffer_size) ||
        mul_overflow_u64((uint64_t)info->pitch,
                         (uint64_t)info->height, &required) ||
        mul_overflow_u64(required, KBI_PIXEL_BYTES, &required))
        return 0;

    return required <= info->framebuffer_size;
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

    if (output_size)
        *output_size = 0;

    if (!efi_map || !info || !output || !output_size ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 || map_size % descriptor_size != 0)
        return EFI_INVALID_PARAMETER;

    count = map_size / descriptor_size;
    if (count > KIRA_BOOTINFO_MAX_MMAP)
        return EFI_OUT_OF_RESOURCES;

    if ((info->kernel_base == 0) != (info->kernel_size == 0) ||
        (info->kernel_size &&
         add_overflow_u64(info->kernel_base, info->kernel_size)) ||
        !framebuffer_valid(info))
        return EFI_LOAD_ERROR;

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

    /* Unknown GOP pixel formats are valid boot states without a usable FB. */
    if (info->framebuffer_base && info->format != 0) {
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
        } else {
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
        const uint8_t *record = source + i * descriptor_size;
        uint64_t length;

        copy_descriptor(&descriptor, record);
        if (descriptor.NumberOfPages == 0 ||
            mul_overflow_u64(descriptor.NumberOfPages,
                             KBI_EFI_PAGE_SIZE, &length) ||
            add_overflow_u64(descriptor.PhysicalStart, length))
            return EFI_LOAD_ERROR;

        entries[i].base = descriptor.PhysicalStart;
        entries[i].length = length;
        entries[i].type = kbi_mem_type(descriptor.Type);
        entries[i].attributes =
            (uint32_t)(descriptor.Attribute & 0xffffffffu);
    }

    *output_size = total_size;
    return EFI_SUCCESS;
}
