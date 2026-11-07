#include "bootinfo.h"
#include "fdt.h"

#define KBI_FDT_MAX_REGIONS 64u
#define KBI_FDT_MAX_RESERVED 128u
#define KBI_FDT_MAX_MAP 384u

/* Boot-time scratch; the converter is called once before the allocator starts. */
static kbi_mmap_entry_t regions[KBI_FDT_MAX_MAP];
static kbi_mmap_entry_t reserved[KBI_FDT_MAX_RESERVED];

static int add_region(kbi_mmap_entry_t* a,uint32_t* n,uint32_t cap,
                      uint64_t base,uint64_t size,uint32_t type) {
    if(!size||base>UINT64_MAX-size||*n>=cap) return -1;
    a[*n]=(kbi_mmap_entry_t){.base=base,.length=size,.type=type,.attributes=0};
    (*n)++;
    return 0;
}

/* Overlay a reserved physical range on sorted, non-overlapping memory ranges.
 * With require_coverage set, every byte must be covered (used for the kernel). */
static int mark_range(kbi_mmap_entry_t* a,uint32_t* n,uint32_t cap,
                      uint64_t base,uint64_t size,uint32_t type,
                      int require_coverage) {
    if(!size||base>UINT64_MAX-size) return -1;
    uint64_t end=base+size,covered=base;
    for(uint32_t i=0;i<*n;i++) {
        kbi_mmap_entry_t original=a[i];
        uint64_t old_end=original.base+original.length;
        if(old_end<=base||original.base>=end) continue;
        uint64_t lo=original.base>base?original.base:base;
        uint64_t hi=old_end<end?old_end:end;
        if(hi<=lo) continue;
        if(require_coverage&&lo>covered) return -1;
        uint32_t extra=(lo>original.base?1u:0u)+(hi<old_end?1u:0u);
        if(extra>cap-*n) return -1;
        for(uint32_t j=*n;j>i+1u;j--)
            a[j+extra-1u]=a[j-1u];
        uint32_t at=i;
        if(lo>original.base) {
            a[at]=original;
            a[at].length=lo-original.base;
            at++;
        }
        a[at].base=lo;
        a[at].length=hi-lo;
        a[at].type=type;
        a[at].attributes=0;
        at++;
        if(hi<old_end) {
            a[at]=original;
            a[at].base=hi;
            a[at].length=old_end-hi;
        }
        *n+=extra;
        i+=extra;
        if(require_coverage) {
            if(lo>covered) return -1;
            covered=hi;
            if(covered==end) return 0;
        }
    }
    return require_coverage&&covered<end?-1:0;
}

static int add_fdt_reg_ranges(const void* fdt,int node,
                              kbi_mmap_entry_t* out,uint32_t* count,
                              uint32_t capacity,uint32_t type) {
    int parent=fdt_parent(fdt,node);
    uint32_t ac=2,sc=1,reg_len=0;
    if(parent>=0) {
        fdt_prop_u32(fdt,parent,"#address-cells",&ac);
        fdt_prop_u32(fdt,parent,"#size-cells",&sc);
    }
    const void* reg=fdt_prop(fdt,node,"reg",&reg_len);
    if(ac<1||ac>2||sc<1||sc>2||!reg) return -1;
    uint32_t stride=4u*(ac+sc);
    if(!stride||!reg_len||reg_len%stride) return -1;
    for(uint32_t i=0;i<reg_len/stride;i++) {
        uint64_t base=0,size=0;
        if(fdt_reg(fdt,node,i,&base,&size)!=0) return -1;
        if(size&&add_region(out,count,capacity,base,size,type)!=0) return -1;
    }
    return 0;
}

/* Build the common KBI handoff from an ARM Device Tree. RAM banks are
 * normalized, FDT reserve-map and /reserved-memory ranges are excluded,
 * and the kernel/framebuffer are explicitly typed before validation. */
