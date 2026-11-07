/* KIRA native UEFI loader.
 * Loads the identity-linked x86-64 ELF kernel, synthesizes a Multiboot2
 * information block from UEFI services, exits boot services, then transfers
 * control to the kernel's UEFI long-mode entry shim.
 */
#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))
#define EFI_ERROR(s) (((int64_t)(s)) < 0)
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ull)
#define EFI_LOADER_DATA 2u
#define EFI_ALLOCATE_ADDRESS 2u
#define EFI_ALLOCATE_MAX_ADDRESS 1u
#define EFI_FILE_MODE_READ 1ull
#define EFI_PAGE_SIZE 4096u
#define MB2_MAGIC 0x36D76289u
#define MB2_INFO_MAX (256u * 1024u)
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
    void *get_next_monotonic_count, *stall, *set_watchdog_timer, *connect_controller;
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
    EFI_LOADED_IMAGE_PROTOCOL *loaded=0;
    EFI_STATUS st=bs->handle_protocol(image,&loaded_image_guid,(void**)&loaded);
    if(EFI_ERROR(st)||!loaded) return st;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;
    st=bs->handle_protocol(loaded->device_handle,&simple_fs_guid,(void**)&fs);
    if(EFI_ERROR(st)||!fs) return st;
    EFI_FILE *root=0,*file=0;
    st=fs->open_volume(fs,&root);
    if(EFI_ERROR(st)||!root) return st;
    static CHAR16 path[]={'\\','K','I','R','A','.','B','I','N',0};
    st=root->open(root,&file,path,EFI_FILE_MODE_READ,0);
    if(EFI_ERROR(st)||!file) {
        static CHAR16 fallback[]={'\\','E','F','I','\\','B','O','O','T','\\','K','I','R','A','.','B','I','N',0};
        st=root->open(root,&file,fallback,EFI_FILE_MODE_READ,0);
    }
    root->close(root);
    if(EFI_ERROR(st)||!file) return st;
    uint64_t cap=KERNEL_MAX;
    st=bs->allocate_pool(EFI_LOADER_DATA,cap,data);
    if(EFI_ERROR(st)){file->close(file);return st;}
    uint64_t got=cap;
    st=file->read(file,&got,*data);
    file->close(file);
    if(EFI_ERROR(st)||!got||got>cap){bs->free_pool(*data);*data=0;return EFI_BUFFER_TOO_SMALL;}
    *length=got;
    return EFI_SUCCESS;
}

static int load_elf(EFI_BOOT_SERVICES *bs,const uint8_t *file,uint64_t size,
                    uint64_t *entry_out,uint64_t *kernel_base,uint64_t *kernel_end) {
    if(size<sizeof(ELF64_EHDR)) return -1;
    const ELF64_EHDR *e=(const ELF64_EHDR*)file;
    static const uint8_t elf_magic[4]={0x7f,'E','L','F'};
    if(!bytes_eq(e->ident,elf_magic,4)||e->ident[4]!=2||e->ident[5]!=1||
       e->type!=2||e->machine!=62||e->version!=1||e->ehsize!=sizeof(ELF64_EHDR)||
       e->phentsize!=sizeof(ELF64_PHDR)||
       !e->phnum||e->phnum>32||e->phoff>size||
       (uint64_t)e->phnum*sizeof(ELF64_PHDR)>size-e->phoff) return -2;
    const ELF64_PHDR *ph=(const ELF64_PHDR*)(file+e->phoff);
    uint64_t low=UINT64_MAX,high=0; int executable_entry=0;
    for(uint16_t i=0;i<e->phnum;i++) if(ph[i].type==1) {
        if(!ph[i].memsz||ph[i].filesz>ph[i].memsz||ph[i].offset>size||
           ph[i].filesz>size-ph[i].offset||ph[i].vaddr!=ph[i].paddr||
           ph[i].paddr<0x100000||ph[i].paddr>=ONE_GB||
           ph[i].memsz>ONE_GB-ph[i].paddr) return -3;
        if((ph[i].flags&1u)&&e->entry>=ph[i].paddr&&e->entry<ph[i].paddr+ph[i].memsz) executable_entry=1;
        for(uint16_t j=0;j<i;j++) if(ph[j].type==1 &&
            ph[i].paddr<ph[j].paddr+ph[j].memsz && ph[j].paddr<ph[i].paddr+ph[i].memsz) return -6;
        if(ph[i].paddr<low) low=ph[i].paddr;
        if(ph[i].paddr+ph[i].memsz>high) high=ph[i].paddr+ph[i].memsz;
    }
    if(low==UINT64_MAX||high<=low||e->entry<low||e->entry>=high||!executable_entry) return -4;
    uint64_t base=low&~(uint64_t)(EFI_PAGE_SIZE-1);
    uint64_t end=align_up(high,EFI_PAGE_SIZE);
    uint64_t pages=(end-base)/EFI_PAGE_SIZE, address=base;
    EFI_STATUS st=bs->allocate_pages(EFI_ALLOCATE_ADDRESS,EFI_LOADER_DATA,pages,&address);
    if(EFI_ERROR(st)||address!=base) return -5;
    zero_mem((void*)(uintptr_t)base,(size_t)(end-base));
    for(uint16_t i=0;i<e->phnum;i++) if(ph[i].type==1)
        copy_mem((void*)(uintptr_t)ph[i].paddr,file+ph[i].offset,(size_t)ph[i].filesz);
    *entry_out=e->entry; *kernel_base=base; *kernel_end=end;
    return 0;
}

