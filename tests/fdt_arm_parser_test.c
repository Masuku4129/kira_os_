#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../arch/aarch64/fdt.h"

#define MAGIC 0xd00dfeedu
static uint8_t structure[512], strings[128], blob[1024];
static size_t sp, tp;
static void be32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void token(uint32_t v){be32(structure+sp,v);sp+=4;}
static void begin(const char *n){token(1);size_t z=strlen(n)+1;memcpy(structure+sp,n,z);sp+=(z+3)&~(size_t)3;}
static void endnode(void){token(2);}
static uint32_t stroff(const char *n){size_t p=0;while(p<tp){if(!strcmp((char*)strings+p,n))return (uint32_t)p;p+=strlen((char*)strings+p)+1;}uint32_t r=(uint32_t)tp;size_t z=strlen(n)+1;memcpy(strings+tp,n,z);tp+=z;return r;}
static void prop(const char *n,uint32_t v){token(3);be32(structure+sp,4);be32(structure+sp+4,stroff(n));sp+=8;be32(structure+sp,v);sp+=4;}
static void reg1(uint32_t value){token(3);be32(structure+sp,4);be32(structure+sp+4,stroff("reg"));sp+=8;be32(structure+sp,value);sp+=4;}
static void reg2(uint32_t base,uint32_t size){uint8_t v[8];be32(v,base);be32(v+4,size);token(3);be32(structure+sp,8);be32(structure+sp+4,stroff("reg"));sp+=8;memcpy(structure+sp,v,8);sp+=8;}
static size_t make_blob(void){
 sp=tp=0;begin("");
 prop("#address-cells",1);prop("#size-cells",1);
 begin("cpus");prop("#address-cells",1);prop("#size-cells",0);
 begin("cpu@3");reg1(3);endnode();endnode();
 begin("bus@0");prop("#address-cells",2);prop("#size-cells",2);
 begin("device@0");endnode();endnode();
 begin("memory@1000");reg2(0x1000,0x2000);endnode();
 endnode();token(9);
 size_t off_struct=72,off_strings=off_struct+sp,total=off_strings+tp;
 memset(blob,0,sizeof(blob));be32(blob,MAGIC);be32(blob+4,(uint32_t)total);
 be32(blob+8,(uint32_t)off_struct);be32(blob+12,(uint32_t)off_strings);
 be32(blob+16,40);be32(blob+20,17);be32(blob+24,16);
 be32(blob+32,(uint32_t)tp);be32(blob+36,(uint32_t)sp);
 be32(blob+40,0);be32(blob+44,0x800);be32(blob+48,0);be32(blob+52,0x100);
 be32(blob+56,0);be32(blob+60,0);be32(blob+64,0);be32(blob+68,0);
 memcpy(blob+off_struct,structure,sp);memcpy(blob+off_strings,strings,tp);return total;
}
int main(void){
 size_t n=make_blob();struct kira_fdt_info out;memset(&out,0xa5,sizeof(out));
 int rc=kira_fdt_parse(blob,n,&out);
 if(rc||out.reserved_count!=1||out.reserved[0].base!=0x800||
    out.reserved[0].size!=0x100||out.memory_count!=1||
    out.memory[0].base!=0x1000||out.memory[0].size!=0x2000||
    out.cpu_count!=1||out.cpu_ids[0]!=3){
  printf("FAIL: parse=%d reserved=%zu memory=%zu base=%llx size=%llx\n",rc,
   out.reserved_count,out.memory_count,(unsigned long long)out.memory[0].base,
   (unsigned long long)out.memory[0].size);return 1;
 }
 /* Header-controlled blocks must not alias one another. */
 uint8_t saved[4];
 memcpy(saved,blob+16,4);be32(blob+16,72);
 memset(&out,0xa5,sizeof(out));
 if(kira_fdt_parse(blob,n,&out)==0){puts("FAIL: reserve map overlapping structure accepted");return 1;}
 memcpy(blob+16,saved,4);
 memcpy(saved,blob+12,4);be32(blob+12,72);
 if(kira_fdt_parse(blob,n,&out)==0){puts("FAIL: strings overlapping structure accepted");return 1;}
 memcpy(blob+12,saved,4);
 puts("arm64 FDT parser: initialized outputs, reserve map, zero-size-cells CPU node, scoped cells, memory@unit-address and block overlap checks passed");
 return 0;
}
