/*
 * KIRA OS x86-64 UEFI application loader.
 *
 * Build as PE32+ EFI application with GNU-EFI or an equivalent UEFI ABI
 * toolchain. The loader reads \EFI\KIRA\KIRA.ELF from the EFI system
 * partition, validates ELF64 PT_LOAD segments, captures GOP and memory-map
 * information, exits boot services, and transfers control to the kernel.
 *
 * The kernel entry is called with the Microsoft x64 ABI:
 *   RCX = pointer to the loader-neutral KIRA BootInfo v1 blob
 *   RDX = byte size of that blob
 */

#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
#include "uefi_kbi.h"

#define KIRA_ELF_MAGIC       0x464c457f
#define KIRA_ELFCLASS64      2
#define KIRA_ELFDATA2LSB     1
#define KIRA_ELF_ET_EXEC     2
#define KIRA_ELF_EM_X86_64   62
#define KIRA_PT_LOAD         1
#define KIRA_MAX_SEGMENTS    32
#define KIRA_PAGE_SIZE       4096
#define KIRA_BOOTSTRAP_LIMIT (1ull << 30)
#define KIRA_MAX_KERNEL_SPAN (128ull * 1024ull * 1024ull)
#define KIRA_KERNEL_PATH     L"\\EFI\\KIRA\\KIRA.ELF"
#define KIRA_BOOTINFO_PAGES  32 /* enough for the bounded 4096-entry KBI memory map */

typedef struct __attribute__((packed)) {
    UINT8 ident[16];
    UINT16 type;
    UINT16 machine;
    UINT32 version;
    UINT64 entry;
    UINT64 phoff;
    UINT64 shoff;
    UINT32 flags;
    UINT16 ehsize;
    UINT16 phentsize;
    UINT16 phnum;
    UINT16 shentsize;
    UINT16 shnum;
    UINT16 shstrndx;
} kira_elf64_header;

typedef struct __attribute__((packed)) {
    UINT32 type;
    UINT32 flags;
    UINT64 offset;
    UINT64 vaddr;
    UINT64 paddr;
    UINT64 filesz;
    UINT64 memsz;
    UINT64 align;
} kira_elf64_program_header;

typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINT64 size;
} kira_loaded_segment;

static EFI_SYSTEM_TABLE *g_st;
static EFI_BOOT_SERVICES *g_bs;

/* Keep GUID values local: GNU-EFI releases differ in which ACPI aliases
   they export, while these UEFI protocol identifiers are stable. */
static EFI_GUID kira_file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static EFI_GUID kira_acpi20_table_guid = {
    0x8868e871, 0xe4f1, 0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};
static EFI_GUID kira_acpi_table_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3,
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};

static void print_error(CHAR16 *message, EFI_STATUS status)
{
    Print(L"KIRA UEFI: %s (status=%lx)\r\n", message, status);
}

static BOOLEAN range_valid(UINT64 offset, UINT64 length, UINT64 total)
{
    return offset <= total && length <= total - offset;
}

static EFI_STATUS read_kernel_image(EFI_HANDLE image,
                                    VOID **buffer,
                                    UINTN *buffer_size)
{
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status;
    EFI_FILE_INFO *info = NULL;
    UINTN info_size = 0;
    UINTN size;

    status = uefi_call_wrapper(g_bs->HandleProtocol, 3, image, &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(g_bs->HandleProtocol, 3, loaded->DeviceHandle,
                                  &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(root->Open, 5, root, &file, KIRA_KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file, &kira_file_info_guid, &info_size, NULL);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData, info_size, (VOID **)&info);
        if (!EFI_ERROR(status))
            status = uefi_call_wrapper(file->GetInfo, 4, file, &kira_file_info_guid, &info_size, info);
    }
    if (EFI_ERROR(status)) {
        if (info)
            uefi_call_wrapper(g_bs->FreePool, 1, info);
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    if (info->FileSize == 0 || info->FileSize > 128ull * 1024ull * 1024ull) {
        uefi_call_wrapper(g_bs->FreePool, 1, info);
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return EFI_BAD_BUFFER_SIZE;
    }

    size = (UINTN)info->FileSize;
    uefi_call_wrapper(g_bs->FreePool, 1, info);

    status = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData, size, buffer);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    *buffer_size = size;
    status = uefi_call_wrapper(file->Read, 3, file, buffer_size, *buffer);
    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);

    if (EFI_ERROR(status) || *buffer_size != size) {
        uefi_call_wrapper(g_bs->FreePool, 1, *buffer);
        *buffer = NULL;
        return EFI_ERROR(status) ? status : EFI_END_OF_FILE;
    }

    return EFI_SUCCESS;
}


