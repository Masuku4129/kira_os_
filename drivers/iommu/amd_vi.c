/*
 * AMD IOMMU IVRS ACPI table parser.
 * Supports IVHD hardware blocks and IVMD memory-definition blocks, including
 * device-ID ranges and single-device entries. Unknown entry types are rejected
 * so the caller never assumes that a device is covered by remapping.
 */
#include "amd_vi.h"
static uint8_t sum8(const uint8_t*p,size_t n){uint8_t s=0;while(n--)s=(uint8_t)(s+*p++);return s;}
static uint16_t le16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t*p){return (uint64_t)le32(p)|((uint64_t)le32(p+4)<<32);}
static int bounds(size_t o,size_t n,size_t t){return o<=t&&n<=t-o;}
int kira_amdvi_parse_ivrs(const void*table,size_t length,struct kira_amdvi_inventory*out)
{
 const uint8_t*b=table;size_t off;
 if(!b||!out||length<48||b[0]!='I'||b[1]!='V'||b[2]!='R'||b[3]!='S')return KIRA_AMDVI_INVALID;
 if(le32(b+4)>length||le32(b+4)<48)return KIRA_AMDVI_INVALID;
 length=le32(b+4);if(sum8(b,length)!=0)return KIRA_AMDVI_CHECKSUM;
 out->unit_count=out->device_count=out->memory_range_count=0;out->host_address_width=0;
 off=48;
 while(off<length){
  uint8_t type;uint16_t block_len;
  if(!bounds(off,4,length))return KIRA_AMDVI_INVALID;
  type=b[off];block_len=le16(b+off+2);
  if(block_len<4||!bounds(off,block_len,length))return KIRA_AMDVI_INVALID;
  if(type==0x10||type==0x11||type==0x40){
   struct kira_amdvi_unit u;
   if(block_len<24||out->unit_count==KIRA_AMDVI_MAX_UNITS)return KIRA_AMDVI_CAPACITY;
   u.flags=b[off+4];u.segment=le16(b+off+6);u.register_base=le64(b+off+8);
   u.first_device=0;u.last_device=0xffff;
   if(!u.register_base||(u.register_base&0xfffu))return KIRA_AMDVI_INVALID;
   if(!out->host_address_width)out->host_address_width=(uint8_t)(b[off+5]&0x7fu);
   out->units[out->unit_count++]=u;
   {
    size_t p=off+24;
    while(p<off+block_len){
     uint8_t entry=b[p];size_t n=4;uint16_t id=0;
     if(entry==0x02||entry==0x42){if(!bounds(p,4,off+block_len))return KIRA_AMDVI_INVALID;id=le16(b+p+1);}
     else if(entry==0x03||entry==0x43){if(!bounds(p,8,off+block_len))return KIRA_AMDVI_INVALID;id=le16(b+p+1);n=8;}
     else if(entry==0x04||entry==0x44){if(!bounds(p,8,off+block_len))return KIRA_AMDVI_INVALID;id=le16(b+p+1);n=8;}
     else if(entry==0x01||entry==0x00||entry==0x46){n=4;}
     else return KIRA_AMDVI_UNSUPPORTED;
     if(out->device_count<KIRA_AMDVI_MAX_DEVICES){
      struct kira_amdvi_device*d=&out->devices[out->device_count++];
      d->device_id=id;d->flags=entry;d->used=1;
     }else return KIRA_AMDVI_CAPACITY;
     p+=n;
    }
   }
  }else if(type==0x20||type==0x21||type==0x22){
   uint64_t base,size;
   if(block_len<32||out->memory_range_count==KIRA_AMDVI_MAX_RANGES)return KIRA_AMDVI_CAPACITY;
   base=le64(b+off+16);size=le64(b+off+24);
   if(!size||UINT64_MAX-base<size)return KIRA_AMDVI_INVALID;
   out->memory_ranges[out->memory_range_count][0]=base;
   out->memory_ranges[out->memory_range_count][1]=size;
   out->memory_range_count++;
  }else return KIRA_AMDVI_UNSUPPORTED;
  off+=block_len;
 }
 return out->unit_count?KIRA_AMDVI_OK:KIRA_AMDVI_UNSUPPORTED;
}
int kira_amdvi_reserve_exclusions(const struct kira_amdvi_inventory*i,const struct kira_amdvi_ops*o)
{
 size_t n;if(!i||!o||!o->reserve)return KIRA_AMDVI_INVALID;
 for(n=0;n<i->memory_range_count;n++){
  uint64_t base=i->memory_ranges[n][0],size=i->memory_ranges[n][1];
  if(o->reserve(o->context,base,size)!=0){if(o->report)o->report(o->context,KIRA_AMDVI_INVALID,base);return KIRA_AMDVI_INVALID;}
  if(o->report)o->report(o->context,KIRA_AMDVI_OK,base);
 }
 return KIRA_AMDVI_OK;
}