int kbi_from_fdt(const void* fdt,uint32_t available,uint64_t fdt_phys,
                 uint64_t kernel_base,uint64_t kernel_size,
                 void* out,uint32_t capacity,uint32_t* out_size) {
    if(out_size) *out_size=0;
    if(!fdt||!out||!out_size||!fdt_phys||(fdt_phys&7)||
       !kernel_base||!kernel_size||kernel_base>UINT64_MAX-kernel_size||
       !fdt_tree_valid(fdt,available)) return -1;

    uint32_t count=0,reserved_count=0;
    int depth=0,root=fdt_root(fdt);
    if(root<0) return -2;
    for(int node=root;node>=0;node=fdt_next_node(fdt,node,&depth)) {
        if(!fdt_node_enabled(fdt,node)) continue;
        uint32_t type_len=0;
        const char* dtype=(const char*)fdt_prop(fdt,node,"device_type",&type_len);
        if(dtype&&type_len>=7&&!__builtin_memcmp(dtype,"memory",7)) {
            if(add_fdt_reg_ranges(fdt,node,regions,&count,
                                  KBI_FDT_MAX_REGIONS,KBI_MEM_USABLE)!=0) return -3;
        }
    }
    if(!count) return -4;

    unsigned reserve_entries=fdt_reserve_count(fdt,available);
    if(reserve_entries>KBI_FDT_MAX_RESERVED) return -5;
    for(unsigned i=0;i<reserve_entries;i++) {
        uint64_t base=0,size=0;
        if(fdt_reserve(fdt,available,i,&base,&size)!=0||
           add_region(reserved,&reserved_count,KBI_FDT_MAX_RESERVED,
                      base,size,KBI_MEM_RESERVED)!=0) return -5;
    }

    int rnode=fdt_find_path(fdt,"/reserved-memory");
    if(rnode>=0) {
        int rd=0;
        for(int node=fdt_next_node(fdt,rnode,&rd);node>=0&&rd>0;
            node=fdt_next_node(fdt,node,&rd)) {
            if(rd!=1||!fdt_node_enabled(fdt,node)) continue;
            if(!fdt_prop(fdt,node,"reg",0)) continue;
            if(add_fdt_reg_ranges(fdt,node,reserved,&reserved_count,
                                  KBI_FDT_MAX_RESERVED,KBI_MEM_RESERVED)!=0) return -6;
        }
    }

    /* Sort RAM banks and merge adjacent/overlapping firmware descriptions. */
    for(uint32_t i=1;i<count;i++) {
        kbi_mmap_entry_t v=regions[i]; uint32_t j=i;
        while(j&&regions[j-1].base>v.base){regions[j]=regions[j-1];j--;}
        regions[j]=v;
    }
    uint32_t merged=0;
    for(uint32_t i=0;i<count;i++) {
        uint64_t end=regions[i].base+regions[i].length;
        if(merged) {
            kbi_mmap_entry_t* prev=&regions[merged-1];
            uint64_t prev_end=prev->base+prev->length;
            if(regions[i].base<=prev_end) {
                if(end>prev_end) prev->length=end-prev->base;
                continue;
            }
        }
        regions[merged++]=regions[i];
    }

    /* Reservations outside RAM are harmless; all intersections are overlaid. */
    for(uint32_t i=0;i<reserved_count;i++)
        if(mark_range(regions,&merged,KBI_FDT_MAX_MAP,reserved[i].base,
                      reserved[i].length,KBI_MEM_RESERVED,0)!=0) return -7;

    fdt_fb_t fb;
    int have_fb=fdt_simplefb(fdt,&fb)==0;
    if(have_fb&&mark_range(regions,&merged,KBI_FDT_MAX_MAP,fb.base,fb.size,
                           KBI_MEM_FRAMEBUFFER,0)!=0) return -7;
    if(mark_range(regions,&merged,KBI_FDT_MAX_MAP,kernel_base,kernel_size,
                  KBI_MEM_KERNEL,1)!=0) return -8;
    if(merged>KIRA_BOOTINFO_MAX_MMAP) return -9;

    /* Merge adjacent ranges only when their semantic memory type matches. */
    uint32_t compact=0;
    for(uint32_t i=0;i<merged;i++) {
        if(compact) {
            kbi_mmap_entry_t* prev=&regions[compact-1];
            uint64_t prev_end=prev->base+prev->length;
            if(prev_end>regions[i].base) return -9;
            if(prev_end==regions[i].base&&prev->type==regions[i].type) {
                if(prev->length>UINT64_MAX-regions[i].length) return -9;
                prev->length+=regions[i].length;
                continue;
            }
        }
        regions[compact++]=regions[i];
    }
    merged=compact;

    uint32_t cmd_len=0;
    const char* cmd=0;
    int chosen=fdt_find_path(fdt,"/chosen");
    if(chosen>=0) {
        uint32_t n=0;
        cmd=(const char*)fdt_prop(fdt,chosen,"bootargs",&n);
        if(cmd&&n) {
            while(cmd_len<n&&cmd[cmd_len]) cmd_len++;
            if(cmd_len==n||cmd_len+1!=n) return -10;
            cmd_len++;
        } else cmd=0;
    }

    uint64_t need64=sizeof(kbi_header_t)+(uint64_t)merged*sizeof(kbi_mmap_entry_t)+cmd_len;
    if(need64>capacity||need64>KIRA_BOOTINFO_MAX_SIZE||need64>UINT32_MAX) return -11;
    uint32_t total=(uint32_t)need64;
    __builtin_memset(out,0,total);
    kbi_header_t* h=(kbi_header_t*)out;
    h->magic=KIRA_BOOTINFO_MAGIC;
    h->version=KIRA_BOOTINFO_VERSION;
    h->header_size=sizeof(*h);
    h->total_size=total;
    h->flags=KBI_F_FDT|(cmd_len?KBI_F_CMDLINE:0);
    h->arch=KBI_ARCH_AARCH64;
    h->boot_mode=KBI_BOOT_DEVICE_TREE;
    h->mmap_off=sizeof(*h);
    h->mmap_count=merged;
    h->mmap_entry_size=sizeof(kbi_mmap_entry_t);
    h->fdt_phys=fdt_phys;
    h->kernel_phys_base=kernel_base;
    h->kernel_size=kernel_size;
    h->loader_id=FDT_MAGIC;
    kbi_mmap_entry_t* dst=(kbi_mmap_entry_t*)((uint8_t*)out+h->mmap_off);
    for(uint32_t i=0;i<merged;i++) dst[i]=regions[i];
    if(cmd_len) {
        h->cmdline_off=h->mmap_off+merged*sizeof(kbi_mmap_entry_t);
        h->cmdline_len=cmd_len;
        __builtin_memcpy((uint8_t*)out+h->cmdline_off,cmd,cmd_len);
    }
    if(have_fb) {
        h->flags|=KBI_F_FRAMEBUFFER;
        h->fb_phys=fb.base; h->fb_size=fb.size;
        h->fb_width=fb.width; h->fb_height=fb.height; h->fb_pitch=fb.stride;
        h->fb_bpp=32;
        h->fb_red_pos=16; h->fb_red_size=8;
        h->fb_green_pos=8; h->fb_green_size=8;
        h->fb_blue_pos=0; h->fb_blue_size=8;
    }
    if(kbi_validate(out,total)) return -12;
    *out_size=total;
    return 0;
}
