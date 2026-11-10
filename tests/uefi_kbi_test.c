#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "arch/x86_64/uefi/uefi_kbi.h"
#include "bootinfo.h"

#define CHECK(x) do { if (!(x)) { printf("uefi_kbi FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)

static const char test_module_name[] = "initrd.img";
static const char test_command_line[] = "root=/dev/vda quiet";

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
    const kbi_module_t *modules;

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
    info.module_base = 0xa00000;
    info.module_size = 128;
    info.module_name = test_module_name;
    info.module_name_len = sizeof(test_module_name);
    info.command_line = test_command_line;
    info.command_line_len = sizeof(test_command_line);

    descriptor(&map[0], EfiConventionalMemory, 0x100000, 0x100);
    descriptor(&map[1], EfiACPIReclaimMemory, 0x900000, 4);
    descriptor(&map[2], EfiLoaderData, 0xa00000, 1);
    descriptor(&map[3], EfiBootServicesData, 0xa01000, 1);
    descriptor(&map[4], EfiMemoryMappedIO, 0xe0000000, 0x400);

    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_SUCCESS);
    CHECK(size > sizeof(kbi_header_t) + 5 * sizeof(kbi_mmap_entry_t));
    CHECK(kbi_validate(blob, size) == 0);
    h = (const kbi_header_t *)blob;
    entries = kbi_mmap(blob);
    modules = kbi_modules(blob);
    CHECK(h->arch == KBI_ARCH_X86_64 && h->boot_mode == KBI_BOOT_UEFI);
    CHECK((h->flags & (KBI_F_ACPI | KBI_F_FRAMEBUFFER | KBI_F_CMDLINE)) ==
          (KBI_F_ACPI | KBI_F_FRAMEBUFFER | KBI_F_CMDLINE));
    CHECK(h->rsdp_phys == info.rsdp && h->kernel_phys_base == info.kernel_base);
    CHECK(h->modules_count == 1 && h->module_entry_size == sizeof(kbi_module_t));
    CHECK(modules && modules[0].base == info.module_base && modules[0].length == info.module_size);
    CHECK(modules[0].name_len == sizeof(test_module_name));
    CHECK(memcmp((const uint8_t *)blob + modules[0].name_off,
                 test_module_name, sizeof(test_module_name)) == 0);
    CHECK((h->flags & KBI_F_CMDLINE) && h->cmdline_len == sizeof(test_command_line));
    CHECK(memcmp((const uint8_t *)blob + h->cmdline_off,
                 test_command_line, sizeof(test_command_line)) == 0);
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

    /* Module ranges overlapping the kernel and unterminated strings are rejected. */
    info.module_base = info.kernel_base;
    size = 0xdeadbeefu;
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);
    CHECK(size == 0xdeadbeefu);
    info.module_base = 0xa00000;
    info.command_line = "bad";
    info.command_line_len = 3;
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_INVALID_PARAMETER);
    info.command_line = test_command_line;
    info.command_line_len = sizeof(test_command_line);

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
    /* OVMF/QEMU place the RSDP on an 8-byte boundary (seen: 0x3F4EBF98). The
       16-byte rule is for the legacy BIOS scan and must not reject UEFI. */
    descriptor(&map[0], EfiConventionalMemory, 0x100000, 0x100);
    info.rsdp = 0x3F4EBF98ull;
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_SUCCESS);
    CHECK(kira_uefi_kbi_reject_reason() == 0);
    CHECK(((const kbi_header_t *)blob)->rsdp_phys == 0x3F4EBF98ull);
    info.rsdp = 0xf0000;

    /* The loader reports why a handoff was refused instead of a bare Load Error. */
    descriptor(&map[1], EfiACPIReclaimMemory, 0x180000, 4); /* overlaps map[0] */
    CHECK(kira_uefi_build_kbi(map, sizeof(map), sizeof(map[0]), 1,
                              &info, blob, sizeof(blob), &size) == EFI_LOAD_ERROR);
    CHECK(kira_uefi_kbi_reject_reason() == -11);
    descriptor(&map[1], EfiACPIReclaimMemory, 0x900000, 4);

    puts("uefi_kbi: PASS");
    return 0;
}
