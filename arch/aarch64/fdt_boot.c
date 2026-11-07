#include "fdt_boot.h"
int kira_fdt_build_memory_regions(const struct kira_fdt_info *f,
 struct kira_boot_handoff *out)
{
 size_t i,j;
 if(!f||!out||!f->memory_count)return -1;
 out->raw=0;out->region_count=0;out->usable_bytes=0;
 out->has_framebuffer=0;out->has_rsdp=0;
 for(i=0;i<f->memory_count;i++){
  const struct kira_fdt_region *r=&f->memory[i];
  if(!r->size||r->base>UINT64_MAX-r->size)return -2;
  if(out->region_count==KIRA_BOOT_MAX_REGIONS)return -3;
  out->regions[out->region_count].base=r->base;
  out->regions[out->region_count].length=r->size;
  out->regions[out->region_count].kind=KIRA_MEM_USABLE;
  out->regions[out->region_count].attributes=0;
  out->usable_bytes+=r->size;out->region_count++;
 }
 /* Reserved ranges override usable RAM; split affected RAM intervals. */
 for(i=0;i<f->reserved_count;i++){
  uint64_t rb=f->reserved[i].base,re=rb+f->reserved[i].size;
  if(!f->reserved[i].size||re<rb)return -2;
  for(j=0;j<out->region_count;j++){
   struct kira_memory_region *m=&out->regions[j];
   uint64_t mb=m->base,me=mb+m->length;
   if(m->kind!=KIRA_MEM_USABLE||rb>=me||re<=mb)continue;
   if(rb<=mb&&re>=me){out->usable_bytes-=m->length;m->kind=KIRA_MEM_RESERVED;continue;}
   if(rb<=mb){
    uint64_t cut=re-mb;out->usable_bytes-=cut;m->base=re;m->length=me-re;
   }else if(re>=me){
    uint64_t cut=me-rb;out->usable_bytes-=cut;m->length=rb-mb;
   }else{
    uint64_t left=rb-mb,right=me-re,cut=re-rb;
    if(out->region_count==KIRA_BOOT_MAX_REGIONS)return -3;
    out->usable_bytes-=cut;
    out->regions[out->region_count].base=re;
    out->regions[out->region_count].length=right;
    out->regions[out->region_count].kind=KIRA_MEM_USABLE;
    out->regions[out->region_count].attributes=0;
    out->region_count++;
    m->length=left;
    /* Insert reserved middle as a distinct region. */
    if(out->region_count==KIRA_BOOT_MAX_REGIONS)return -3;
    out->regions[out->region_count].base=rb;
    out->regions[out->region_count].length=cut;
    out->regions[out->region_count].kind=KIRA_MEM_RESERVED;
    out->regions[out->region_count].attributes=0;
    out->region_count++;
   }
  }
 }
 return 0;
}
