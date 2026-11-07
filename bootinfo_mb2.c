#include "bootinfo.h"
#include <stdint.h>

#define MB2_MAGIC 0x36D76289u
#define MB2_MAX_SIZE 65536u
#define MB2_TAG_END 0u
#define MB2_TAG_CMDLINE 1u
#define MB2_TAG_MODULE 3u
#define MB2_TAG_MMAP 6u
#define MB2_TAG_FRAMEBUFFER 8u

static uint8_t handoff[KIRA_BOOTINFO_MAX_SIZE] __attribute__((aligned(16)));

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void copy_bytes(uint8_t* d, const uint8_t* s, uint32_t n) {
    for(uint32_t i=0;i<n;i++) d[i]=s[i];
}
static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

static int mb2_bounds(const uint8_t* b, uint32_t available, uint32_t* total) {
    if(!b || available<16 || ((uintptr_t)b&7u)) return -1;
    uint32_t n=rd32(b);
    if(n<16 || n>MB2_MAX_SIZE || n>available || (n&7u)) return -2;
    uint32_t off=8;
    while(off<=n-8) {
        uint32_t type=rd32(b+off), size=rd32(b+off+4);
        if(size<8 || size>n-off) return -3;
        uint32_t padded=align8(size);
        if(padded>n-off) return -3;
        if(type==MB2_TAG_END) {
            if(size!=8 || off+padded!=n) return -3;
            *total=n;
            return 0;
        }
        off+=padded;
    }
    return -3;
}

static uint32_t mb2_type(uint32_t t) {
    switch(t) {
        case 1: return KBI_MEM_USABLE;
        case 3: return KBI_MEM_ACPI_RECLAIM;
        case 4: return KBI_MEM_ACPI_NVS;
        case 5: return KBI_MEM_BAD;
        default: return KBI_MEM_RESERVED;
    }
}

/* Converts the Multiboot2 data currently supplied by GRUB into the stable KBI ABI.
   It does not reinterpret copied ACPI tables as physical addresses: ACPI remains
   available through the legacy MB2 path until a native loader supplies RSDP. */