static void *alloc_low_pages(EFI_BOOT_SERVICES *bs,uint64_t bytes) {
    uint64_t pages=(bytes+EFI_PAGE_SIZE-1)/EFI_PAGE_SIZE;
    uint64_t address=ONE_GB-1;
    EFI_STATUS st=bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS,EFI_LOADER_DATA,pages,&address);
    return EFI_ERROR(st)?0:(void*)(uintptr_t)address;
}


static int mask_info(uint32_t mask,uint8_t *pos,uint8_t *bits) {
    if(!mask) return -1;
    uint32_t shift=ctz32(mask), value=mask>>shift, n=popcount32(mask);
    if(n==0||n>8||(value& (value+1u))!=0) return -1;
    *pos=(uint8_t)shift; *bits=(uint8_t)n; return 0;
}

static int build_mbi(EFI_BOOT_SERVICES *bs,EFI_SYSTEM_TABLE *st,
                     void *mbi,uint64_t mbi_capacity,void *mapbuf,uint64_t mapcap,
                     uint64_t *mapkey_out) {
    uint64_t map_size=mapcap,mapkey=0,desc_size=0; uint32_t desc_ver=0;
    EFI_STATUS rc=bs->get_memory_map(&map_size,mapbuf,&mapkey,&desc_size,&desc_ver);
    if(EFI_ERROR(rc)||desc_size<sizeof(EFI_MEMORY_DESCRIPTOR)||!desc_size||
       map_size%desc_size) return -1;
    uint64_t count=map_size/desc_size;
    if(count>4096) return -2;
    zero_mem(mbi,(size_t)mbi_capacity);
    uint8_t *base=(uint8_t*)mbi, *p=base+8;
    uint32_t mm_size=(uint32_t)(16+count*24);
    if((uint64_t)(p-base)+mm_size+8>mbi_capacity) return -3;
    uint32_t *mm=(uint32_t*)p; mm[0]=6; mm[1]=mm_size;
    mm[2]=24; mm[3]=0;
    uint8_t *q=p+16;
    for(uint64_t i=0;i<count;i++) {
        EFI_MEMORY_DESCRIPTOR *d=(EFI_MEMORY_DESCRIPTOR*)((uint8_t*)mapbuf+i*desc_size);
        if(d->pages>UINT64_MAX/EFI_PAGE_SIZE) return -4;
        uint64_t len=d->pages*EFI_PAGE_SIZE;
        if(d->physical_start>UINT64_MAX-len) return -4;
        uint32_t type=2;
        if(d->type==7) type=1;
        else if(d->type==9) type=3;
        else if(d->type==10) type=4;
        else if(d->type==8) type=5;
        uint64_t *pair=(uint64_t*)q; pair[0]=d->physical_start; pair[1]=len;
        ((uint32_t*)q)[4]=type; ((uint32_t*)q)[5]=0; q+=24;
    }
    p=(uint8_t*)align_up((uintptr_t)(p+mm_size),8);

    void *rsdp=0;
    for(uint64_t i=0;i<st->number_of_table_entries;i++)
        if(bytes_eq((const uint8_t*)&st->configuration_table[i].guid,
                    (const uint8_t*)&acpi2_guid,sizeof(EFI_GUID))) {
            rsdp=st->configuration_table[i].table; break;
        }
    if(!rsdp) for(uint64_t i=0;i<st->number_of_table_entries;i++)
        if(bytes_eq((const uint8_t*)&st->configuration_table[i].guid,
                    (const uint8_t*)&acpi1_guid,sizeof(EFI_GUID))) {
            rsdp=st->configuration_table[i].table; break;
        }
    if(rsdp) {
        uint8_t rev=((uint8_t*)rsdp)[15];
        uint32_t rsdp_size=rev>=2?*(uint32_t*)((uint8_t*)rsdp+20):20;
        uint32_t tag_type=rev>=2?15:14;
        if(rsdp_size<(rev>=2?36u:20u)||rsdp_size>4096||
           (uint64_t)(p-base)+8+rsdp_size>mbi_capacity) return -5;
        uint32_t *tag=(uint32_t*)p; tag[0]=tag_type; tag[1]=8+rsdp_size;
        copy_mem(p+8,rsdp,rsdp_size);
        p=(uint8_t*)align_up((uintptr_t)(p+8+rsdp_size),8);
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=0;
    if(bs->locate_protocol &&
       !EFI_ERROR(bs->locate_protocol(&gop_guid,0,(void**)&gop)) &&
       gop&&gop->mode&&gop->mode->info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *gi=gop->mode->info;
        uint32_t red=0,green=0,blue=0;
        if(gi->pixel_format==0){red=0x000000ff;green=0x0000ff00;blue=0x00ff0000;}
        else if(gi->pixel_format==1){red=0x00ff0000;green=0x0000ff00;blue=0x000000ff;}
        else if(gi->pixel_format==2){red=gi->pixel_information.red_mask;green=gi->pixel_information.green_mask;blue=gi->pixel_information.blue_mask;}
        uint8_t rp=0,rs=0,gp=0,gs=0,bp=0,bsz=0;
        if(gi->pixel_format<=2&&gop->mode->framebuffer_base&&
           gop->mode->framebuffer_size&&
           mask_info(red,&rp,&rs)==0&&mask_info(green,&gp,&gs)==0&&
           mask_info(blue,&bp,&bsz)==0&&
           (uint16_t)rp+rs<=32&&(uint16_t)gp+gs<=32&&(uint16_t)bp+bsz<=32&&
           !(red&green)&&!(red&blue)&&!(green&blue)&&
           (uint64_t)gi->pixels_per_scanline*4<=UINT32_MAX&&
           (uint64_t)gi->pixels_per_scanline*4*gi->vertical_resolution<=gop->mode->framebuffer_size) {
            if((uint64_t)(p-base)+38>mbi_capacity) return -6;
            uint32_t *w=(uint32_t*)p; w[0]=8; w[1]=38;
            *(uint64_t*)(p+8)=gop->mode->framebuffer_base;
            *(uint32_t*)(p+16)=gi->pixels_per_scanline*4;
            *(uint32_t*)(p+20)=gi->horizontal_resolution;
            *(uint32_t*)(p+24)=gi->vertical_resolution;
            p[28]=32; p[29]=1; p[30]=p[31]=0;
            p[32]=rp; p[33]=rs; p[34]=gp; p[35]=gs; p[36]=bp; p[37]=bsz;
            p=(uint8_t*)align_up((uintptr_t)(p+38),8);
        }
    }
    if((uint64_t)(p-base)+8>mbi_capacity) return -7;
    uint32_t *end=(uint32_t*)p; end[0]=0; end[1]=8; p+=8;
    uint32_t total=(uint32_t)(p-base);
    ((uint32_t*)mbi)[0]=total; ((uint32_t*)mbi)[1]=0;
    *mapkey_out=mapkey;
    return 0;
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs=st->boot_services;
    void *file=0; uint64_t file_size=0;
    EFI_STATUS rc=open_kernel(bs,image,&file,&file_size);
    if(EFI_ERROR(rc)||!file) return rc;
    uint64_t entry=0,kernel_base=0,kernel_end=0;
    int er=load_elf(bs,(const uint8_t*)file,file_size,&entry,&kernel_base,&kernel_end);
    bs->free_pool(file);
    if(er) return (EFI_STATUS)(0x8000000000001000ull+(uint32_t)(-er));

    void *mbi=alloc_low_pages(bs,MB2_INFO_MAX);
    void *mapbuf=alloc_low_pages(bs,1024u*1024u);
    if(!mbi||!mapbuf) return (EFI_STATUS)0x8000000000002001ull;
    uint64_t mapkey=0;
    int br=build_mbi(bs,st,mbi,MB2_INFO_MAX,mapbuf,1024u*1024u,&mapkey);
    if(br) return (EFI_STATUS)(0x8000000000002000ull+(uint32_t)(-br));

    rc=bs->exit_boot_services(image,mapkey);
    if(EFI_ERROR(rc)) {
        /* Firmware may invalidate the key; rebuild from a fresh map without
           allocating or freeing anything, then retry exactly once. */
        br=build_mbi(bs,st,mbi,MB2_INFO_MAX,mapbuf,1024u*1024u,&mapkey);
        if(br) return (EFI_STATUS)0x8000000000002002ull;
        rc=bs->exit_boot_services(image,mapkey);
        if(EFI_ERROR(rc)) return rc;
    }
    ((kernel_entry_fn)(uintptr_t)entry)(MB2_MAGIC,(uint64_t)(uintptr_t)mbi);
    for(;;) __asm__ volatile("cli; hlt");
}
