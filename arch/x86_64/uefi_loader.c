/* KIRA native UEFI loader.
 * Loads the identity-linked x86-64 ELF kernel, synthesizes the versioned KIRA
 * BootInfo handoff from UEFI services, exits boot services, then transfers
 * control to the kernel's UEFI long-mode entry shim.
 */
#include <stdint.h>
#include <stddef.h>
#include "../../bootinfo.h"

#define EFIAPI __attribute__((ms_abi))
#define EFI_ERROR(s) (((int64_t)(s)) < 0)
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER ((EFI_STATUS)0x8000000000000002ull)
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ull)
#define EFI_LOADER_CODE 1u
#define EFI_LOADER_DATA 2u
#define EFI_BOOT_SERVICES_DATA 4u
#define EFI_ALLOCATE_ADDRESS 2u
#define EFI_ALLOCATE_MAX_ADDRESS 1u
#define EFI_FILE_MODE_READ 1ull
#define EFI_PAGE_SIZE 4096u
#define EFI_MEMORY_RP 0x0000000000002000ull
#define EFI_MEMORY_XP 0x0000000000004000ull
#define EFI_MEMORY_RO 0x0000000000020000ull
#define BOOTINFO_MAX (256u * 1024u)
#define KERNEL_MAX (64u * 1024u * 1024u)
#define ONE_GB 0x40000000ull

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } EFI_GUID;
typedef struct { uint64_t signature; uint32_t revision, header_size, crc32, reserved; } EFI_TABLE_HEADER;
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_FILE EFI_FILE;
typedef struct EFI_MEMORY_ATTRIBUTE_PROTOCOL EFI_MEMORY_ATTRIBUTE_PROTOCOL;

struct EFI_MEMORY_ATTRIBUTE_PROTOCOL {
    EFI_STATUS (EFIAPI *get_memory_attributes)(EFI_MEMORY_ATTRIBUTE_PROTOCOL *,uint64_t,uint64_t,uint64_t *);
    EFI_STATUS (EFIAPI *set_memory_attributes)(EFI_MEMORY_ATTRIBUTE_PROTOCOL *,uint64_t,uint64_t,uint64_t);
    EFI_STATUS (EFIAPI *clear_memory_attributes)(EFI_MEMORY_ATTRIBUTE_PROTOCOL *,uint64_t,uint64_t,uint64_t);
};

