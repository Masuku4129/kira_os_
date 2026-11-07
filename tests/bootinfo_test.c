#include "bootinfo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)){printf("bootinfo FAIL:%d %s\n",__LINE__,#x);return 1;} }while(0)
int main(void) {
    uint8_t b[512]={0}; kbi_header_t* h=(kbi_header_t*)b;
    h->magic=KIRA_BOOTINFO_MAGIC; h->version=1; h->header_size=sizeof(*h);
    h->total_size=sizeof(*h)+sizeof(kbi_mmap_entry_t)+8;
    h->arch=KBI_ARCH_X86_64; h->boot_mode=KBI_BOOT_UEFI;
    h->mmap_off=sizeof(*h); h->mmap_count=1; h->mmap_entry_size=sizeof(kbi_mmap_entry_t);
    h->kernel_phys_base=0x100000; h->kernel_size=0x200000;
    kbi_mmap_entry_t* m=(kbi_mmap_entry_t*)(b+h->mmap_off);
    m->base=0; m->length=0x9f000; m->type=KBI_MEM_USABLE;
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
    h->kernel_phys_base=UINT64_MAX-4; h->kernel_size=8;
    CHECK(kbi_validate(b,sizeof b)==-10);
    h->kernel_phys_base=0x100000; h->kernel_size=0x200000;
    h->flags=KBI_F_FRAMEBUFFER; h->fb_size=0x1000; h->fb_phys=0xE0000000;
    h->fb_width=640; h->fb_height=480; h->fb_pitch=4096; h->fb_bpp=32;
    h->fb_red_pos=16; h->fb_red_size=8; h->fb_green_pos=8; h->fb_green_size=8;
    h->fb_blue_pos=0; h->fb_blue_size=8;
    CHECK(kbi_validate(b,sizeof b)==-8); /* pitch*height exceeds framebuffer size */
    h->fb_size=4096ULL*480;
    CHECK(kbi_validate(b,sizeof b)==0);
    h->fb_blue_pos=31; h->fb_blue_size=8;
    CHECK(kbi_validate(b,sizeof b)==-8);
    puts("bootinfo: PASS"); return 0;
}
