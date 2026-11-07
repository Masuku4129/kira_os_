/*
 * KIRA OS x86-64 UEFI application loader.
 *
 * Build as PE32+ EFI application with GNU-EFI or an equivalent UEFI ABI
 * toolchain. The loader reads \EFI\KIRA\KIRA.ELF from the EFI system
 * partition, validates ELF64 PT_LOAD segments, captures GOP and memory-map
 * information, exits boot services, and transfers control to the kernel.
 *
 * The kernel entry ABI is:
 *   RDI = pointer to kira_uefi_boot_info
 *   RSI = byte size of kira_uefi_boot_info
 */

#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>

#define KIRA_EFI_MAGIC       0x314942454152494bull
#define KIRA_ELF_MAGIC       0x464c457f
#define KIRA_ELFCLASS64      2
#define KIRA_ELFDATA2LSB     1
#define KIRA_ELF_ET_EXEC     2
#define KIRA_ELF_EM_X86_64   62
#define KIRA_PT_LOAD         1
#define KIRA_MAX_SEGMENTS    32
#define KIRA_PAGE_SIZE       4096
#define KIRA_KERNEL_PATH     L"\\EFI\\KIRA\\KIRA.ELF"
#define KIRA_BOOTINFO_PAGES  4

typedef struct {
    UINT32 type;
    UINT32 pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    EFI_VIRTUAL_ADDRESS virtual_start;
    UINT64 pages;
    UINT64 attributes;
} kira_efi_memory_descriptor;

