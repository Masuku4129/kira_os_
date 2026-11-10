#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bootinfo.h"

int kbi_to_multiboot2(const void *blob, uint32_t available,
                      void *output, uint32_t capacity, uint32_t *output_size);

#define CHECK(x) do { if (!(x)) { printf("bootinfo_uefi FAIL:%d %s\\n",__LINE__,#x); return 1; } } while (0)

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }
static const char module_name[] = "initrd0";
static const char command_line[] = "root=/dev/vda quiet";

int main(void) {
    uint8_t kbi[sizeof(kbi_header_t) + 3 * sizeof(kbi_mmap_entry_t) +
                sizeof(kbi_module_t) + sizeof(module_name) +
                sizeof(command_line)] = {0};
    uint8_t mbi[512];
    uint32_t out_size = 0;
    kbi_header_t *h = (kbi_header_t *)kbi;
    kbi_mmap_entry_t *m = (kbi_mmap_entry_t *)(kbi + sizeof(*h));
    uint32_t modules_off = sizeof(*h) + 3 * sizeof(*m);
    kbi_module_t *mod = (kbi_module_t *)(kbi + modules_off);
    uint32_t name_off = modules_off + sizeof(*mod);
    uint32_t cmdline_off = name_off + sizeof(module_name);
    uint32_t off, total;
    int saw_map = 0, saw_fb = 0, saw_cmdline = 0, saw_module = 0;

    h->magic = KIRA_BOOTINFO_MAGIC;
    h->version = KIRA_BOOTINFO_VERSION;
    h->header_size = sizeof(*h);
    h->total_size = sizeof(kbi);
    h->arch = KBI_ARCH_X86_64;
    h->boot_mode = KBI_BOOT_UEFI;
    h->mmap_off = sizeof(*h);
    h->mmap_count = 3;
    h->mmap_entry_size = sizeof(*m);
    h->modules_off = modules_off;
    h->modules_count = 1;
    h->module_entry_size = sizeof(*mod);
    h->cmdline_off = cmdline_off;
    h->cmdline_len = sizeof(command_line);
    h->kernel_phys_base = 0x100000;
    h->kernel_size = 0x180000;
    h->flags = KBI_F_FRAMEBUFFER | KBI_F_ACPI | KBI_F_CMDLINE;
    h->rsdp_phys = 0x9fc00;
    h->fb_phys = 0xe0000000;
    h->fb_size = 4096u * 768u;
    h->fb_width = 1024;
    h->fb_height = 768;
    h->fb_pitch = 4096;
    h->fb_bpp = 32;
    h->fb_red_pos = 16; h->fb_red_size = 8;
    h->fb_green_pos = 8; h->fb_green_size = 8;
    h->fb_blue_pos = 0; h->fb_blue_size = 8;

    m[0] = (kbi_mmap_entry_t){0x100000, 0x100000, KBI_MEM_USABLE, 0};
    m[1] = (kbi_mmap_entry_t){0x900000, 0x4000, KBI_MEM_ACPI_RECLAIM, 0};
    m[2] = (kbi_mmap_entry_t){0xe0000000, 0x400000, KBI_MEM_RESERVED, 0};
    mod->base = 0x400000;
    mod->length = 0x2000;
    mod->name_off = name_off;
    mod->name_len = sizeof(module_name);
    memcpy(kbi + name_off, module_name, sizeof(module_name));
    memcpy(kbi + cmdline_off, command_line, sizeof(command_line));

    CHECK(kbi_validate(kbi, sizeof(kbi)) == 0);
    CHECK(kbi_to_multiboot2(kbi, sizeof(kbi), mbi, sizeof(mbi), &out_size) == 0);
    CHECK(out_size >= 16 && !(out_size & 7u));
    total = *(const uint32_t *)mbi;
    CHECK(total == out_size);
    off = 8;
    while (off + 8 <= total) {
        uint32_t type = *(const uint32_t *)(mbi + off);
        uint32_t size = *(const uint32_t *)(mbi + off + 4);
        CHECK(size >= 8 && size <= total - off);
        if (type == 6) {
            CHECK(size == 16 + 3 * 24);
            CHECK(*(const uint32_t *)(mbi + off + 8) == 24);
            CHECK(*(const uint32_t *)(mbi + off + 16 + 16) == 1);
            CHECK(*(const uint32_t *)(mbi + off + 16 + 24 + 16) == 3);
            saw_map = 1;
        } else if (type == 8) {
            CHECK(size == 38);
            CHECK(*(const uint64_t *)(mbi + off + 8) == h->fb_phys);
            CHECK(*(const uint32_t *)(mbi + off + 16) == h->fb_pitch);
            CHECK(*(const uint8_t *)(mbi + off + 28) == 32);
            CHECK(*(const uint8_t *)(mbi + off + 29) == 1);
            saw_fb = 1;
        } else if (type == 1) {
            CHECK(size == 8 + sizeof(command_line));
            CHECK(memcmp(mbi + off + 8, command_line, sizeof(command_line)) == 0);
            saw_cmdline = 1;
        } else if (type == 3) {
            CHECK(size == 16 + sizeof(module_name));
            CHECK(*(const uint32_t *)(mbi + off + 8) == 0x400000);
            CHECK(*(const uint32_t *)(mbi + off + 12) == 0x402000);
            CHECK(memcmp(mbi + off + 16, module_name, sizeof(module_name)) == 0);
            saw_module = 1;
        } else if (type == 0) {
            CHECK(size == 8 && off + size == total);
            break;
        } else {
            CHECK(0);
        }
        off += align8(size);
    }
    CHECK(saw_map && saw_fb && saw_cmdline && saw_module);

    /* MB2 module start/end fields are 32-bit: refuse truncation instead of handing the kernel a bogus range. */
    mod->base = 0x100000000ull;
    CHECK(kbi_validate(kbi, sizeof(kbi)) == 0);
    CHECK(kbi_to_multiboot2(kbi, sizeof(kbi), mbi, sizeof(mbi), &out_size) == -5);
    mod->base = 0x400000;

    CHECK(kbi_to_multiboot2(kbi, sizeof(kbi) - 1, mbi, sizeof(mbi), &out_size) != 0);
    CHECK(kbi_to_multiboot2(kbi, sizeof(kbi), mbi, 16, &out_size) != 0);
    puts("bootinfo_uefi: PASS");
    return 0;
}