#define KIRA_INITRD_PATH L"\\EFI\\KIRA\\INITRD.IMG"
#define KIRA_INITRD_MAX_SIZE (64ull * 1024ull * 1024ull)
#define KIRA_CMDLINE_MAX 1024u

static char g_boot_command_line[KIRA_CMDLINE_MAX];

static EFI_STATUS collect_command_line(EFI_HANDLE image,
                                       kira_uefi_kbi_info *info)
{
    EFI_LOADED_IMAGE *loaded = NULL;
    const CHAR16 *options;
    UINTN units, i, out = 0;
    EFI_STATUS status;

    if (!info) return EFI_INVALID_PARAMETER;
    status = uefi_call_wrapper(g_bs->HandleProtocol, 3, image,
                               &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(status)) return status;
    if (!loaded->LoadOptions || !loaded->LoadOptionsSize) return EFI_SUCCESS;
    if ((loaded->LoadOptionsSize % sizeof(CHAR16)) != 0 ||
        loaded->LoadOptionsSize > KIRA_CMDLINE_MAX * sizeof(CHAR16))
        return EFI_BAD_BUFFER_SIZE;

    options = (const CHAR16 *)loaded->LoadOptions;
    units = loaded->LoadOptionsSize / sizeof(CHAR16);
    for (i = 0; i < units && options[i];) {
        UINT32 cp = options[i++];
        UINTN need;

        if (cp >= 0xd800u && cp <= 0xdbffu) {
            UINT32 low;
            if (i >= units) return EFI_UNSUPPORTED;
            low = options[i++];
            if (low < 0xdc00u || low > 0xdfffu) return EFI_UNSUPPORTED;
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
        } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
            return EFI_UNSUPPORTED;
        }
        if (cp < 0x20u && cp != 0x09u) return EFI_UNSUPPORTED;

        need = cp < 0x80u ? 1u : cp < 0x800u ? 2u :
               cp < 0x10000u ? 3u : 4u;
        if (need > sizeof(g_boot_command_line) - 1u - out)
            return EFI_BAD_BUFFER_SIZE;
        if (need == 1u) {
            g_boot_command_line[out++] = (char)cp;
        } else if (need == 2u) {
            g_boot_command_line[out++] = (char)(0xc0u | (cp >> 6));
            g_boot_command_line[out++] = (char)(0x80u | (cp & 0x3fu));
        } else if (need == 3u) {
            g_boot_command_line[out++] = (char)(0xe0u | (cp >> 12));
            g_boot_command_line[out++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
            g_boot_command_line[out++] = (char)(0x80u | (cp & 0x3fu));
        } else {
            g_boot_command_line[out++] = (char)(0xf0u | (cp >> 18));
            g_boot_command_line[out++] = (char)(0x80u | ((cp >> 12) & 0x3fu));
            g_boot_command_line[out++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
            g_boot_command_line[out++] = (char)(0x80u | (cp & 0x3fu));
        }
    }

    g_boot_command_line[out] = 0;
    if (out) {
        info->command_line = g_boot_command_line;
        info->command_line_len = (UINT32)out + 1u;
    }
    return EFI_SUCCESS;
}

/* INITRD.IMG is optional. When present it is loaded into reserved physical
   pages below the kernel's 1-GiB bootstrap identity map and retained after
   ExitBootServices. Its exact byte length, not rounded allocation size, is
   recorded in BootInfo. */
static EFI_STATUS load_boot_module(EFI_HANDLE image,
                                   kira_uefi_kbi_info *info)
{
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
    EFI_FILE_INFO *file_info = NULL;
    EFI_PHYSICAL_ADDRESS module_base = KIRA_BOOTSTRAP_LIMIT - 1u;
    UINTN info_size = 0, file_size, pages, read_size;
    EFI_STATUS status;
    static const char module_name[] = "initrd.img";

    if (!info) return EFI_INVALID_PARAMETER;
    status = uefi_call_wrapper(g_bs->HandleProtocol, 3, image,
                               &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(g_bs->HandleProtocol, 3, loaded->DeviceHandle,
                               &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(root->Open, 5, root, &file, KIRA_INITRD_PATH,
                               EFI_FILE_MODE_READ, 0);
    if (status == EFI_NOT_FOUND) {
        uefi_call_wrapper(root->Close, 1, root);
        return EFI_NOT_FOUND;
    }
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file, &kira_file_info_guid,
                               &info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        if (!EFI_ERROR(status)) status = EFI_DEVICE_ERROR;
        goto fail;
    }
    status = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData,
                               info_size, (VOID **)&file_info);
    if (EFI_ERROR(status)) goto fail;
    status = uefi_call_wrapper(file->GetInfo, 4, file,
                               &kira_file_info_guid, &info_size, file_info);
    if (EFI_ERROR(status)) goto fail;
    if (!file_info->FileSize || file_info->FileSize > KIRA_INITRD_MAX_SIZE) {
        status = EFI_BAD_BUFFER_SIZE;
        goto fail;
    }

    file_size = (UINTN)file_info->FileSize;
    pages = (file_size + KIRA_PAGE_SIZE - 1u) / KIRA_PAGE_SIZE;
    status = uefi_call_wrapper(g_bs->AllocatePages, 4, AllocateMaxAddress,
                               EfiLoaderData, pages, &module_base);
    if (EFI_ERROR(status)) goto fail;
    SetMem((VOID *)(UINTN)module_base, pages * KIRA_PAGE_SIZE, 0);
    read_size = file_size;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size,
                               (VOID *)(UINTN)module_base);
    if (EFI_ERROR(status) || read_size != file_size) {
        if (!EFI_ERROR(status)) status = EFI_END_OF_FILE;
        (VOID)uefi_call_wrapper(g_bs->FreePages, 2, module_base, pages);
        goto fail;
    }

    info->module_base = module_base;
    info->module_size = file_size;
    info->module_name = module_name;
    info->module_name_len = sizeof(module_name);

    if (file_info) uefi_call_wrapper(g_bs->FreePool, 1, file_info);
    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);
    Print(L"KIRA UEFI: loaded INITRD.IMG (%lu bytes at 0x%lx)\r\n",
          (UINT64)file_size, module_base);
    return EFI_SUCCESS;