typedef struct {
    UINT64 magic;
    UINT32 version;
    UINT32 size;
    UINT64 memory_map;
    UINT64 memory_map_size;
    UINT64 descriptor_size;
    UINT32 descriptor_version;
    UINT32 reserved;
    UINT64 framebuffer_base;
    UINT64 framebuffer_size;
    UINT32 framebuffer_width;
    UINT32 framebuffer_height;
    UINT32 framebuffer_stride;
    UINT32 framebuffer_format;
    UINT64 rsdp;
    UINT64 kernel_physical_base;
    UINT64 kernel_size;
} kira_uefi_boot_info;

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

    status = g_bs->HandleProtocol(image, &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(status))
        return status;

    status = g_bs->HandleProtocol(loaded->DeviceHandle,
                                  &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status))
        return status;

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status))
        return status;

    status = root->Open(root, &file, KIRA_KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        root->Close(root);
        return status;
    }

    status = file->GetInfo(file, &FileInfo, &info_size, NULL);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = g_bs->AllocatePool(EfiLoaderData, info_size, (VOID **)&info);
        if (!EFI_ERROR(status))
            status = file->GetInfo(file, &FileInfo, &info_size, info);
    }
    if (EFI_ERROR(status)) {
        if (info)
            g_bs->FreePool(info);
        file->Close(file);
        root->Close(root);
        return status;
    }

    if (info->FileSize == 0 || info->FileSize > 128ull * 1024ull * 1024ull) {
        g_bs->FreePool(info);
        file->Close(file);
        root->Close(root);
        return EFI_BAD_BUFFER_SIZE;
    }

    size = (UINTN)info->FileSize;
    g_bs->FreePool(info);

    status = g_bs->AllocatePool(EfiLoaderData, size, buffer);
    if (EFI_ERROR(status)) {
        file->Close(file);
        root->Close(root);
        return status;
    }

    *buffer_size = size;
    status = file->Read(file, buffer_size, *buffer);
    file->Close(file);
    root->Close(root);

    if (EFI_ERROR(status) || *buffer_size != size) {
        g_bs->FreePool(*buffer);
        *buffer = NULL;
        return EFI_ERROR(status) ? status : EFI_END_OF_FILE;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS load_elf64(VOID *image,
                             UINTN image_size,
                             EFI_PHYSICAL_ADDRESS *entry_out,
                             EFI_PHYSICAL_ADDRESS *base_out,
                             UINT64 *size_out)
{
    kira_elf64_header *eh = (kira_elf64_header *)image;
    kira_elf64_program_header *ph;
    kira_loaded_segment segments[KIRA_MAX_SEGMENTS];
    UINTN segment_count = 0;
    UINTN i;
    EFI_PHYSICAL_ADDRESS min_address = ~(EFI_PHYSICAL_ADDRESS)0;
    EFI_PHYSICAL_ADDRESS max_address = 0;

    if (image_size < sizeof(*eh) ||
        *(UINT32 *)eh->ident != KIRA_ELF_MAGIC ||
        eh->ident[4] != KIRA_ELFCLASS64 ||
        eh->ident[5] != KIRA_ELFDATA2LSB ||
        eh->type != KIRA_ELF_ET_EXEC ||
        eh->machine != KIRA_ELF_EM_X86_64 ||
        eh->version != 1 ||
        eh->ehsize != sizeof(*eh) ||
        eh->phentsize != sizeof(kira_elf64_program_header) ||
        eh->phnum == 0 || eh->phnum > KIRA_MAX_SEGMENTS)
        return EFI_LOAD_ERROR;

    if (!range_valid(eh->phoff,
                     (UINT64)eh->phnum * eh->phentsize,
                     image_size))
        return EFI_LOAD_ERROR;

    ph = (kira_elf64_program_header *)((UINT8 *)image + eh->phoff);

    for (i = 0; i < eh->phnum; ++i) {
        EFI_PHYSICAL_ADDRESS destination;
        EFI_PHYSICAL_ADDRESS allocation;
        UINTN pages;
        EFI_STATUS status;

        if (ph[i].type != KIRA_PT_LOAD)
            continue;

        if (ph[i].memsz < ph[i].filesz ||
            !range_valid(ph[i].offset, ph[i].filesz, image_size) ||
            ph[i].memsz == 0 ||
            ph[i].paddr > 0x000fffffffffffull ||
            ph[i].memsz > 0x000fffffffffffull - ph[i].paddr)
            return EFI_LOAD_ERROR;

        destination = ph[i].paddr ? ph[i].paddr : ph[i].vaddr;
        if (destination < 0x100000 ||
            destination > 0x000fffffffffffull ||
            ph[i].memsz > 0x000fffffffffffull - destination)
            return EFI_LOAD_ERROR;

        pages = (UINTN)((ph[i].memsz + KIRA_PAGE_SIZE - 1) / KIRA_PAGE_SIZE);
        allocation = destination;
        status = g_bs->AllocatePages(AllocateAddress, EfiLoaderData,
                                     pages, &allocation);
        if (EFI_ERROR(status))
            return status;

        if (ph[i].filesz)
            CopyMem((VOID *)(UINTN)destination,
                    (UINT8 *)image + ph[i].offset,
                    (UINTN)ph[i].filesz);
        if (ph[i].memsz > ph[i].filesz)
            SetMem((UINT8 *)(UINTN)destination + ph[i].filesz,
                   (UINTN)(ph[i].memsz - ph[i].filesz), 0);

        segments[segment_count].base = destination;
        segments[segment_count].size = ph[i].memsz;
        ++segment_count;

        if (destination < min_address)
            min_address = destination;
        if (destination + ph[i].memsz > max_address)
            max_address = destination + ph[i].memsz;
    }

    if (segment_count == 0 || eh->entry < min_address || eh->entry >= max_address)
        return EFI_LOAD_ERROR;

    *entry_out = eh->entry;
    *base_out = min_address;
    *size_out = max_address - min_address;
    return EFI_SUCCESS;
}

static EFI_STATUS collect_gop(kira_uefi_boot_info *info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    status = g_bs->LocateProtocol(&GraphicsOutputProtocol, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info)
        return EFI_NOT_FOUND;

    info->framebuffer_base = gop->Mode->FrameBufferBase;
    info->framebuffer_size = gop->Mode->FrameBufferSize;
    info->framebuffer_width = gop->Mode->Info->HorizontalResolution;
    info->framebuffer_height = gop->Mode->Info->VerticalResolution;
    info->framebuffer_stride = gop->Mode->Info->PixelsPerScanLine;

    switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        info->framebuffer_format = 1;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        info->framebuffer_format = 2;
        break;
    default:
        info->framebuffer_format = 0;
        break;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS find_rsdp(UINT64 *address)
{
    UINTN i;

    *address = 0;
    for (i = 0; i < g_st->NumberOfConfigurationTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *entry = &g_st->ConfigurationTable[i];

        if (CompareGuid(&entry->VendorGuid, &Acpi20TableGuid) ||
            CompareGuid(&entry->VendorGuid, &AcpiTableGuid)) {
            *address = (UINT64)(UINTN)entry->VendorTable;
            if (CompareGuid(&entry->VendorGuid, &Acpi20TableGuid))
                return EFI_SUCCESS;
        }
    }

    return *address ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
    EFI_STATUS status;
    VOID *kernel_image = NULL;
    UINTN kernel_image_size = 0;
    EFI_PHYSICAL_ADDRESS kernel_entry = 0;
    EFI_PHYSICAL_ADDRESS kernel_base = 0;
    UINT64 kernel_size = 0;
    EFI_PHYSICAL_ADDRESS bootinfo_physical = 0;
    kira_uefi_boot_info *bootinfo = NULL;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    UINTN map_pages;
    UINTN attempt;

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
    g_bs->FreePool(kernel_image);
    if (EFI_ERROR(status)) {
        print_error(L"invalid or unloadable ELF64 kernel", status);
        return status;
    }

    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                 KIRA_BOOTINFO_PAGES, &bootinfo_physical);
    if (EFI_ERROR(status))
        return status;

    bootinfo = (kira_uefi_boot_info *)(UINTN)bootinfo_physical;
    SetMem(bootinfo, KIRA_BOOTINFO_PAGES * KIRA_PAGE_SIZE, 0);
    bootinfo->magic = KIRA_EFI_MAGIC;
    bootinfo->version = 1;
    bootinfo->size = sizeof(*bootinfo);
    bootinfo->kernel_physical_base = kernel_base;
    bootinfo->kernel_size = kernel_size;

    (VOID)collect_gop(bootinfo);
    (VOID)find_rsdp(&bootinfo->rsdp);

    /*
     * Allocate a generously sized memory-map buffer. If firmware reports
     * EFI_BUFFER_TOO_SMALL, grow it and retry before ExitBootServices.
     */
    map_size = 64 * 1024;
    status = g_bs->AllocatePool(EfiLoaderData, map_size,
                                (VOID **)&bootinfo->memory_map);
    if (EFI_ERROR(status))
        return status;

    for (attempt = 0; attempt < 8; ++attempt) {
        UINTN current_size = map_size;

        status = g_bs->GetMemoryMap(&current_size,
                                    (EFI_MEMORY_DESCRIPTOR *)(UINTN)bootinfo->memory_map,
                                    &map_key, &descriptor_size,
                                    &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            g_bs->FreePool((VOID *)(UINTN)bootinfo->memory_map);
            map_size = current_size + descriptor_size * 8;
            status = g_bs->AllocatePool(EfiLoaderData, map_size,
                                        (VOID **)&bootinfo->memory_map);
            if (EFI_ERROR(status))
                return status;
            continue;
        }
        if (EFI_ERROR(status))
            return status;

        bootinfo->memory_map_size = current_size;
        bootinfo->descriptor_size = descriptor_size;
        bootinfo->descriptor_version = descriptor_version;

        status = g_bs->ExitBootServices(image, map_key);
        if (!EFI_ERROR(status))
            break;

        /* Firmware changed the map between GetMemoryMap and ExitBootServices. */
    }

    if (EFI_ERROR(status))
        return status;

    /*
     * No firmware boot-service calls are permitted beyond this point.
     * Kernel must establish its own page tables and runtime environment.
     */
#if defined(__x86_64__)
    {
        typedef void (__attribute__((ms_abi)) *kernel_entry_fn)(
            kira_uefi_boot_info *, UINTN);
        kernel_entry_fn entry = (kernel_entry_fn)(UINTN)kernel_entry;
        entry(bootinfo, sizeof(*bootinfo));
    }
#else
#error "This UEFI loader must be built for x86_64"
#endif

    for (;;)
        __asm__ __volatile__("cli; hlt");
    return EFI_SUCCESS;
}
