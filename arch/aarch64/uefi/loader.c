/*
 * AArch64 UEFI ELF64 loader for KIRA OS.
 * Build as an AArch64 PE/COFF EFI application. Kernel entry receives x0=boot
 * information pointer and x1=structure size under the AAPCS64 ABI.
 */
#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
#include "kira/bootinfo.h"

#define ELF_MAGIC 0x464c457f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_AARCH64 183
#define PT_LOAD 1
#define PAGE 4096u
#define MAX_PHDR 64u
#define KERNEL_PATH L"\\EFI\\KIRA\\KIRA-ARM64.ELF"

typedef struct __attribute__((packed)) {
 UINT8 ident[16];UINT16 type,machine;UINT32 version;UINT64 entry,phoff,shoff;
 UINT32 flags;UINT16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx;
} elf_header;
typedef struct __attribute__((packed)) {
 UINT32 type,flags;UINT64 offset,vaddr,paddr,filesz,memsz,align;
} elf_phdr;

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;

static int range_ok(UINT64 off,UINT64 len,UINT64 total)
{return off<=total&&len<=total-off;}

static EFI_STATUS load_file(EFI_HANDLE image,VOID **data,UINTN *size)
{
 EFI_LOADED_IMAGE *li=0;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;
 EFI_FILE_PROTOCOL *root=0,*file=0;EFI_FILE_INFO *info=0;
 EFI_STATUS rc;UINTN isz=0;
 rc=bs->HandleProtocol(image,&LoadedImageProtocol,(VOID **)&li);if(EFI_ERROR(rc))return rc;
 rc=bs->HandleProtocol(li->DeviceHandle,&FileSystemProtocol,(VOID **)&fs);if(EFI_ERROR(rc))return rc;
 rc=fs->OpenVolume(fs,&root);if(EFI_ERROR(rc))return rc;
 rc=root->Open(root,&file,KERNEL_PATH,EFI_FILE_MODE_READ,0);
 if(EFI_ERROR(rc)){root->Close(root);return rc;}
 rc=file->GetInfo(file,&FileInfo,&isz,0);
 if(rc==EFI_BUFFER_TOO_SMALL){rc=bs->AllocatePool(EfiLoaderData,isz,(VOID **)&info);
  if(!EFI_ERROR(rc))rc=file->GetInfo(file,&FileInfo,&isz,info);}
 if(EFI_ERROR(rc)){if(info)bs->FreePool(info);file->Close(file);root->Close(root);return rc;}
 if(!info->FileSize||info->FileSize>128ull*1024*1024){
  bs->FreePool(info);file->Close(file);root->Close(root);return EFI_BAD_BUFFER_SIZE;}
 *size=(UINTN)info->FileSize;bs->FreePool(info);
 rc=bs->AllocatePool(EfiLoaderData,*size,data);
 if(!EFI_ERROR(rc))rc=file->Read(file,size,*data);
 file->Close(file);root->Close(root);return rc;
}