fail:
    if (file_info) uefi_call_wrapper(g_bs->FreePool, 1, file_info);
    if (file) uefi_call_wrapper(file->Close, 1, file);
    if (root) uefi_call_wrapper(root->Close, 1, root);
    return status;
}

static void release_loaded_segments(kira_loaded_segment *segments,
                                     UINTN segment_count)
{
    while (segment_count) {
        kira_loaded_segment *segment = &segments[--segment_count];
        UINTN pages = (UINTN)((segment->size + KIRA_PAGE_SIZE - 1) /
                              KIRA_PAGE_SIZE);
        if (pages)
            (VOID)uefi_call_wrapper(g_bs->FreePages, 2, segment->base, pages);
    }
}

static EFI_STATUS load_elf64(VOID *image,
                             UINTN image_size,
                             EFI_PHYSICAL_ADDRESS *entry_out,
                             EFI_PHYSICAL_ADDRESS *base_out,
                             UINT64 *size_out)
{
    kira_elf64_header *eh = (kira_elf64_header *)image;
    kira_elf64_program_header *ph;
    kira_loaded_segment load_segments[KIRA_MAX_SEGMENTS];
    kira_loaded_segment allocation_segment;
    UINTN load_count = 0;
    UINTN allocation_count = 0;
    UINTN i;
    EFI_STATUS status = EFI_LOAD_ERROR;
    EFI_PHYSICAL_ADDRESS min_address = ~(EFI_PHYSICAL_ADDRESS)0;
    EFI_PHYSICAL_ADDRESS max_address = 0;
    EFI_PHYSICAL_ADDRESS allocation_base;
    EFI_PHYSICAL_ADDRESS allocation_end;
    EFI_PHYSICAL_ADDRESS allocation;
    UINTN allocation_pages;
    BOOLEAN entry_executable = FALSE;

    if (!image || !entry_out || !base_out || !size_out) {
        Print(L"KIRA UEFI: ELF loader arguments invalid\r\n");
        return EFI_LOAD_ERROR;
    }
    if (image_size < sizeof(*eh)) {
        Print(L"KIRA UEFI: ELF file too short: %lu\r\n", (UINT64)image_size);
        return EFI_LOAD_ERROR;
    }
    Print(L"KIRA UEFI: ELF header type=%u machine=%u ver=%u eh=%u phent=%u phnum=%u phoff=%lx entry=%lx size=%lu\r\n",
          eh->type, eh->machine, eh->version, eh->ehsize, eh->phentsize,
          eh->phnum, eh->phoff, eh->entry, (UINT64)image_size);
    Print(L"KIRA UEFI: ELF identity check begin\r\n");
    if (*(UINT32 *)eh->ident != KIRA_ELF_MAGIC ||
        eh->ident[4] != KIRA_ELFCLASS64 ||
        eh->ident[5] != KIRA_ELFDATA2LSB || eh->ident[6] != 1) {
        Print(L"KIRA UEFI: ELF identity rejected\r\n");
        return EFI_LOAD_ERROR;
    }
    Print(L"KIRA UEFI: ELF identity accepted\r\n");
    if (eh->type != KIRA_ELF_ET_EXEC || eh->machine != KIRA_ELF_EM_X86_64 ||
        eh->version != 1 || eh->ehsize != sizeof(*eh) ||
        eh->phentsize != sizeof(kira_elf64_program_header) ||
        eh->phnum == 0 || eh->phnum > KIRA_MAX_SEGMENTS) {
        Print(L"KIRA UEFI: ELF header fields rejected\r\n");
        return EFI_LOAD_ERROR;
    }
    if (!range_valid(eh->phoff,
                     (UINT64)eh->phnum * eh->phentsize,
                     image_size)) {
        Print(L"KIRA UEFI: ELF program header table outside file\r\n");
        return EFI_LOAD_ERROR;
    }

    Print(L"KIRA UEFI: ELF program-header table accepted\r\n");
    ph = (kira_elf64_program_header *)((UINT8 *)image + eh->phoff);
    Print(L"KIRA UEFI: ELF program-header pointer=%lx\r\n", (UINT64)(UINTN)ph);

    /*
     * ELF PT_LOAD addresses need only be congruent with file offsets modulo
     * p_align. They are not required to be page-aligned. Validate all ranges
     * first, then allocate one page-aligned span so adjacent segments that
     * share a physical page are not allocated twice.
     */
    for (i = 0; i < eh->phnum; ++i) {
        EFI_PHYSICAL_ADDRESS destination;
        UINT64 segment_end;
        UINT64 align = ph[i].align;
        UINTN j;

        if (ph[i].type != KIRA_PT_LOAD)
            continue;

        if (ph[i].memsz < ph[i].filesz ||
            !range_valid(ph[i].offset, ph[i].filesz, image_size) ||
            ph[i].memsz == 0) {
            Print(L"KIRA UEFI: PT_LOAD %u size/range rejected off=%lx filesz=%lx memsz=%lx\r\n",
                  (UINT32)i, ph[i].offset, ph[i].filesz, ph[i].memsz);
            goto fail;
        }

        destination = ph[i].paddr ? ph[i].paddr : ph[i].vaddr;
        if (destination != ph[i].vaddr ||
            destination < 0x100000 ||
            destination >= KIRA_BOOTSTRAP_LIMIT ||
            ph[i].memsz > KIRA_BOOTSTRAP_LIMIT - destination) {
            Print(L"KIRA UEFI: PT_LOAD %u address rejected v=%lx p=%lx memsz=%lx\r\n",
                  (UINT32)i, ph[i].vaddr, ph[i].paddr, ph[i].memsz);
            goto fail;
        }

        segment_end = destination + ph[i].memsz;
        if (align > 1 &&
            ((align & (align - 1)) != 0 ||
             (ph[i].offset & (align - 1)) !=
             (destination & (align - 1)))) {
            Print(L"KIRA UEFI: PT_LOAD %u alignment rejected off=%lx v=%lx align=%lx\r\n",
                  (UINT32)i, ph[i].offset, destination, align);
            goto fail;
        }

        for (j = 0; j < load_count; ++j) {
            UINT64 previous_end = load_segments[j].base +
                                  load_segments[j].size;
            if (destination < previous_end &&
                load_segments[j].base < segment_end) {
                Print(L"KIRA UEFI: PT_LOAD %u overlaps segment %u\r\n",
                      (UINT32)i, (UINT32)j);
                goto fail;
            }
        }

        load_segments[load_count].base = destination;
        load_segments[load_count].size = ph[i].memsz;
        ++load_count;

        if ((ph[i].flags & 1u) &&
            eh->entry >= destination && eh->entry < segment_end)
            entry_executable = TRUE;

        if (destination < min_address)
            min_address = destination;
        if (segment_end > max_address)
            max_address = segment_end;
    }

    if (load_count == 0) {
        Print(L"KIRA UEFI: ELF has no PT_LOAD segments\r\n");
        goto fail;
    }
    if (!entry_executable) {
        Print(L"KIRA UEFI: ELF entry %lx is not executable\r\n", eh->entry);
        goto fail;
    }
    if (max_address <= min_address || max_address > KIRA_BOOTSTRAP_LIMIT) {
        Print(L"KIRA UEFI: ELF span invalid min=%lx max=%lx\r\n", min_address, max_address);
        goto fail;
    }

    allocation_base = min_address & ~((EFI_PHYSICAL_ADDRESS)KIRA_PAGE_SIZE - 1);
    allocation_end = (max_address + KIRA_PAGE_SIZE - 1) &
                     ~((EFI_PHYSICAL_ADDRESS)KIRA_PAGE_SIZE - 1);
    if (allocation_end <= allocation_base ||
        allocation_end - allocation_base > KIRA_MAX_KERNEL_SPAN) {
        Print(L"KIRA UEFI: ELF allocation span invalid base=%lx end=%lx\r\n",
              allocation_base, allocation_end);
        goto fail;
    }

    allocation_pages = (UINTN)((allocation_end - allocation_base) /
                               KIRA_PAGE_SIZE);
    allocation = allocation_base;
    status = uefi_call_wrapper(g_bs->AllocatePages, 4, AllocateAddress,
                               EfiLoaderData, allocation_pages, &allocation);
    if (EFI_ERROR(status)) {
        print_error(L"cannot allocate kernel span", status);
        goto fail;
    }

    allocation_segment.base = allocation_base;
    allocation_segment.size = allocation_end - allocation_base;
    load_segments[0] = allocation_segment;
    allocation_count = 1;

    /* Clear gaps and BSS, then copy each validated file-backed segment. */
    SetMem((VOID *)(UINTN)allocation_base,
           (UINTN)(allocation_end - allocation_base), 0);
    for (i = 0; i < eh->phnum; ++i) {
        EFI_PHYSICAL_ADDRESS destination;
        if (ph[i].type != KIRA_PT_LOAD)
            continue;
        destination = ph[i].paddr ? ph[i].paddr : ph[i].vaddr;
        if (ph[i].filesz)
            CopyMem((VOID *)(UINTN)destination,
                    (UINT8 *)image + ph[i].offset,
                    (UINTN)ph[i].filesz);
    }

    *entry_out = eh->entry;
    *base_out = min_address;
    *size_out = max_address - min_address;
    return EFI_SUCCESS;

fail:
    if (allocation_count)
        release_loaded_segments(load_segments, allocation_count);
    if (!EFI_ERROR(status))
        status = EFI_LOAD_ERROR;
    return status;
}

