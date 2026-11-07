#include "bootinfo.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){printf("bootinfo FAIL:%d %s\n",__LINE__,#x);return 1;} }while(0)

static void init(uint8_t* b, kbi_header_t** hp, kbi_mmap_entry_t** mp) {
    memset(b, 0, 512);
    kbi_header_t* h=(kbi_header_t*)b;
    h->magic=KIRA_BOOTINFO_MAGIC; h->version=1; h->header_size=sizeof(*h);
    h->total_size=sizeof(*h)+sizeof(kbi_mmap_entry_t);
    h->arch=KBI_ARCH_X86_64; h->boot_mode=KBI_BOOT_UEFI;
    h->mmap_off=sizeof(*h); h->mmap_count=1;
    h->mmap_entry_size=sizeof(kbi_mmap_entry_t);
    h->kernel_phys_base=0x100000; h->kernel_size=0x200000;
    kbi_mmap_entry_t* m=(kbi_mmap_entry_t*)(b+h->mmap_off);
    m->base=0; m->length=0x9f000; m->type=KBI_MEM_USABLE;
    *hp=h; *mp=m;
}

int main(void) {
    uint8_t b[512]; kbi_header_t* h; kbi_mmap_entry_t* m;
    init(b,&h,&m);
    CHECK(kbi_validate(b,sizeof b)==0);
    CHECK(kbi_mmap(b)==m);

    h->total_size=sizeof(*h)+sizeof(*m)-1;
    CHECK(kbi_validate(b,sizeof b)==-5);
    h->total_size=sizeof(*h)+sizeof(*m);
    h->mmap_off=0xfffffff8u;
    CHECK(kbi_validate(b,sizeof b)==-5);
    h->mmap_off=sizeof(*h); m->base=UINT64_MAX-3; m->length=8;
    CHECK(kbi_validate(b,sizeof b)==-11);
    m->base=0; m->length=0x9f000;

    h->arch=99; CHECK(kbi_validate(b,sizeof b)==-3); h->arch=KBI_ARCH_X86_64;
    h->arch=KBI_ARCH_AARCH64; h->boot_mode=KBI_BOOT_BIOS;
    CHECK(kbi_validate(b,sizeof b)==-3);
    h->arch=KBI_ARCH_X86_64; h->boot_mode=KBI_BOOT_UEFI;

    h->flags=0x80000000u; CHECK(kbi_validate(b,sizeof b)==-4); h->flags=0;
    h->flags=KBI_F_FDT; h->fdt_phys=0x1003;
    CHECK(kbi_validate(b,sizeof b)==-9); h->flags=0; h->fdt_phys=0;

    h->kernel_phys_base=UINT64_MAX-4; h->kernel_size=8;
    CHECK(kbi_validate(b,sizeof b)==-10);
    h->kernel_phys_base=0x100000; h->kernel_size=0x200000;

    h->flags=KBI_F_FRAMEBUFFER; h->fb_size=0x1000; h->fb_phys=0xE0000000;
    h->fb_width=640; h->fb_height=480; h->fb_pitch=4096; h->fb_bpp=32;
    h->fb_red_pos=16; h->fb_red_size=8; h->fb_green_pos=8; h->fb_green_size=8;
    h->fb_blue_pos=0; h->fb_blue_size=8;
    CHECK(kbi_validate(b,sizeof b)==-8);
    h->fb_size=4096ULL*480;
    CHECK(kbi_validate(b,sizeof b)==0);
    h->fb_pitch=1024; CHECK(kbi_validate(b,sizeof b)==-8); h->fb_pitch=4096;
    h->fb_blue_pos=15; h->fb_blue_size=8;
    CHECK(kbi_validate(b,sizeof b)==-8);
    h->fb_blue_pos=0; h->fb_blue_size=8;
    h->fb_phys=UINT64_MAX-100; CHECK(kbi_validate(b,sizeof b)==-8);
    h->fb_phys=0xE0000000; h->flags=0; h->fb_size=0;

    /* Firmware map entries must be ordered and non-overlapping. */
    h->mmap_count=2; h->mmap_entry_size=sizeof(*m);
    h->total_size=sizeof(*h)+2*sizeof(*m);
    kbi_mmap_entry_t* m2=(kbi_mmap_entry_t*)(b+h->mmap_off+sizeof(*m));
    m2->base=0x80000; m2->length=0x1000; m2->type=KBI_MEM_RESERVED;
    CHECK(kbi_validate(b,sizeof b)==-11);
    m2->base=0x100000; CHECK(kbi_validate(b,sizeof b)==0);

    /* Module table and its name live in disjoint blob ranges. */
    h->modules_off=sizeof(*h)+2*sizeof(*m);
    h->modules_count=1; h->module_entry_size=sizeof(kbi_module_t);
    h->total_size=h->modules_off+sizeof(kbi_module_t)+5;
    kbi_module_t* mod=(kbi_module_t*)(b+h->modules_off);
    memset(mod,0,sizeof(*mod)); mod->base=0x400000; mod->length=0x1000;
    mod->name_off=h->modules_off+sizeof(*mod); mod->name_len=5;
    memcpy(b+mod->name_off,"init",5);
    CHECK(kbi_validate(b,sizeof b)==0);
    mod->base=0x200000; CHECK(kbi_validate(b,sizeof b)==-12); mod->base=0x400000;
    mod->name_off=h->modules_off; CHECK(kbi_validate(b,sizeof b)==-12);
    mod->name_off=h->modules_off+sizeof(*mod);
    /* Two module names must not alias even when both are individually in-bounds. */
    h->modules_count=2;
    h->total_size=h->modules_off+2*sizeof(kbi_module_t)+10;
    kbi_module_t* mod2=mod+1; *mod2=*mod;
    mod2->base=0x500000;
    mod->name_off=h->modules_off+2*sizeof(*mod);
    mod2->name_off=mod->name_off;
    CHECK(kbi_validate(b,sizeof b)==-12);
    mod2->name_off=mod->name_off+5;
    memcpy(b+mod->name_off,"init",5);
    memcpy(b+mod2->name_off,"init",5);
    CHECK(kbi_validate(b,sizeof b)==0);
    h->modules_count=1;
    h->total_size=h->modules_off+sizeof(kbi_module_t)+5;
    mod->name_off=h->modules_off+sizeof(*mod);
    memcpy(b+mod->name_off,"init",5);

    /* Module table cannot alias the memory map. */
    h->modules_off=h->mmap_off; CHECK(kbi_validate(b,sizeof b)==-13);
    h->modules_off=sizeof(*h)+2*sizeof(*m);

    /* Command line cannot alias a structured table. */
    h->cmdline_off=h->mmap_off+1; h->cmdline_len=1; h->flags=KBI_F_CMDLINE;
    CHECK(kbi_validate(b,sizeof b)==-13);
    h->cmdline_off=0; h->cmdline_len=0; h->flags=0;

    /* Multiboot2 is an x86 handoff, never an ARM64 boot mode. */
    h->arch=KBI_ARCH_AARCH64; h->boot_mode=KBI_BOOT_MULTIBOOT2;
    CHECK(kbi_validate(b,sizeof b)==-3);
    h->arch=KBI_ARCH_X86_64; h->boot_mode=KBI_BOOT_MULTIBOOT2;
    CHECK(kbi_validate(b,sizeof b)==0);
    h->boot_mode=KBI_BOOT_UEFI;

    puts("bootinfo: PASS");
    return 0;
}