typedef struct {
    uint32_t type, pad;
    uint64_t physical_start, virtual_start, pages, attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t version, horizontal_resolution, vertical_resolution;
    uint32_t pixel_format;
    EFI_PIXEL_BITMASK pixel_information;
    uint32_t pixels_per_scanline;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t max_mode, mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t size_of_info, framebuffer_base, framebuffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *query_mode, *set_mode, *blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    uint32_t revision;
    EFI_STATUS (EFIAPI *open_volume)(void *self, EFI_FILE **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_FILE {
    uint64_t revision;
    EFI_STATUS (EFIAPI *open)(EFI_FILE *, EFI_FILE **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *close)(EFI_FILE *);
    void *delete_file;
    EFI_STATUS (EFIAPI *read)(EFI_FILE *, uint64_t *, void *);
    void *write, *get_position, *set_position, *get_info, *set_info, *flush;
};

typedef struct {
    uint32_t revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    void *file_path, *reserved;
    uint32_t load_options_size;
    CHAR16 *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type, image_data_type;
} EFI_LOADED_IMAGE_PROTOCOL;

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER hdr;
    void *raise_tpl, *restore_tpl;
    EFI_STATUS (EFIAPI *allocate_pages)(uint32_t, uint32_t, uint64_t, uint64_t *);
    EFI_STATUS (EFIAPI *free_pages)(uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *get_memory_map)(uint64_t *, EFI_MEMORY_DESCRIPTOR *, uint64_t *,
                                         uint64_t *, uint32_t *);
    EFI_STATUS (EFIAPI *allocate_pool)(uint32_t, uint64_t, void **);
    EFI_STATUS (EFIAPI *free_pool)(void *);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    EFI_STATUS (EFIAPI *handle_protocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    EFI_STATUS (EFIAPI *exit_boot_services)(EFI_HANDLE, uint64_t);
    void *get_next_monotonic_count, *stall;
    EFI_STATUS (EFIAPI *set_watchdog_timer)(uint64_t,uint64_t,uint64_t,CHAR16 *);
    void *connect_controller;
    void *disconnect_controller, *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle, *locate_handle_buffer;
    EFI_STATUS (EFIAPI *locate_protocol)(EFI_GUID *, void *, void **);
};

typedef struct {
    EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    uint32_t firmware_revision, pad;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    void *con_out;
    EFI_HANDLE standard_error_handle;
    void *std_err;
    void *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    uint64_t number_of_table_entries;
    struct { EFI_GUID guid; void *table; } *configuration_table;
} EFI_SYSTEM_TABLE;

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ELF64_EHDR;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} ELF64_PHDR;

typedef void (EFIAPI *kernel_entry_fn)(uint64_t magic, uint64_t mbi);

static EFI_GUID loaded_image_guid = {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID simple_fs_guid = {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID gop_guid = {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
static EFI_GUID acpi2_guid = {0x8868E871,0xE4F1,0x11D3,{0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}};
static EFI_GUID acpi1_guid = {0xEB9D2D30,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
static EFI_GUID memory_attribute_guid = {0xF4560CF6,0x40EC,0x4B4A,{0xA1,0x92,0xBF,0x1D,0x57,0xD0,0xB1,0x89}};
static uint64_t loaded_kernel_base[32],loaded_kernel_pages[32];
static uint16_t loaded_kernel_count;

static void release_loaded_kernel(EFI_BOOT_SERVICES *bs) {
    while(loaded_kernel_count) {
        loaded_kernel_count--;
        (void)bs->free_pages(loaded_kernel_base[loaded_kernel_count],
                             loaded_kernel_pages[loaded_kernel_count]);
    }
}

static void *zero_mem(void *p, size_t n) {
    uint8_t *d=(uint8_t*)p; for(size_t i=0;i<n;i++) d[i]=0; return p;
}
static void *copy_mem(void *d, const void *s, size_t n) {
    uint8_t *a=(uint8_t*)d; const uint8_t *b=(const uint8_t*)s;
    for(size_t i=0;i<n;i++) a[i]=b[i]; return d;
}
static int bytes_eq(const uint8_t *a,const uint8_t *b,size_t n) {
    uint8_t x=0; for(size_t i=0;i<n;i++) x|=a[i]^b[i]; return x==0;
}
static uint64_t align_up(uint64_t x,uint64_t a) { return (x+a-1)&~(a-1); }
static uint32_t popcount32(uint32_t x) { uint32_t n=0; while(x){x&=x-1;n++;} return n; }
static uint32_t ctz32(uint32_t x) { uint32_t n=0; if(!x)return 0; while(!(x&1)){x>>=1;n++;} return n; }

static EFI_STATUS open_kernel(EFI_BOOT_SERVICES *bs, EFI_HANDLE image,
                              void **data, uint64_t *length) {
    if(!bs||!bs->handle_protocol||!bs->allocate_pool||!bs->free_pool||!data||!length)
        return EFI_INVALID_PARAMETER;
    EFI_LOADED_IMAGE_PROTOCOL *loaded=0;
    EFI_STATUS st=bs->handle_protocol(image,&loaded_image_guid,(void**)&loaded);
    if(EFI_ERROR(st)) return st;
    if(!loaded) return EFI_INVALID_PARAMETER;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;
    st=bs->handle_protocol(loaded->device_handle,&simple_fs_guid,(void**)&fs);
    if(EFI_ERROR(st)) return st;
    if(!fs||!fs->open_volume) return EFI_INVALID_PARAMETER;
    EFI_FILE *root=0,*file=0;
    st=fs->open_volume(fs,&root);
    if(EFI_ERROR(st)) return st;
    if(!root||!root->open||!root->close) return EFI_INVALID_PARAMETER;
    static CHAR16 path[]={'\\','K','I','R','A','.','B','I','N',0};
    st=root->open(root,&file,path,EFI_FILE_MODE_READ,0);
    if(EFI_ERROR(st)||!file) {
        file=0;
        static CHAR16 fallback[]={'\\','E','F','I','\\','B','O','O','T','\\','K','I','R','A','.','B','I','N',0};
        st=root->open(root,&file,fallback,EFI_FILE_MODE_READ,0);
    }
    root->close(root);
    if(EFI_ERROR(st)) return st;
    if(!file||!file->read||!file->close) return EFI_INVALID_PARAMETER;
    uint64_t cap=KERNEL_MAX;
    st=bs->allocate_pool(EFI_BOOT_SERVICES_DATA,cap,data);
    if(EFI_ERROR(st)){file->close(file);return st;}
    uint64_t total=0;
    while(total<cap) {
        uint64_t request=cap-total;
        st=file->read(file,&request,(uint8_t*)(*data)+total);
        if(EFI_ERROR(st)||!request) break;
        if(request>cap-total) { st=EFI_BUFFER_TOO_SMALL; break; }
        total+=request;
    }
    if(!EFI_ERROR(st)&&total==cap) {
        uint8_t extra=0;
        uint64_t request=1;
        st=file->read(file,&request,&extra);
        if(!EFI_ERROR(st)&&request) st=EFI_BUFFER_TOO_SMALL;
    }
    file->close(file);
    if(EFI_ERROR(st)||!total){bs->free_pool(*data);*data=0;return EFI_BUFFER_TOO_SMALL;}
    *length=total;
    return EFI_SUCCESS;
}

static int load_elf(EFI_BOOT_SERVICES *bs,const uint8_t *file,uint64_t size,
                    uint64_t *entry_out,uint64_t *kernel_base,uint64_t *kernel_end) {
    if(size<sizeof(ELF64_EHDR)) return -1;
    const ELF64_EHDR *e=(const ELF64_EHDR*)file;
    static const uint8_t elf_magic[4]={0x7f,'E','L','F'};
    if(!bytes_eq(e->ident,elf_magic,4)||e->ident[4]!=2||e->ident[5]!=1||
       e->ident[6]!=1||e->ident[7]!=0||
       e->type!=2||e->machine!=62||e->version!=1||e->ehsize!=sizeof(ELF64_EHDR)||
       e->phentsize!=sizeof(ELF64_PHDR)||(e->phoff&7u)||
       !e->phnum||e->phnum>32||e->phoff>size||
       (uint64_t)e->phnum*sizeof(ELF64_PHDR)>size-e->phoff) return -2;
    const ELF64_PHDR *ph=(const ELF64_PHDR*)(file+e->phoff);
    uint64_t low=UINT64_MAX,high=0; int executable_entry=0;
    for(uint16_t i=0;i<e->phnum;i++) {
        if(ph[i].type==2||ph[i].type==3||ph[i].type==7) return -2; /* no dynamic/interpreter/TLS */
        if(ph[i].type!=1) continue;
        if(!ph[i].memsz||ph[i].filesz>ph[i].memsz||ph[i].offset>size||
           ph[i].filesz>size-ph[i].offset||ph[i].vaddr!=ph[i].paddr||
           ph[i].paddr<0x100000||ph[i].paddr>=ONE_GB||
           (ph[i].paddr&(EFI_PAGE_SIZE-1u))||
           ph[i].memsz>ONE_GB-ph[i].paddr||(ph[i].flags&~7u)||
           ((ph[i].flags&3u)==3u)) return -3;
        if(ph[i].align>1) {
            if((ph[i].align&(ph[i].align-1))||
               ((ph[i].paddr-ph[i].offset)&(ph[i].align-1))) return -3;
        }
        if((ph[i].flags&1u)&&e->entry>=ph[i].paddr&&e->entry<ph[i].paddr+ph[i].memsz) executable_entry=1;
        uint64_t page_base=ph[i].paddr&~(uint64_t)(EFI_PAGE_SIZE-1u);
        uint64_t page_end=align_up(ph[i].paddr+ph[i].memsz,EFI_PAGE_SIZE);
        for(uint16_t j=0;j<i;j++) if(ph[j].type==1) {
            uint64_t other_base=ph[j].paddr&~(uint64_t)(EFI_PAGE_SIZE-1u);
            uint64_t other_end=align_up(ph[j].paddr+ph[j].memsz,EFI_PAGE_SIZE);
            if(page_base<other_end&&other_base<page_end) return -6;
        }
        if(ph[i].paddr<low) low=ph[i].paddr;
        if(ph[i].paddr+ph[i].memsz>high) high=ph[i].paddr+ph[i].memsz;
    }
    if(low==UINT64_MAX||high<=low||e->entry<low||e->entry>=high||!executable_entry) return -4;
    uint64_t base=low&~(uint64_t)(EFI_PAGE_SIZE-1);
    uint64_t end=align_up(high,EFI_PAGE_SIZE);
    loaded_kernel_count=0;
    for(uint16_t i=0;i<e->phnum;i++) if(ph[i].type==1) {
        uint64_t segment_base=ph[i].paddr&~(uint64_t)(EFI_PAGE_SIZE-1);
        uint64_t segment_end=align_up(ph[i].paddr+ph[i].memsz,EFI_PAGE_SIZE);
        uint64_t pages=(segment_end-segment_base)/EFI_PAGE_SIZE;
        uint64_t address=segment_base;
        uint32_t memory_type=(ph[i].flags&1u)?EFI_LOADER_CODE:EFI_LOADER_DATA;
        EFI_STATUS st=bs->allocate_pages(EFI_ALLOCATE_ADDRESS,memory_type,pages,&address);
        if(EFI_ERROR(st)||address!=segment_base) {
            if(!EFI_ERROR(st)&&address!=segment_base)
                (void)bs->free_pages(address,pages);
            release_loaded_kernel(bs);
            return -5;
        }
        loaded_kernel_base[loaded_kernel_count]=segment_base;
        loaded_kernel_pages[loaded_kernel_count]=pages;
        loaded_kernel_count++;
        zero_mem((void*)(uintptr_t)segment_base,(size_t)(segment_end-segment_base));
        copy_mem((void*)(uintptr_t)ph[i].paddr,file+ph[i].offset,(size_t)ph[i].filesz);
    }
    /* EFI_LOADER_CODE is not an executable guarantee on protected
       firmware. Require the standard Memory Attribute Protocol and verify
       that every executable PT_LOAD range has XP cleared before handoff. */
    EFI_MEMORY_ATTRIBUTE_PROTOCOL *attributes=0;
    EFI_STATUS attr_status=bs->locate_protocol
        ? bs->locate_protocol(&memory_attribute_guid,0,(void**)&attributes)
        : (EFI_STATUS)0x8000000000000003ull;
    if(EFI_ERROR(attr_status)||!attributes||!attributes->get_memory_attributes||
       !attributes->clear_memory_attributes) {
        release_loaded_kernel(bs);
        return -7;
    }
    for(uint16_t i=0;i<e->phnum;i++) if(ph[i].type==1) {
        uint64_t segment_base=ph[i].paddr&~(uint64_t)(EFI_PAGE_SIZE-1);
        uint64_t segment_end=align_up(ph[i].paddr+ph[i].memsz,EFI_PAGE_SIZE);
        uint64_t segment_length=segment_end-segment_base;
        uint64_t memory_attributes=0;
        attr_status=attributes->get_memory_attributes(attributes,segment_base,
                              segment_length,&memory_attributes);
        if(EFI_ERROR(attr_status)||(memory_attributes&EFI_MEMORY_RP))
            goto executable_attribute_error;
        if(ph[i].flags&1u) {
            if(memory_attributes&EFI_MEMORY_XP) {
                attr_status=attributes->clear_memory_attributes(attributes,
                        segment_base,segment_length,EFI_MEMORY_XP);
                if(EFI_ERROR(attr_status)) goto executable_attribute_error;
            }
            memory_attributes=0;
            attr_status=attributes->get_memory_attributes(attributes,segment_base,
                                  segment_length,&memory_attributes);
            if(EFI_ERROR(attr_status)||
               (memory_attributes&(EFI_MEMORY_RP|EFI_MEMORY_XP)))
                goto executable_attribute_error;
        } else if(ph[i].flags&2u) {
            if(memory_attributes&EFI_MEMORY_RO) {
                attr_status=attributes->clear_memory_attributes(attributes,
                        segment_base,segment_length,EFI_MEMORY_RO);
                if(EFI_ERROR(attr_status)) goto executable_attribute_error;
            }
            memory_attributes=0;
            attr_status=attributes->get_memory_attributes(attributes,segment_base,
                                  segment_length,&memory_attributes);
            if(EFI_ERROR(attr_status)||
               (memory_attributes&(EFI_MEMORY_RP|EFI_MEMORY_RO)))
                goto executable_attribute_error;
        }
    }
    *entry_out=e->entry; *kernel_base=base; *kernel_end=end;
    return 0;

executable_attribute_error:
    release_loaded_kernel(bs);
    return -7;
}

static void *alloc_low_pages(EFI_BOOT_SERVICES *bs,uint64_t bytes,uint32_t memory_type) {
    uint64_t pages=(bytes+EFI_PAGE_SIZE-1)/EFI_PAGE_SIZE;
    uint64_t address=ONE_GB-1;
    EFI_STATUS st=bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS,memory_type,pages,&address);
    return EFI_ERROR(st)?0:(void*)(uintptr_t)address;
}


static void free_low_pages(EFI_BOOT_SERVICES *bs,void *memory,uint64_t bytes) {
    if(memory) (void)bs->free_pages((uint64_t)(uintptr_t)memory,
                                    (bytes+EFI_PAGE_SIZE-1u)/EFI_PAGE_SIZE);
}

static int mask_info(uint32_t mask,uint8_t *pos,uint8_t *bits) {
    if(!mask) return -1;
    uint32_t shift=ctz32(mask), value=mask>>shift, n=popcount32(mask);
    if(n==0||n>8||(value& (value+1u))!=0) return -1;
    *pos=(uint8_t)shift; *bits=(uint8_t)n; return 0;
}

static int build_bootinfo(EFI_BOOT_SERVICES *bs,EFI_SYSTEM_TABLE *st,
                          void *blob,uint64_t blob_capacity,
                          void *mapbuf,uint64_t map_capacity,
                          uint64_t kernel_base,uint64_t kernel_end,
                          uint64_t *mapkey_out,uint32_t *blob_size_out) {
    void *rsdp=0;
    uint64_t table_count=st->configuration_table&&st->number_of_table_entries<=4096u
                         ?st->number_of_table_entries:0;
    for(uint64_t i=0;i<table_count;i++)
        if(bytes_eq((const uint8_t*)&st->configuration_table[i].guid,
                    (const uint8_t*)&acpi2_guid,sizeof(EFI_GUID))) {
            rsdp=st->configuration_table[i].table; break;
        }
    if(!rsdp) for(uint64_t i=0;i<table_count;i++)
        if(bytes_eq((const uint8_t*)&st->configuration_table[i].guid,
                    (const uint8_t*)&acpi1_guid,sizeof(EFI_GUID))) {
            rsdp=st->configuration_table[i].table; break;
        }
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=0;
    if(bs->locate_protocol)
        (void)bs->locate_protocol(&gop_guid,0,(void**)&gop);

    uint64_t map_size=map_capacity,mapkey=0,desc_size=0; uint32_t desc_ver=0;
    EFI_STATUS rc=bs->get_memory_map(&map_size,mapbuf,&mapkey,&desc_size,&desc_ver);
    if(EFI_ERROR(rc)||desc_size<sizeof(EFI_MEMORY_DESCRIPTOR)||!desc_size||
       map_size%desc_size||map_size>UINT32_MAX||desc_size>UINT32_MAX) return -1;

    uint32_t blob_size=0;
    int br=kbi_from_uefi_memory_map(mapbuf,(uint32_t)map_size,(uint32_t)desc_size,
        (uint64_t)(uintptr_t)rsdp,kernel_base,kernel_end-kernel_base,
        blob,(uint32_t)blob_capacity,&blob_size);
    if(br) return -10+br;

    kbi_header_t *h=(kbi_header_t*)blob;
    if(gop&&gop->mode&&gop->mode->info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *gi=gop->mode->info;
        uint32_t red=0,green=0,blue=0;
        if(gi->pixel_format==0){red=0x000000ff;green=0x0000ff00;blue=0x00ff0000;}
        else if(gi->pixel_format==1){red=0x00ff0000;green=0x0000ff00;blue=0x000000ff;}
        else if(gi->pixel_format==2){red=gi->pixel_information.red_mask;green=gi->pixel_information.green_mask;blue=gi->pixel_information.blue_mask;}
        uint8_t rp=0,rs=0,gp=0,gs=0,bp=0,bsz=0;
        uint64_t pitch=(uint64_t)gi->pixels_per_scanline*4u;
        if(gi->pixel_format<=2&&gop->mode->framebuffer_base&&
           gop->mode->framebuffer_size&&gi->horizontal_resolution&&
           gi->vertical_resolution&&gi->pixels_per_scanline>=gi->horizontal_resolution&&
           pitch<=UINT32_MAX&&pitch*gi->vertical_resolution<=gop->mode->framebuffer_size&&
           mask_info(red,&rp,&rs)==0&&mask_info(green,&gp,&gs)==0&&
           mask_info(blue,&bp,&bsz)==0&&
           (uint16_t)rp+rs<=32&&(uint16_t)gp+gs<=32&&(uint16_t)bp+bsz<=32&&
           !(red&green)&&!(red&blue)&&!(green&blue)&&
           gop->mode->framebuffer_size<=UINT64_MAX-gop->mode->framebuffer_base) {
            h->flags|=KBI_F_FRAMEBUFFER;
            h->fb_phys=gop->mode->framebuffer_base;
            h->fb_size=gop->mode->framebuffer_size;
            h->fb_pitch=(uint32_t)pitch;
            h->fb_width=gi->horizontal_resolution;
            h->fb_height=gi->vertical_resolution;
            h->fb_bpp=32;
            h->fb_red_pos=rp; h->fb_red_size=rs;
            h->fb_green_pos=gp; h->fb_green_size=gs;
            h->fb_blue_pos=bp; h->fb_blue_size=bsz;
        }
    }
    if(kbi_validate(blob,blob_size)!=0) return -20;
    *mapkey_out=mapkey;
    *blob_size_out=blob_size;
    return 0;
}
/* After a failed ExitBootServices call, firmware may have shut down
   services other than GetMemoryMap/ExitBootServices. Refresh only the map;
   never locate protocols, allocate, free, or touch files on this retry path. */
static int refresh_bootinfo_after_exit_failure(EFI_BOOT_SERVICES *bs,
                          void *blob,uint64_t blob_capacity,
                          void *mapbuf,uint64_t map_capacity,
                          uint64_t kernel_base,uint64_t kernel_end,
                          uint64_t *mapkey_out,uint32_t *blob_size_out) {
    kbi_header_t saved;
    copy_mem(&saved,blob,sizeof(saved));
    uint64_t map_size=map_capacity,mapkey=0,desc_size=0;
    uint32_t desc_ver=0;
    EFI_STATUS rc=bs->get_memory_map(&map_size,mapbuf,&mapkey,&desc_size,&desc_ver);
    if(EFI_ERROR(rc)||desc_size<sizeof(EFI_MEMORY_DESCRIPTOR)||!desc_size||
       map_size%desc_size||map_size>UINT32_MAX||desc_size>UINT32_MAX) return -1;
    uint32_t blob_size=0;
    int br=kbi_from_uefi_memory_map(mapbuf,(uint32_t)map_size,(uint32_t)desc_size,
        saved.rsdp_phys,kernel_base,kernel_end-kernel_base,
        blob,(uint32_t)blob_capacity,&blob_size);
    if(br||!blob_size) return -2;
    kbi_header_t *h=(kbi_header_t*)blob;
    if(saved.flags&KBI_F_FRAMEBUFFER) {
        h->flags|=KBI_F_FRAMEBUFFER;
        h->fb_phys=saved.fb_phys;
        h->fb_size=saved.fb_size;
        h->fb_width=saved.fb_width;
        h->fb_height=saved.fb_height;
        h->fb_pitch=saved.fb_pitch;
        h->fb_bpp=saved.fb_bpp;
        h->fb_red_pos=saved.fb_red_pos;
        h->fb_red_size=saved.fb_red_size;
        h->fb_green_pos=saved.fb_green_pos;
        h->fb_green_size=saved.fb_green_size;
        h->fb_blue_pos=saved.fb_blue_pos;
        h->fb_blue_size=saved.fb_blue_size;
        h->fb_reserved=saved.fb_reserved;
    }
    if(kbi_validate(blob,blob_size)!=0) return -3;
    *mapkey_out=mapkey;
    *blob_size_out=blob_size;
    return 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    if(!st||!st->boot_services) return EFI_INVALID_PARAMETER;
    EFI_BOOT_SERVICES *bs=st->boot_services;
    if(!bs->allocate_pages||!bs->free_pages||!bs->get_memory_map||
       !bs->exit_boot_services||!bs->handle_protocol||
       !bs->allocate_pool||!bs->free_pool) return EFI_INVALID_PARAMETER;
    if(bs->set_watchdog_timer) (void)bs->set_watchdog_timer(0,0,0,0);
    void *file=0; uint64_t file_size=0;
    EFI_STATUS rc=open_kernel(bs,image,&file,&file_size);
    if(EFI_ERROR(rc)||!file) return rc;
    uint64_t entry=0,kernel_base=0,kernel_end=0;
    int er=load_elf(bs,(const uint8_t*)file,file_size,&entry,&kernel_base,&kernel_end);
    bs->free_pool(file);
    if(er) return (EFI_STATUS)(0x8000000000001000ull+(uint32_t)(-er));

    void *mbi=alloc_low_pages(bs,BOOTINFO_MAX,EFI_LOADER_DATA);
    void *mapbuf=alloc_low_pages(bs,1024u*1024u,EFI_LOADER_DATA);
    if(!mbi||!mapbuf) {
        free_low_pages(bs,mbi,BOOTINFO_MAX);
        free_low_pages(bs,mapbuf,1024u*1024u);
        release_loaded_kernel(bs);
        return (EFI_STATUS)0x8000000000002001ull;
    }
    uint64_t mapkey=0; uint32_t bootinfo_size=0;
    int br=build_bootinfo(bs,st,mbi,BOOTINFO_MAX,mapbuf,1024u*1024u,
                          kernel_base,kernel_end,&mapkey,&bootinfo_size);
    if(br||!bootinfo_size) {
        free_low_pages(bs,mbi,BOOTINFO_MAX);
        free_low_pages(bs,mapbuf,1024u*1024u);
        release_loaded_kernel(bs);
        return (EFI_STATUS)(0x8000000000002000ull+(uint32_t)(br? -br : 0xFF));
    }

    rc=bs->exit_boot_services(image,mapkey);
    if(EFI_ERROR(rc)) {
        /* After a failed ExitBootServices, only refresh the map and retry.
           Do not free or allocate pages once firmware may be partially shut down. */
        br=refresh_bootinfo_after_exit_failure(bs,mbi,BOOTINFO_MAX,
                          mapbuf,1024u*1024u,kernel_base,kernel_end,
                          &mapkey,&bootinfo_size);
        if(br||!bootinfo_size) return (EFI_STATUS)0x8000000000002002ull;
        rc=bs->exit_boot_services(image,mapkey);
        if(EFI_ERROR(rc)) return rc;
    }
    loaded_kernel_count=0; /* ownership passes to the running kernel */
    ((kernel_entry_fn)(uintptr_t)entry)(KIRA_BOOTINFO_MAGIC,(uint64_t)(uintptr_t)mbi);
    for(;;) __asm__ volatile("cli; hlt");
}