static EFI_STATUS collect_gop(kira_uefi_kbi_info *info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;
    UINT32 pitch;
    UINT32 width, height, scanline;
    UINT64 base, size;
    UINT32 format;

    status = uefi_call_wrapper(g_bs->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info)
        return EFI_NOT_FOUND;

    switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        format = 1;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        format = 2;
        break;
    default:
        return EFI_NOT_FOUND;
    }

    width = gop->Mode->Info->HorizontalResolution;
    height = gop->Mode->Info->VerticalResolution;
    scanline = gop->Mode->Info->PixelsPerScanLine;
    base = gop->Mode->FrameBufferBase;
    size = gop->Mode->FrameBufferSize;
    /* Invalid firmware geometry must degrade to headless boot, not poison KBI. */
    if (!kira_uefi_gop_geometry_valid(base, size, width, height,
                                     scanline, format, &pitch))
        return EFI_NOT_FOUND;

    info->framebuffer_base = base;
    info->framebuffer_size = size;
    info->width = width;
    info->height = height;
    info->pitch = pitch;
    info->format = format;
    return EFI_SUCCESS;
}

/* CompareGuid из GNU-EFI возвращает 0 при РАВЕНСТВЕ (как memcmp): использовать его как логическое «равны» нельзя. */
static int guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    UINTN i;
    for (i = 0; i < sizeof(EFI_GUID); ++i)
        if (x[i] != y[i]) return 0;
    return 1;
}