static EFI_STATUS load_elf(void *data,UINTN size,EFI_PHYSICAL_ADDRESS *entry,
                           EFI_PHYSICAL_ADDRESS *base,UINT64 *span)
{
 elf_header *eh=(elf_header *)data;elf_phdr *ph;UINTN i;
 EFI_PHYSICAL_ADDRESS low=~(EFI_PHYSICAL_ADDRESS)0,high=0;
 if(size<sizeof(*eh)||*(UINT32 *)eh->ident!=ELF_MAGIC||
  eh->ident[4]!=ELFCLASS64||eh->ident[5]!=ELFDATA2LSB||
  eh->type!=ET_EXEC||eh->machine!=EM_AARCH64||eh->version!=1||
  eh->ehsize!=sizeof(*eh)||eh->phentsize!=sizeof(elf_phdr)||
  !eh->phnum||eh->phnum>MAX_PHDR||
  !range_ok(eh->phoff,(UINT64)eh->phnum*sizeof(elf_phdr),size))return EFI_LOAD_ERROR;
 ph=(elf_phdr *)((UINT8 *)data+eh->phoff);
 for(i=0;i<eh->phnum;i++){
  EFI_PHYSICAL_ADDRESS dst,alloc;UINTN pages;EFI_STATUS rc;
  if(ph[i].type!=PT_LOAD)continue;
  if(!ph[i].memsz||ph[i].memsz<ph[i].filesz||
     !range_ok(ph[i].offset,ph[i].filesz,size))return EFI_LOAD_ERROR;
  dst=ph[i].paddr?ph[i].paddr:ph[i].vaddr;
  if(dst<0x400000||dst>0x0000ffffffffffffull||
     ph[i].memsz>0x0000ffffffffffffull-dst)return EFI_LOAD_ERROR;
  pages=(UINTN)((ph[i].memsz+PAGE-1)/PAGE);alloc=dst;
  rc=bs->AllocatePages(AllocateAddress,EfiLoaderData,pages,&alloc);
  if(EFI_ERROR(rc))return rc;
  CopyMem((VOID *)(UINTN)dst,(UINT8 *)data+ph[i].offset,(UINTN)ph[i].filesz);
  if(ph[i].memsz>ph[i].filesz)
   SetMem((UINT8 *)(UINTN)(dst+ph[i].filesz),(UINTN)(ph[i].memsz-ph[i].filesz),0);
  if(dst<low)low=dst;if(dst+ph[i].memsz>high)high=dst+ph[i].memsz;
 }
 if(low==~(EFI_PHYSICAL_ADDRESS)0||eh->entry<low||eh->entry>=high)return EFI_LOAD_ERROR;
 *entry=eh->entry;*base=low;*span=high-low;return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system_table)
{
 EFI_STATUS rc;VOID *file=0;UINTN file_size=0;
 EFI_PHYSICAL_ADDRESS entry=0,kbase=0,bi_pa=0;
 UINT64 ksize=0;struct kira_bootinfo *bi;
 UINTN map_size=128*1024,map_key=0,desc_size=0,attempt;
 UINT32 desc_version=0;VOID *map=0;
 InitializeLib(image,system_table);st=system_table;bs=st->BootServices;
 rc=load_file(image,&file,&file_size);if(EFI_ERROR(rc))return rc;
 rc=load_elf(file,file_size,&entry,&kbase,&ksize);bs->FreePool(file);
 if(EFI_ERROR(rc))return rc;
 rc=bs->AllocatePages(AllocateAnyPages,EfiLoaderData,2,&bi_pa);if(EFI_ERROR(rc))return rc;
 bi=(struct kira_bootinfo *)(UINTN)bi_pa;SetMem(bi,2*PAGE,0);
 bi->magic=KIRA_BOOTINFO_MAGIC;bi->version=KIRA_BOOTINFO_VERSION;
 bi->structure_size=sizeof(*bi);bi->boot_source=KIRA_BOOT_UEFI;
 bi->kernel_physical_base=kbase;bi->kernel_image_size=ksize;
 {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=0;
  if(!EFI_ERROR(bs->LocateProtocol(&GraphicsOutputProtocol,0,(VOID **)&gop))&&gop&&gop->Mode&&gop->Mode->Info){
   bi->framebuffer.address=gop->Mode->FrameBufferBase;
   bi->framebuffer.byte_size=gop->Mode->FrameBufferSize;
   bi->framebuffer.width=gop->Mode->Info->HorizontalResolution;
   bi->framebuffer.height=gop->Mode->Info->VerticalResolution;
   bi->framebuffer.pixels_per_scanline=gop->Mode->Info->PixelsPerScanLine;
   if(gop->Mode->Info->PixelFormat==PixelRedGreenBlueReserved8BitPerColor)
    bi->framebuffer.format=KIRA_FB_RGBX8;
   else if(gop->Mode->Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor)
    bi->framebuffer.format=KIRA_FB_BGRX8;
  }
 }
 {
  UINTN i;
  for(i=0;i<st->NumberOfConfigurationTableEntries;i++){
   EFI_CONFIGURATION_TABLE *t=&st->ConfigurationTable[i];
   if(CompareGuid(&t->VendorGuid,&Acpi20TableGuid)){bi->rsdp_address=(UINT64)(UINTN)t->VendorTable;break;}
   if(CompareGuid(&t->VendorGuid,&AcpiTableGuid))bi->rsdp_address=(UINT64)(UINTN)t->VendorTable;
  }
 }
 rc=bs->AllocatePool(EfiLoaderData,map_size,&map);if(EFI_ERROR(rc))return rc;
 for(attempt=0;attempt<8;attempt++){
  UINTN current=map_size;
  rc=bs->GetMemoryMap(&current,map,&map_key,&desc_size,&desc_version);
  if(rc==EFI_BUFFER_TOO_SMALL){
   bs->FreePool(map);map_size=current+desc_size*8;
   rc=bs->AllocatePool(EfiLoaderData,map_size,&map);if(EFI_ERROR(rc))return rc;continue;
  }
  if(EFI_ERROR(rc))return rc;
  bi->memory.address=(UINT64)(UINTN)map;
  bi->memory.byte_size=current;bi->memory.descriptor_size=desc_size;
  bi->memory.descriptor_version=desc_version;
  rc=bs->ExitBootServices(image,map_key);
  if(!EFI_ERROR(rc))break;
 }
 if(EFI_ERROR(rc))return rc;
 __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
 ((void (*)(struct kira_bootinfo *,UINT64))(UINTN)entry)(bi,sizeof(*bi));
 for(;;)__asm__ __volatile__("wfe");
 return EFI_SUCCESS;
}
