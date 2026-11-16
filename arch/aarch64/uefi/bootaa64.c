/*
 * Kira OS, ARM64: собственный UEFI-загрузчик (EFI-приложение BOOTAA64.EFI), без GRUB и без gnu-efi.
 *
 * Что делает (по шагам, каждый печатается в консоль прошивки, то есть в последовательный порт):
 *   1. Открывает раздел, с которого запущен сам (ESP на ISO или диске), и читает \EFI\KIRA\KIRA-ARM64.IMG -
 *      тот же образ kira-arm64.img (заголовок образа ARM64 + позиционно-независимое ядро), что запускает QEMU -kernel.
 *   2. Проверяет заголовок образа (магия "ARM\x64", размер), выделяет память, выровненную на 2 МБ,
 *      кладёт туда образ, а сразу за ним (в той же выделенной области, куда ядро не лезет) - копию дерева устройств.
 *   3. Берёт дерево устройств у прошивки (таблица конфигурации EFI_DTB_TABLE_GUID; QEMU virt + edk2 её публикует).
 *   4. Получает карту памяти UEFI и вызывает ExitBootServices (между GetMemoryMap и ExitBootServices ничего,
 *      что трогает службы загрузки, не вызывается - ключ карты иначе устареет).
 *   5. Передаёт управление ядру по протоколу загрузки ARM64: MMU и кэш данных выключены, кэш очищен до точки
 *      согласованности, x0 = физический адрес дерева устройств, x1..x3 = 0.
 *
 * Метка загрузчика: в зарезервированное поле заголовка образа (смещение 40) записывается "KIRAUEFI",
 * ядро печатает по ней «загружено через UEFI».
 *
 * Сообщения - ASCII по-английски: консоль прошивки на последовательном порту не выводит кириллицу (получаются вопросительные знаки).
 *
 * Сборка: build_arm64.sh (clang --target=aarch64-unknown-windows + lld-link). Свои описания типов UEFI
 * ниже: заголовков gnu-efi для ARM64 на машине сборки нет, а нужен лишь малый набор.
 * Старый arch/aarch64/uefi/loader.c (ELF + kira_bootinfo, gnu-efi) оставлен как есть: он описывает другой
 * способ передачи (структура вместо дерева) и ни разу не собирался.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 UINTN;
typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16 CHAR16;
typedef struct { u32 a; u16 b, c; u8 d[8]; } EFI_GUID;

#define EFIERR(x) (0x8000000000000000ull | (x))
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR EFIERR(1)
#define EFI_BUFFER_TOO_SMALL EFIERR(5)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ERR(s) ((s) & 0x8000000000000000ull)

typedef struct { u64 sig; u32 rev, hsize, crc, res; } EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUT {
    void *Reset;
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUT *, const CHAR16 *);
} EFI_SIMPLE_TEXT_OUT;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (*AllocatePages)(u32 type, u32 memtype, UINTN pages, u64 *addr);
    EFI_STATUS (*FreePages)(u64 addr, UINTN pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *size, void *map, UINTN *key, UINTN *dsize, u32 *dver);
    EFI_STATUS (*AllocatePool)(u32 memtype, UINTN size, void **buf);
    EFI_STATUS (*FreePool)(void *buf);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, UINTN key);
    void *GetNextMonotonicCount, *Stall;
    EFI_STATUS (*SetWatchdogTimer)(UINTN timeout, u64 code, UINTN size, const CHAR16 *data);
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID guid; void *table; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor; u32 FirmwareRevision, pad;
    EFI_HANDLE ConsoleInHandle; void *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUT *ConOut;
    EFI_HANDLE StandardErrorHandle; EFI_SIMPLE_TEXT_OUT *StdErr;
    void *RuntimeServices; EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    u32 Revision; u32 pad; EFI_HANDLE ParentHandle; EFI_SYSTEM_TABLE *SystemTable; EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE;

typedef struct EFI_FILE {
    u64 Revision;
    EFI_STATUS (*Open)(struct EFI_FILE *, struct EFI_FILE **, const CHAR16 *, u64 mode, u64 attr);
    EFI_STATUS (*Close)(struct EFI_FILE *);
    void *Delete;
    EFI_STATUS (*Read)(struct EFI_FILE *, UINTN *size, void *buf);
    void *Write, *GetPosition, *SetPosition;
    EFI_STATUS (*GetInfo)(struct EFI_FILE *, const EFI_GUID *, UINTN *size, void *buf);
} EFI_FILE;

typedef struct EFI_SFS {
    u64 Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SFS *, EFI_FILE **);
} EFI_SFS;

static const EFI_GUID g_loaded_image = { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID g_sfs = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID g_file_info = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID g_dtb = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

#define EfiLoaderData 2
#define AllocateAnyPages 0
#define PAGE 4096ull
#define KERNEL_PATH L"\\EFI\\KIRA\\KIRA-ARM64.IMG"

/* Магия образа ARM64 (смещение 56) и поля: image_size (16), flags (24), метка загрузчика (40) - см. arch/aarch64/boot.S */
#define IMG_MAGIC 0x644d5241u        /* "ARM\x64" */

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;