static EFI_STATUS find_rsdp(UINT64 *address)
{
    UINTN i;

    *address = 0;
    for (i = 0; i < g_st->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *entry = &g_st->ConfigurationTable[i];

        if (guid_equal(&entry->VendorGuid, &kira_acpi20_table_guid) ||
            guid_equal(&entry->VendorGuid, &kira_acpi_table_guid)) {
            *address = (UINT64)(UINTN)entry->VendorTable;
            if (guid_equal(&entry->VendorGuid, &kira_acpi20_table_guid))
                return EFI_SUCCESS;
        }
    }

    return *address ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
    EFI_STATUS status;
    VOID *kernel_image = NULL;
    VOID *memory_map = NULL;
    UINTN kernel_image_size = 0;
    EFI_PHYSICAL_ADDRESS kernel_entry = 0;
    EFI_PHYSICAL_ADDRESS kernel_base = 0;
    UINT64 kernel_size = 0;
    EFI_PHYSICAL_ADDRESS bootinfo_physical = (1ULL << 30) - 1;
    kira_uefi_kbi_info kbi_info;
    UINT32 bootinfo_size = 0;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    UINTN attempt;

    SetMem(&kbi_info, sizeof(kbi_info), 0);
    InitializeLib(image, system_table);
    g_st = system_table;
    g_bs = system_table->BootServices;

    Print(L"KIRA OS UEFI loader\r\n");

    status = read_kernel_image(image, &kernel_image, &kernel_image_size);
    if (EFI_ERROR(status)) {
        print_error(L"cannot read kernel image", status);
        return status;
    }

    status = load_elf64(kernel_image, kernel_image_size,
                        &kernel_entry, &kernel_base, &kernel_size);
    uefi_call_wrapper(g_bs->FreePool, 1, kernel_image);
    if (EFI_ERROR(status)) {
        print_error(L"invalid or unloadable ELF64 kernel", status);
        return status;
    }

    /* Keep the handoff blob below the kernel's initial 1-GiB identity map. */
    status = uefi_call_wrapper(g_bs->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
                                 KIRA_BOOTINFO_PAGES, &bootinfo_physical);
    if (EFI_ERROR(status))
        return status;

    kbi_info.kernel_base = kernel_base;
    kbi_info.kernel_size = kernel_size;
    (VOID)collect_gop(&kbi_info);
    (VOID)find_rsdp(&kbi_info.rsdp);

    status = collect_command_line(image, &kbi_info);
    if (EFI_ERROR(status)) {
        print_error(L"unsupported UEFI command line", status);
        return status;
    }
    status = load_boot_module(image, &kbi_info);
    if (status != EFI_NOT_FOUND && EFI_ERROR(status)) {
        print_error(L"cannot load optional INITRD.IMG", status);
        return status;
    }

    /*
     * Allocate a generously sized memory-map buffer. If firmware reports
     * EFI_BUFFER_TOO_SMALL, grow it and retry before ExitBootServices.
     */
    map_size = 64 * 1024;
    status = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData, map_size, &memory_map);
    if (EFI_ERROR(status))
        return status;

    /* Console output is a Boot Services call that can allocate and so change the
       memory map. Print here, before GetMemoryMap: any output between GetMemoryMap
       and ExitBootServices makes the map key stale and ExitBootServices fails with
       EFI_INVALID_PARAMETER on every attempt. */
    Print(L"KIRA UEFI: handing off to kernel (BootInfo, ExitBootServices)\r\n");
    for (attempt = 0; attempt < 8; ++attempt) {
        UINTN current_size = map_size;

        status = uefi_call_wrapper(g_bs->GetMemoryMap, 5, &current_size,
                                    (EFI_MEMORY_DESCRIPTOR *)memory_map,
                                    &map_key, &descriptor_size,
                                    &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            uefi_call_wrapper(g_bs->FreePool, 1, memory_map);
            map_size = current_size + descriptor_size * 8;
            status = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData, map_size, &memory_map);
            if (EFI_ERROR(status))
                return status;
            continue;
        }
        if (EFI_ERROR(status))
            return status;

        /* Pure memory conversion only: do not call Boot Services between this
           GetMemoryMap and ExitBootServices, or the map key becomes stale. */
        status = kira_uefi_build_kbi(memory_map, current_size, descriptor_size,
                                     descriptor_version, &kbi_info,
                                     (VOID *)(UINTN)bootinfo_physical,
                                     KIRA_BOOTINFO_PAGES * KIRA_PAGE_SIZE,
                                     &bootinfo_size);
        if (EFI_ERROR(status)) {
            Print(L"KIRA UEFI: BootInfo rejected: status=%r reason=%d rsdp=0x%lx descriptors=%d\r\n",
                  status, kira_uefi_kbi_reject_reason(),
                  kbi_info.rsdp, (INTN)(current_size / descriptor_size));
            return status;
        }

        status = uefi_call_wrapper(g_bs->ExitBootServices, 2, image, map_key);
        if (!EFI_ERROR(status))
            break;

        /* Firmware changed the map between GetMemoryMap and ExitBootServices. */
    }

    if (EFI_ERROR(status))
        return status;

    /*
     * No firmware boot-service calls are permitted beyond this point.
     * start detects the UEFI long-mode entry and forwards KBI to the kernel
     * bridge using the native System V kernel ABI.
     */
#if defined(__x86_64__)
    {
        typedef void (__attribute__((ms_abi)) *kernel_entry_fn)(void *, UINTN);
        kernel_entry_fn entry = (kernel_entry_fn)(UINTN)kernel_entry;
        entry((VOID *)(UINTN)bootinfo_physical, bootinfo_size);
    }
#else
#error "This UEFI loader must be built for x86_64"
#endif

    for (;;)
        __asm__ __volatile__("cli; hlt");
    return EFI_SUCCESS;
}
