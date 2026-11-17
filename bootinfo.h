#ifndef KIRA_BOOTINFO_H
#define KIRA_BOOTINFO_H
#include <stdint.h>

/* Versioned, loader-neutral KIRA handoff. All offsets are from the start of
   this blob; all physical addresses are physical, never loader virtual pointers.
   Integers are little-endian on both supported targets. */
#define KIRA_BOOTINFO_MAGIC 0x4941524Bu /* "KRAI" */
#define KIRA_BOOTINFO_VERSION 1u
#define KIRA_BOOTINFO_MAX_SIZE (1u << 20)
#define KIRA_BOOTINFO_MAX_MMAP 4096u
#define KIRA_BOOTINFO_MAX_MODULES 64u

enum { KBI_ARCH_X86_64=1, KBI_ARCH_AARCH64=2 };
enum { KBI_BOOT_BIOS=1, KBI_BOOT_UEFI=2, KBI_BOOT_DEVICE_TREE=3, KBI_BOOT_MULTIBOOT2=4 };
enum { KBI_F_FRAMEBUFFER=1u, KBI_F_ACPI=2u, KBI_F_FDT=4u, KBI_F_CMDLINE=8u };
/* Состояние доверенной загрузки (А2, docs/SECURE_BOOT.md). Нули у всех загрузчиков без цепочки доверия: BIOS, GRUB, дерево устройств. */
enum { KBI_TRUST_SB_KNOWN=1u,      /* загрузчик смог прочитать переменную SecureBoot (иначе состояние неизвестно) */
       KBI_TRUST_SB_ENABLED=2u,    /* SecureBoot=1: прошивка проверила подпись загрузчика */
       KBI_TRUST_SETUP_MODE=4u,    /* SetupMode=1: ключи платформы не заведены, подпись ничего не доказывает */
       KBI_TRUST_ENFORCING=8u,     /* загрузчик собран с KIRA_SECURE=1 и без валидной подписи ядра не передаёт управление */
       KBI_TRUST_SIG_VERIFIED=16u, /* подпись манифеста, хэши ядра и initrd проверены (поле image_version достоверно) */
       KBI_TRUST_MEASURED=32u,     /* хэши ядра и initrd внесены в PCR 9 через TCG2 */
       KBI_TRUST_CMDLINE_IGNORED=64u, /* в доверенном режиме командная строка из NVRAM не принимается */
       KBI_TRUST_ALL=127u };
enum { KBI_MEM_USABLE=1, KBI_MEM_RESERVED=2, KBI_MEM_ACPI_RECLAIM=3,
       KBI_MEM_ACPI_NVS=4, KBI_MEM_BAD=5, KBI_MEM_BOOTLOADER=6,
       KBI_MEM_KERNEL=7, KBI_MEM_MODULE=8, KBI_MEM_FRAMEBUFFER=9 };

typedef struct __attribute__((packed)) {
    uint64_t base, length;
    uint32_t type, attributes;
} kbi_mmap_entry_t;

typedef struct __attribute__((packed)) {
    uint64_t base, length;
    uint32_t name_off, name_len;
    uint32_t flags, reserved;
} kbi_module_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, header_size;
    uint32_t total_size, flags;
    uint16_t arch, boot_mode;
    uint32_t mmap_off, mmap_count, mmap_entry_size, image_version;   /* image_version: номер выпуска из подписанного манифеста (0 - не проверялся) */
    uint64_t rsdp_phys, fdt_phys;
    uint64_t fb_phys, fb_size;
    uint32_t fb_width, fb_height, fb_pitch;
    uint8_t fb_bpp, fb_red_pos, fb_red_size, fb_green_pos;
    uint8_t fb_green_size, fb_blue_pos, fb_blue_size, fb_reserved;
    uint32_t modules_off, modules_count, module_entry_size, cmdline_off;
    uint32_t cmdline_len, trust_flags;   /* trust_flags: KBI_TRUST_*, заполняет только загрузчик UEFI */
    uint64_t kernel_phys_base, kernel_size;
    uint64_t loader_id;
} kbi_header_t;

_Static_assert(sizeof(kbi_mmap_entry_t)==24, "KBI mmap ABI");
_Static_assert(sizeof(kbi_module_t)==32, "KBI module ABI");
_Static_assert(sizeof(kbi_header_t)==136, "KBI header ABI");

/* Returns 0 only for a structurally valid v1 handoff. Does not dereference
   physical addresses: only validates offsets and ranges inside blob. */
int kbi_validate(const void* blob, uint32_t available);
int kbi_from_multiboot2(const void* info, uint32_t available,
                        uint64_t kernel_base, uint64_t kernel_size,
                        const void** result, uint32_t* result_size);
int kbi_from_fdt(const void* fdt, uint32_t available, uint64_t fdt_phys,
                 uint64_t kernel_base, uint64_t kernel_size,
                 void* out, uint32_t capacity, uint32_t* out_size);
const kbi_mmap_entry_t* kbi_mmap(const void* blob);
const kbi_module_t* kbi_modules(const void* blob);

/* UEFI entry compatibility: convert validated KBI v1 into bounded Multiboot2 tags. */
int kbi_to_multiboot2(const void *blob, uint32_t available,
                      void *output, uint32_t capacity, uint32_t *output_size);

#endif