/* Своя память: компилятор без libc может вызвать memcpy/memset сам */
void *memcpy(void *d, const void *s, UINTN n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, UINTN n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }

static void out(const char *s) {
    CHAR16 buf[160]; UINTN n = 0;
    while (*s && n < 156) { if (*s == '\n') buf[n++] = '\r'; buf[n++] = (CHAR16)(u8)*s++; }
    buf[n] = 0;
    st->ConOut->OutputString(st->ConOut, buf);
}
static void out_hex(u64 v) {
    char b[19] = "0x0000000000000000"; int i;
    for (i = 0; i < 16; i++) b[17 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    b[18] = 0; out(b);
}
static void out_dec(u64 v) {
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}
static int fail(const char *msg, EFI_STATUS rc) {
    out("[KIRA UEFI] error: "); out(msg); out(", status "); out_hex(rc); out("\n");
    return 1;
}

/* Последние шаги - только на регистрах, без стека и без обращений к данным: после выключения MMU любой
   невыровненный доступ к памяти устройства (так видна вся память при выключенной MMU) - сбой.
   x0 = точка входа, x1 = адрес дерева устройств. Очистка кэша - по уровням/наборам/путям (как в Linux и edk2). */
__attribute__((noreturn)) void kira_enter_kernel(u64 entry, u64 fdt);
__asm__(
    ".text\n.balign 64\n.global kira_enter_kernel\nkira_enter_kernel:\n"
    "   mov     x20, x0\n   mov     x21, x1\n"
    "   msr     daifset, #0xf\n"
    "   mrs     x3, CurrentEL\n   lsr     x3, x3, #2\n   cmp     x3, #2\n   b.eq    1f\n"
    /* EL1: выключить кэш данных */
    "   mrs     x4, sctlr_el1\n   bic     x4, x4, #(1 << 2)\n   msr     sctlr_el1, x4\n   b       2f\n"
    "1: mrs     x4, sctlr_el2\n   bic     x4, x4, #(1 << 2)\n   msr     sctlr_el2, x4\n"
    "2: isb\n   dsb     sy\n"
    /* очистка и сброс всего кэша данных по наборам/путям (Linux: __flush_dcache_all) */
    "   mrs     x0, clidr_el1\n   and     w3, w0, #0x07000000\n   lsr     w3, w3, #23\n   cbz     w3, 9f\n   mov     w10, #0\n"
    "3: add     w2, w10, w10, lsr #1\n   lsr     w1, w0, w2\n   and     w1, w1, #7\n   cmp     w1, #2\n   b.lt    8f\n"
    "   msr     csselr_el1, x10\n   isb\n   mrs     x1, ccsidr_el1\n   and     w2, w1, #7\n   add     w2, w2, #4\n"
    "   mov     w4, #0x3ff\n   and     w4, w4, w1, lsr #3\n   clz     w5, w4\n   mov     w7, #0x7fff\n   and     w7, w7, w1, lsr #13\n"
    "4: mov     w9, w4\n"
    "5: lsl     w6, w9, w5\n   orr     w11, w10, w6\n   lsl     w6, w7, w2\n   orr     w11, w11, w6\n   dc      cisw, x11\n"
    "   subs    w9, w9, #1\n   b.ge    5b\n   subs    w7, w7, #1\n   b.ge    4b\n"
    "8: add     w10, w10, #2\n   cmp     w3, w10\n   b.gt    3b\n"
    "9: dsb     sy\n   isb\n"
    /* выключить MMU, сбросить кэш команд */
    "   mrs     x3, CurrentEL\n   lsr     x3, x3, #2\n   cmp     x3, #2\n   b.eq    6f\n"
    "   mrs     x4, sctlr_el1\n   bic     x4, x4, #1\n   msr     sctlr_el1, x4\n   b       7f\n"
    "6: mrs     x4, sctlr_el2\n   bic     x4, x4, #1\n   msr     sctlr_el2, x4\n"
    "7: isb\n   ic      iallu\n   dsb     nsh\n   isb\n"
    "   mov     x0, x21\n   mov     x1, xzr\n   mov     x2, xzr\n   mov     x3, xzr\n   br      x20\n");

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 le64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    EFI_STATUS rc;
    EFI_LOADED_IMAGE *li = 0; EFI_SFS *fs = 0; EFI_FILE *root = 0, *file = 0;
    u8 info[512]; UINTN isz = sizeof info, fsize;
    st = system_table; bs = st->BootServices;
    bs->SetWatchdogTimer(0, 0, 0, 0);                 /* 5-минутный сторожевой таймер прошивки не должен перезагрузить нас */
    out("\n[KIRA UEFI] Kira OS ARM64 loader (UEFI, no GRUB)\n");

    rc = bs->HandleProtocol(image, &g_loaded_image, (void **)&li);
    if (EFI_ERR(rc)) return fail("no LoadedImage", rc), rc;
    rc = bs->HandleProtocol(li->DeviceHandle, &g_sfs, (void **)&fs);
    if (EFI_ERR(rc)) return fail("cannot read files from the boot medium (no SimpleFileSystem)", rc), rc;
    rc = fs->OpenVolume(fs, &root);
    if (EFI_ERR(rc)) return fail("cannot open the volume", rc), rc;
    rc = root->Open(root, &file, KERNEL_PATH, 1 /* READ */, 0);
    if (EFI_ERR(rc)) return fail("file \\EFI\\KIRA\\KIRA-ARM64.IMG not found", rc), rc;
    rc = file->GetInfo(file, &g_file_info, &isz, info);
    if (EFI_ERR(rc)) return fail("GetInfo", rc), rc;
    fsize = (UINTN)le64(info + 8);                      /* EFI_FILE_INFO.FileSize */
    if (fsize < 64 || fsize > (256ull << 20)) return fail("invalid image size", EFI_LOAD_ERROR), EFI_LOAD_ERROR;

    /* Заголовок: читаем 64 байта, проверяем, затем выделяем место под image_size (он больше файла: там .bss) */
    u8 hdr[64]; UINTN hn = sizeof hdr;
    rc = file->Read(file, &hn, hdr);
    if (EFI_ERR(rc) || hn != sizeof hdr) return fail("cannot read the image header", rc), EFI_LOAD_ERROR;
    if (le32(hdr + 56) != IMG_MAGIC) return fail("image has no ARM64 header (magic ARM\\x64)", EFI_LOAD_ERROR), EFI_LOAD_ERROR;
    u64 image_size = le64(hdr + 16);
    if (image_size < fsize || image_size > (1ull << 30)) return fail("image_size is smaller than the file or too large", EFI_LOAD_ERROR), EFI_LOAD_ERROR;

    /* Дерево устройств от прошивки */
    const u8 *fdt = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++)
        if (guid_eq(&st->ConfigurationTable[i].guid, &g_dtb)) { fdt = st->ConfigurationTable[i].table; break; }
    if (!fdt) {
        out("[KIRA UEFI] configuration tables of the firmware:");
        for (UINTN i = 0; i < st->NumberOfTableEntries; i++) { out(" "); out_hex(st->ConfigurationTable[i].guid.a); }
        out("\n");
        return fail("firmware gave no device tree (EFI_DTB_TABLE); with ACPI-only firmware start QEMU with -machine virt,acpi=off", EFI_NOT_FOUND), EFI_NOT_FOUND;
    }
    if (be32(fdt) != 0xd00dfeedu) return fail("device tree has no magic", EFI_LOAD_ERROR), EFI_LOAD_ERROR;
    u64 fdt_size = be32(fdt + 4);
    if (fdt_size < 40 || fdt_size > (4ull << 20)) return fail("device tree size is invalid", EFI_LOAD_ERROR), EFI_LOAD_ERROR;

    /* Память: [образ, 2 МБ-выровнен][дерево устройств]; всё - одна выделенная область */
    u64 img_rounded = (image_size + 4095) & ~4095ull;
    u64 total = img_rounded + ((fdt_size + 4095) & ~4095ull);
    u64 raw = 0, base;
    rc = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, (total + (2ull << 20)) / PAGE, &raw);
    if (EFI_ERR(rc)) return fail("not enough memory for the image", rc), rc;
    base = (raw + (2ull << 20) - 1) & ~((2ull << 20) - 1);
    memset((void *)base, 0, (UINTN)total);
    memcpy((void *)base, hdr, 64);
    UINTN rest = fsize - 64;
    rc = file->Read(file, &rest, (void *)(base + 64));
    if (EFI_ERR(rc) || rest != fsize - 64) return fail("cannot read the image", rc), EFI_LOAD_ERROR;
    file->Close(file); root->Close(root);
    u64 fdt_dst = base + img_rounded;
    memcpy((void *)fdt_dst, fdt, (UINTN)fdt_size);
    memcpy((void *)(base + 40), "KIRAUEFI", 8);        /* метка загрузчика в зарезервированном поле заголовка */

    out("[KIRA UEFI] kernel image: "); out_dec(fsize); out(" bytes, "); out_dec(image_size); out(" in memory, at "); out_hex(base);
    out("\n[KIRA UEFI] device tree from firmware: "); out_dec(fdt_size); out(" bytes, copy at "); out_hex(fdt_dst); out("\n");

    /* Карта памяти -> ExitBootServices. Между ними служб загрузки не трогать (в том числе вывод). */
    UINTN map_size = 0, key = 0, dsize = 0; u32 dver = 0; void *map = 0; int tries;
    rc = bs->GetMemoryMap(&map_size, 0, &key, &dsize, &dver);
    for (tries = 0; tries < 8; tries++) {
        if (map) bs->FreePool(map);
        map_size += 8 * dsize + 4096;
        rc = bs->AllocatePool(EfiLoaderData, map_size, &map);
        if (EFI_ERR(rc)) return fail("memory for the memory map", rc), rc;
        UINTN cur = map_size;
        rc = bs->GetMemoryMap(&cur, map, &key, &dsize, &dver);
        if (rc == EFI_BUFFER_TOO_SMALL) { map_size = cur; continue; }
        if (EFI_ERR(rc)) return fail("GetMemoryMap", rc), rc;
        rc = bs->ExitBootServices(image, key);
        if (!EFI_ERR(rc)) break;
    }
    if (EFI_ERR(rc)) return rc;                         /* службы загрузки ещё живы только если не вышли; печать уже недоступна */
    kira_enter_kernel(base + 0 /* вход - начало образа */, fdt_dst);
}