int kbi_from_multiboot2(const void* info, uint32_t available,
                        uint64_t kernel_base, uint64_t kernel_size,
                        const void** result, uint32_t* result_size) {
    if(!result || !result_size || !kernel_base || !kernel_size ||
       kernel_base > UINT64_MAX-kernel_size) return -10;
    const uint8_t* b=(const uint8_t*)info;
    uint32_t total=0;
    int rc=mb2_bounds(b,available,&total);
    if(rc) return -20+rc;

    uint32_t mmap_tag=0, mmap_count=0, mmap_stride=0;
    uint32_t modules_count=0;
    uint32_t cmd_tag=0, cmd_len=0, fb_tag=0;
    for(uint32_t off=8;off<total;) {
        uint32_t type=rd32(b+off), size=rd32(b+off+4);
        if(type==MB2_TAG_MMAP) {
            if(mmap_tag || size<16) return -30;
            mmap_stride=rd32(b+off+8);
            if(rd32(b+off+12)!=0 || mmap_stride<24 ||
               (size-16)%mmap_stride) return -30;
            mmap_count=(size-16)/mmap_stride;
            if(!mmap_count || mmap_count>KIRA_BOOTINFO_MAX_MMAP) return -30;
            mmap_tag=off;
        } else if(type==MB2_TAG_MODULE) {
            if(size<17 || modules_count==KIRA_BOOTINFO_MAX_MODULES) return -31;
            uint32_t start=rd32(b+off+8), end=rd32(b+off+12);
            if(end<=start || b[off+size-1]!=0) return -31;
            modules_count++;

        } else if(type==MB2_TAG_CMDLINE) {
            if(cmd_tag || size<9 || b[off+size-1]!=0) return -32;
            cmd_tag=off; cmd_len=size-8;
        } else if(type==MB2_TAG_FRAMEBUFFER) {
            if(fb_tag || size<32) return -33;
            fb_tag=off;
        }
        off+=align8(size);
    }
    if(!mmap_tag) return -34;

    uint32_t cursor=sizeof(kbi_header_t);
    cursor=align8(cursor);
    uint32_t mmap_off=cursor;
    uint64_t bytes=(uint64_t)mmap_count*sizeof(kbi_mmap_entry_t);
    if(bytes>KIRA_BOOTINFO_MAX_SIZE-cursor) return -35;
    cursor+=(uint32_t)bytes;
    cursor=align8(cursor);
    uint32_t modules_off=modules_count?cursor:0;
    bytes=(uint64_t)modules_count*sizeof(kbi_module_t);
    if(bytes>KIRA_BOOTINFO_MAX_SIZE-cursor) return -35;
    cursor+=(uint32_t)bytes;
    cursor=align8(cursor);
    uint32_t modules_names[ KIRA_BOOTINFO_MAX_MODULES ];
    uint32_t name_lengths[ KIRA_BOOTINFO_MAX_MODULES ];
    uint32_t mi=0;
    for(uint32_t off=8;off<total;) {
        uint32_t type=rd32(b+off), size=rd32(b+off+4);
        if(type==MB2_TAG_MODULE) {
            uint32_t len=size-16;
            if(len>KIRA_BOOTINFO_MAX_SIZE-cursor) return -35;
            modules_names[mi]=cursor; name_lengths[mi]=len;
            cursor+=len; cursor=align8(cursor); if(cursor>KIRA_BOOTINFO_MAX_SIZE) return -35; mi++;
        }
        off+=align8(size);
    }
    uint32_t cmd_off=0;
    if(cmd_len) {
        if(cmd_len>KIRA_BOOTINFO_MAX_SIZE-cursor) return -35;
        cmd_off=cursor; cursor+=cmd_len;
    }
    if(cursor>KIRA_BOOTINFO_MAX_SIZE) return -35;
    for(uint32_t i=0;i<cursor;i++) handoff[i]=0;

    kbi_header_t* h=(kbi_header_t*)handoff;
    h->magic=KIRA_BOOTINFO_MAGIC; h->version=KIRA_BOOTINFO_VERSION;
    h->header_size=sizeof(*h); h->total_size=cursor;
    h->arch=KBI_ARCH_X86_64; h->boot_mode=KBI_BOOT_MULTIBOOT2;
    h->mmap_off=mmap_off; h->mmap_count=mmap_count;
    h->mmap_entry_size=sizeof(kbi_mmap_entry_t);
    h->modules_off=modules_off; h->modules_count=modules_count;
    h->module_entry_size=modules_count?sizeof(kbi_module_t):0;
    h->cmdline_off=cmd_off; h->cmdline_len=cmd_len;
    h->kernel_phys_base=kernel_base; h->kernel_size=kernel_size;
    h->loader_id=MB2_MAGIC;
    if(cmd_len) h->flags|=KBI_F_CMDLINE;

    const uint8_t* mt=b+mmap_tag;
    kbi_mmap_entry_t* out=(kbi_mmap_entry_t*)(handoff+mmap_off);
    uint32_t n=0;
    for(uint32_t p=16;p<rd32(mt+4);p+=mmap_stride) {
        out[n].base=rd64(mt+p); out[n].length=rd64(mt+p+8);
        out[n].type=mb2_type(rd32(mt+p+16)); out[n].attributes=0; n++;
    }
    /* Firmware maps are normally ordered; normalize them before validation. */
    for(uint32_t i=1;i<n;i++) {
        kbi_mmap_entry_t v=out[i]; uint32_t j=i;
        while(j && out[j-1].base>v.base) { out[j]=out[j-1]; j--; }
        out[j]=v;
    }

    kbi_module_t* mods=modules_count?(kbi_module_t*)(handoff+modules_off):0;
    mi=0;
    for(uint32_t off=8;off<total;) {
        uint32_t type=rd32(b+off), size=rd32(b+off+4);
        if(type==MB2_TAG_MODULE) {
            mods[mi].base=rd32(b+off+8);
            mods[mi].length=rd32(b+off+12)-mods[mi].base;
            mods[mi].name_off=modules_names[mi];
            mods[mi].name_len=name_lengths[mi];
            copy_bytes(handoff+modules_names[mi],b+off+16,name_lengths[mi]);
            mi++;
        }
        off+=align8(size);
    }
    if(cmd_len) copy_bytes(handoff+cmd_off,b+cmd_tag+8,cmd_len);

    if(fb_tag) {
        const uint8_t* f=b+fb_tag;
        uint8_t kind=f[29];
        if(kind==1 && rd32(f+16) && rd32(f+20) && rd32(f+24) && rd32(f+4)>=38 && (f[28]==15 || f[28]==16 || f[28]==24 || f[28]==32)) {
            /* RGB direct-colour framebuffer: Multiboot2 colour_info follows byte 32. */
            if(rd64(f+8)) {
                h->fb_phys=rd64(f+8);
                h->fb_pitch=rd32(f+16); h->fb_width=rd32(f+20); h->fb_height=rd32(f+24);
                h->fb_bpp=f[28]; h->fb_red_pos=f[32]; h->fb_red_size=f[33];
                h->fb_green_pos=f[34]; h->fb_green_size=f[35];
                h->fb_blue_pos=f[36]; h->fb_blue_size=f[37];
                h->fb_size=(uint64_t)h->fb_pitch*h->fb_height;
                if(h->fb_size && h->fb_phys) h->flags|=KBI_F_FRAMEBUFFER;
                else { h->fb_phys=0; h->fb_size=0; h->fb_width=0; h->fb_height=0; h->fb_pitch=0; }
            }
        }
    }
    rc=kbi_validate(h,cursor);
    if(rc) return -40+rc;
    *result=handoff; *result_size=cursor;
    return 0;
}
