#include "bootinfo.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){printf("bootinfo_mb2 FAIL:%d %s\n",__LINE__,#x);return 1;} }while(0)
static void w32(uint8_t* p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void w64(uint8_t* p,uint64_t v){w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));}
int main(void){
    uint8_t b[256] __attribute__((aligned(8))); memset(b,0,sizeof b);
    /* Multiboot2: mmap, initrd module, command line, RGB framebuffer, end tag. */
    w32(b,136); w32(b+4,0);
    w32(b+8,6); w32(b+12,40); w32(b+16,24); w32(b+20,0);
    w64(b+24,0); w64(b+32,0x9f000); w32(b+40,1); w32(b+44,0);
    w32(b+48,3); w32(b+52,21); w32(b+56,0x400000); w32(b+60,0x401000);
    memcpy(b+64,"init",5);
    w32(b+72,1); w32(b+76,14); memcpy(b+80,"quiet",6);
    w32(b+88,8); w32(b+92,38); w64(b+96,0xE0000000);
    w32(b+104,2560); w32(b+108,640); w32(b+112,480);
    b[116]=32; b[117]=1; b[120]=16; b[121]=8; b[122]=8; b[123]=8; b[124]=0; b[125]=8;
    w32(b+128,0); w32(b+132,8);

    const void* out=0; uint32_t out_size=0;
    out=(const void*)(uintptr_t)1; out_size=123;
    CHECK(kbi_from_multiboot2(b,136,0,0x200000,&out,&out_size)==-10);
    CHECK(out==0 && out_size==0);
    out=(const void*)(uintptr_t)1; out_size=123;
    CHECK(kbi_from_multiboot2(b,136,0x100000,0x200000,&out,&out_size)==0);
    CHECK(out && out_size>=sizeof(kbi_header_t));
    CHECK(kbi_validate(out,out_size)==0);
    const kbi_header_t* h=(const kbi_header_t*)out;
    CHECK(h->arch==KBI_ARCH_X86_64 && h->boot_mode==KBI_BOOT_MULTIBOOT2);
    CHECK(h->mmap_count==1 && h->modules_count==1);
    CHECK(h->flags==(KBI_F_FRAMEBUFFER|KBI_F_CMDLINE));
    CHECK(h->fb_phys==0xE0000000 && h->fb_size==2560u*480u);
    CHECK(h->fb_width==640 && h->fb_height==480 && h->fb_bpp==32);
    CHECK(h->cmdline_len==6 && !strcmp((const char*)((const uint8_t*)out+h->cmdline_off),"quiet"));
    const kbi_module_t* m=kbi_modules(out);
    CHECK(m && m[0].base==0x400000 && m[0].length==0x1000);
    CHECK(!strcmp((const char*)((const uint8_t*)out+m[0].name_off),"init"));

    /* ACPI new tag is copied into aligned kernel-owned storage and represented in BootInfo. */
    {
        uint8_t with_acpi[256] __attribute__((aligned(8)));
        memcpy(with_acpi,b,128);
        w32(with_acpi,184);
        w32(with_acpi+128,15); w32(with_acpi+132,44);
        memset(with_acpi+136,0,36); memcpy(with_acpi+136,"RSD PTR ",8);
        with_acpi[143]=2; /* revision */
        w32(with_acpi+156,36);
        w32(with_acpi+176,0); w32(with_acpi+180,8);
        CHECK(kbi_from_multiboot2(with_acpi,184,0x100000,0x200000,&out,&out_size)==0);
        const kbi_header_t* ah=(const kbi_header_t*)out;
        CHECK((ah->flags & KBI_F_ACPI) && ah->rsdp_phys && !(ah->rsdp_phys & 15));
        CHECK(memcmp((const void*)(uintptr_t)ah->rsdp_phys,with_acpi+136,36)==0);
        /* Legacy RSDP tag is retained as the 20-byte ACPI 1.0 structure. */
        w32(with_acpi,168);
        w32(with_acpi+128,14); w32(with_acpi+132,28);
        w32(with_acpi+160,0); w32(with_acpi+164,8);
        CHECK(kbi_from_multiboot2(with_acpi,168,0x100000,0x200000,&out,&out_size)==0);
        ah=(const kbi_header_t*)out;
        CHECK((ah->flags & KBI_F_ACPI) && ah->rsdp_phys && !(ah->rsdp_phys & 15));
        CHECK(memcmp((const void*)(uintptr_t)ah->rsdp_phys,with_acpi+136,20)==0);
        /* When both standard tags are present, the extended ACPI 2.0+ tag wins. */
        {
            uint8_t both[256] __attribute__((aligned(8)));
            memcpy(both,b,128); w32(both,216);
            w32(both+128,14); w32(both+132,28);
            memset(both+136,0x11,20);
            w32(both+160,15); w32(both+164,44);
            memset(both+168,0x22,36);
            w32(both+208,0); w32(both+212,8);
            CHECK(kbi_from_multiboot2(both,216,0x100000,0x200000,&out,&out_size)==0);
            ah=(const kbi_header_t*)out;
            CHECK((ah->flags & KBI_F_ACPI) && memcmp((const void*)(uintptr_t)ah->rsdp_phys,both+168,36)==0);
        }
        /* Duplicate extended ACPI tags are ambiguous and rejected. */
        {
            uint8_t dup[256] __attribute__((aligned(8)));
            memcpy(dup,b,128); w32(dup,232);
            w32(dup+128,15); w32(dup+132,44); memset(dup+136,0,36);
            w32(dup+176,15); w32(dup+180,44); memset(dup+184,0,36);
            w32(dup+224,0); w32(dup+228,8);
            CHECK(kbi_from_multiboot2(dup,232,0x100000,0x200000,&out,&out_size)==-36);
        }
    }

    uint8_t valid[136]; memcpy(valid,b,sizeof valid);

    /* A module may not overlap the loaded kernel image. */
    memcpy(b,valid,sizeof valid);
    w32(b+56,0x100000); w32(b+60,0x101000);
    CHECK(kbi_from_multiboot2(b,136,0x100000,0x200000,&out,&out_size)!=0);

    /* RGB component bit ranges must be disjoint. */
    memcpy(b,valid,sizeof valid);
    b[122]=16;
    CHECK(kbi_from_multiboot2(b,136,0x100000,0x200000,&out,&out_size)!=0);

    /* Duplicate framebuffer tags are ambiguous and must fail closed. */
    memcpy(b,valid,sizeof valid);
    memcpy(b+128,b+88,40); w32(b+168,0); w32(b+172,8); w32(b,176);
    CHECK(kbi_from_multiboot2(b,176,0x100000,0x200000,&out,&out_size)!=0);

    /* The end tag is mandatory and must terminate the declared block. */
    memcpy(b,valid,sizeof valid);
    w32(b+128,9);
    CHECK(kbi_from_multiboot2(b,136,0x100000,0x200000,&out,&out_size)!=0);

    /* Misaligned and undersized caller buffers are rejected before parsing. */
    CHECK(kbi_from_multiboot2(b+1,135,0x100000,0x200000,&out,&out_size)!=0);
    CHECK(kbi_from_multiboot2(valid,135,0x100000,0x200000,&out,&out_size)!=0);

    /* Corrupt memory-map tag stride/size is rejected. */
    memcpy(b,valid,sizeof valid);
    w32(b+12,23);
    out=(const void*)(uintptr_t)1; out_size=123;
    CHECK(kbi_from_multiboot2(b,136,0x100000,0x200000,&out,&out_size)!=0);
    CHECK(out==0 && out_size==0);
    puts("bootinfo_mb2: PASS"); return 0;
}
