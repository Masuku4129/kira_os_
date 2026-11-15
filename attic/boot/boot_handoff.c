/*
 * Normalize bootloader handoff data before the physical memory manager starts.
 * Unknown firmware descriptor types are retained as reserved, never usable.
 */
#include "boot_handoff.h"

static int add_overflow_u64(uint64_t a,uint64_t b,uint64_t *r)
{ *r=a+b; return *r<a; }

int kira_boot_handoff_init(struct kira_boot_handoff *out,
                           const struct kira_bootinfo *info,
                           uint64_t available_size,
                           const struct kira_boot_map_ops *ops)
{
 uint64_t end,offset;size_t count=0;
 if(!out||!kira_bootinfo_valid(info,available_size))return -1;
 out->raw=info;out->region_count=0;out->usable_bytes=0;
 out->has_framebuffer=0;out->has_rsdp=info->rsdp_address!=0;
 if(info->framebuffer.address&&info->framebuffer.byte_size&&
    info->framebuffer.width&&info->framebuffer.height&&
    info->framebuffer.pixels_per_scanline>=info->framebuffer.width&&
    (info->framebuffer.format==KIRA_FB_RGBX8||
     info->framebuffer.format==KIRA_FB_BGRX8))
  out->has_framebuffer=1;
 if(!info->memory.address||!info->memory.byte_size||
    info->memory.descriptor_size<sizeof(uint32_t)||
    info->memory.descriptor_size>4096||
    info->memory.byte_size%info->memory.descriptor_size||
    !ops||!ops->decode_descriptor)return -2;
 count=(size_t)(info->memory.byte_size/info->memory.descriptor_size);
 if(count>KIRA_BOOT_MAX_REGIONS)return -3;
 for(offset=0;offset<info->memory.byte_size;offset+=info->memory.descriptor_size){
  struct kira_memory_region region;
  const void *desc=(const void *)(uintptr_t)(info->memory.address+offset);
  region.base=0;region.length=0;region.kind=KIRA_MEM_RESERVED;region.attributes=0;
  if(ops->decode_descriptor(ops->context,desc,
       info->memory.descriptor_version,&region)!=0)continue;
  if(!region.length||add_overflow_u64(region.base,region.length,&end))return -4;
  if(region.kind<KIRA_MEM_USABLE||region.kind>KIRA_MEM_BAD)
   region.kind=KIRA_MEM_RESERVED;
  if(region.kind==KIRA_MEM_USABLE){
   if(UINT64_MAX-out->usable_bytes<region.length)return -4;
   out->usable_bytes+=region.length;
  }
  out->regions[out->region_count++]=region;
 }
 return out->region_count?0:-5;
}
