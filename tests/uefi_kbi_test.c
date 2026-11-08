#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "arch/x86_64/uefi/uefi_kbi.h"
#include "bootinfo.h"

#define CHECK(x) do { if (!(x)) { printf("uefi_kbi FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)

static void descriptor(EFI_MEMORY_DESCRIPTOR *d, UINT32 type,
                       uint64_t base, uint64_t pages) {
    memset(d, 0, sizeof(*d));
    d->Type = type;
    d->PhysicalStart = base;
    d->NumberOfPages = pages;
    d->Attribute = 0x8;
}

int main(void) {
    EFI_MEMORY_DESCRIPTOR map[5];
    kira_uefi_kbi_info info;
    unsigned char blob[KIRA_BOOTINFO_MAX_SIZE];
    uint32_t size = 0xdeadbeefu;
    const kbi_header_t *h;
    const kbi_mmap_entry_t *entries;

    {
        uint32_t pitch = 0xdeadbeefu;
        CHECK(kira_uefi_gop_geometry_valid(0xe0000000, 0x400000,
                                           1024, 768, 1024, 1, &pitch));
        CHECK(pitch == 4096);
        CHECK(!kira_uefi_gop_geometry_valid(0xe0000000, 0x1000,
                                            1024, 768, 1024, 1, &pitch));
        CHECK(!kira_uefi_gop_geometry_valid(0xe0000000, 0x400000,
                                            1024, 768, 1000, 1, &pitch));
        CHECK(!kira_uefi_gop_geometry_valid(UINT64_MAX - 0xfffu, 0x2000,
                                            640, 480, 640, 1, &pitch));
        CHECK(!kira_uefi_gop_geometry_valid(0xe0000000, 0x400000,
                                            640, 480, 640, 0, &pitch));
        CHECK(!kira_uefi_gop_geometry_valid(0xe0000000, 0x400000,
                                            16385, 480, 16385, 1, &pitch));
    }

    memset(&info, 0, sizeof(info));
    info.kernel_base = 0x200000;
    info.kernel_size = 0x180000;
    info.rsdp = 0xf0000;
    info.framebuffer_base = 0xe0000000;
    info.framebuffer_size = 0x400000;
    info.width = 1024;
    info.height = 768;
    info.pitch = 4096;
    info.format = 1;

    descriptor(&map[0], EfiConventionalMemory, 0x100000, 0x100);
    descriptor(&map[1], EfiACPIReclaimMemory, 0x900000, 4);
    descriptor(&map[2], EfiLoaderData, 0xa00000, 1);
    descriptor(&map[3], EfiBootServicesData, 0xa01000, 1);
    descriptor(&map[4], EfiMemoryMappedIO, 0xe0000000, 0x400);

    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_SUCCESS);
    CHECK(size == sizeof(kbi_header_t) + 5 * sizeof(kbi_mmap_entry_t));
    CHECK(kbi_validate(blob, size) == 0);
    h = (const kbi_header_t *)blob;
    entries = kbi_mmap(blob);
    CHECK(h->arch == KBI_ARCH_X86_64 && h->boot_mode == KBI_BOOT_UEFI);
    CHECK((h->flags & (KBI_F_ACPI | KBI_F_FRAMEBUFFER)) ==
          (KBI_F_ACPI | KBI_F_FRAMEBUFFER));
    CHECK(h->rsdp_phys == info.rsdp && h->kernel_phys_base == info.kernel_base);
    CHECK(entries[0].type == KBI_MEM_USABLE && entries[0].length == 0x100000);
    CHECK(entries[1].type == KBI_MEM_ACPI_RECLAIM && entries[1].length == 0x4000);
    CHECK(entries[2].type == KBI_MEM_BOOTLOADER);
    CHECK(entries[3].type == KBI_MEM_BOOTLOADER);
    CHECK(entries[4].type == KBI_MEM_RESERVED);

    /* Unsupported GOP pixel formats must produce a valid headless handoff. */
    info.format = 0;
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_SUCCESS);
    h = (const kbi_header_t *)blob;
    CHECK(!(h->flags & KBI_F_FRAMEBUFFER) && h->fb_size == 0);
    info.format = 1;

    /* Invalid GOP pitch and overlapping firmware ranges must fail closed. */
    info.pitch = 1;
    size = 0xdeadbeefu;
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);
    CHECK(size == 0xdeadbeefu);
    info.pitch = 4096;
    descriptor(&map[1], EfiACPIReclaimMemory, 0x180000, 4);
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);
    descriptor(&map[1], EfiACPIReclaimMemory, 0x900000, 4);

    descriptor(&map[0], EfiConventionalMemory, 0x100000, UINT64_MAX / 4096u + 1u);
    size = 0xdeadbeefu;
    CHECK(kira_uefi_build_kbi(map, sizeof(map[0]), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);
    CHECK(size == 0xdeadbeefu);

    descriptor(&map[0], EfiConventionalMemory, UINT64_MAX - 0xfffu, 2);
    CHECK(kira_uefi_build_kbi(map, sizeof(map[0]), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);

    CHECK(kira_uefi_build_kbi(map, sizeof(map[0]) - 1, sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_INVALID_PARAMETER);
    puts("uefi_kbi: PASS");
    return 0;
}
