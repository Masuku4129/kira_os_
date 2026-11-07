/*
 * Bounded decoder for static ACPI AML Name(_PRT, Package(...)) objects.
 * Dynamic AML expressions are rejected rather than treated as guessed routes.
 */
#include "acpi_prt.h"

struct cursor { const uint8_t *p, *end; };

static int pkglen(struct cursor *c, size_t *out)
{
 uint8_t lead; unsigned n, i; size_t v;
 if (c->p >= c->end) return KIRA_AML_MALFORMED;
 lead=*c->p++; n=lead>>6;
 if ((size_t)(c->end-c->p)<n) return KIRA_AML_MALFORMED;
 if (!n) v=lead&0x3f;
 else {
  v=lead&0x0f;
  for(i=0;i<n;i++) v|=(size_t)c->p[i]<<(4+8*i);
  c->p+=n;
 }
 if(v>(size_t)(c->end-c->p)) return KIRA_AML_MALFORMED;
 *out=v; return KIRA_AML_OK;
}
static int integer(struct cursor *c, uint64_t *out)
{
 uint8_t op; unsigned n,i; uint64_t v=0;
 if(c->p>=c->end) return KIRA_AML_MALFORMED;
 op=*c->p++;
 if(op==0){*out=0;return 0;} if(op==1){*out=1;return 0;}
 if(op==0xff){*out=UINT64_MAX;return 0;}
 if(op==0x0a)n=1; else if(op==0x0b)n=2; else if(op==0x0c)n=4;
 else if(op==0x0e)n=8; else return KIRA_AML_MALFORMED;
 if((size_t)(c->end-c->p)<n)return KIRA_AML_MALFORMED;
 for(i=0;i<n;i++)v|=(uint64_t)c->p[i]<<(8*i);
 c->p+=n;*out=v;return 0;
}
static int name_string(struct cursor *c,char *out,size_t cap)
{
 size_t n=0; uint8_t lead;
 if(c->p>=c->end)return KIRA_AML_MALFORMED;
 if(*c->p==0){++c->p;if(!cap)return KIRA_AML_CAPACITY;out[0]=0;return 0;}
 if(*c->p=='\\'||*c->p=='^')return KIRA_AML_MALFORMED;
 lead=*c->p++;
 if(lead==0x2e){
  if((size_t)(c->end-c->p)<8)return KIRA_AML_MALFORMED;
  while(n<8&&n+1<cap)out[n++]=(char)*c->p++;
 } else if(lead==0x2f){
  uint8_t count;if(c->p>=c->end)return KIRA_AML_MALFORMED;
  count=*c->p++;if(!count||(size_t)(c->end-c->p)<(size_t)count*4)return KIRA_AML_MALFORMED;
  while(count--){
   unsigned j;for(j=0;j<4;j++){if(n+1>=cap)return KIRA_AML_CAPACITY;out[n++]=(char)*c->p++;}
   if(count){if(n+1>=cap)return KIRA_AML_CAPACITY;out[n++]='.';}
  }
 } else {
  if((size_t)(c->end-c->p)<3||cap<5)return KIRA_AML_MALFORMED;
  out[n++]=(char)lead;out[n++]=(char)c->p[0];out[n++]=(char)c->p[1];out[n++]=(char)c->p[2];c->p+=3;
 }
 if(!n||n>=cap)return KIRA_AML_CAPACITY;out[n]=0;return 0;
}
static int source_string(struct cursor *c,char *out,size_t cap,uint8_t *link)
{
 size_t n=0;*link=0;if(c->p>=c->end)return KIRA_AML_MALFORMED;
 if(*c->p==0x0d){
  ++c->p;while(c->p<c->end&&*c->p){if(n+1>=cap)return KIRA_AML_CAPACITY;out[n++]=(char)*c->p++;}
  if(c->p>=c->end)return KIRA_AML_MALFORMED;++c->p;out[n]=0;*link=n!=0;return 0;
 }
 if(name_string(c,out,cap)!=0)return KIRA_AML_MALFORMED;
 *link=out[0]!=0;return 0;
}
static int parse_package(struct cursor c,struct kira_acpi_prt_table *t)
{
 size_t len;uint8_t count;unsigned i;
 if(pkglen(&c,&len)!=0||c.p>=c.end)return KIRA_AML_MALFORMED;
 c.end=c.p+len;count=*c.p++;
 for(i=0;i<count;i++){
  struct cursor e;size_t el;uint8_t n;uint64_t addr,pin,index;
  struct kira_acpi_prt_entry x;
  if(c.p>=c.end||*c.p++!=0x12)return KIRA_AML_MALFORMED;
  if(pkglen(&c,&el)!=0||c.p>=c.end)return KIRA_AML_MALFORMED;
  e.p=c.p;e.end=c.p+el;c.p=e.end;n=*e.p++;
  if(n!=4||integer(&e,&addr)||integer(&e,&pin)||pin>3)return KIRA_AML_MALFORMED;
  x.device=(uint32_t)((addr>>16)&0xffff);x.function=(uint8_t)addr;x.pin=(uint8_t)pin;x.reserved=0;
  if(source_string(&e,x.source,sizeof(x.source),&x.link_device)||integer(&e,&index)||index>UINT32_MAX)return KIRA_AML_MALFORMED;
  x.source_index=(uint32_t)index;
  if(t->count==KIRA_ACPI_PRT_MAX_ENTRIES)return KIRA_AML_CAPACITY;
  t->entries[t->count++]=x;
 }
 return KIRA_AML_OK;
}
int kira_acpi_parse_prt(const uint8_t *aml,size_t length,struct kira_acpi_prt_table *t)
{
 size_t i;int found=0;
 if(!aml||!t)return KIRA_AML_MALFORMED;t->count=0;
 for(i=0;i+6<length;i++){
  struct cursor c;int rc;
  if(aml[i]!=0x08||aml[i+1]!='_'||aml[i+2]!='P'||aml[i+3]!='R'||aml[i+4]!='T'||aml[i+5]!=0x12)continue;
  c.p=aml+i+6;c.end=aml+length;rc=parse_package(c,t);if(rc)return rc;
  found=1;
 }
 return found?KIRA_AML_OK:KIRA_AML_NOT_FOUND;
}
