/*
 * Flattened Device Tree (FDT) v17 parser for ARM64 boot discovery.
 * Bounds-checks every structure/string/property access and rejects malformed
 * nesting. Unsupported properties are skipped; memory/reserved ranges and
 * essential boot metadata are retained.
 */
#include "fdt.h"
#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u
#define FDT_HEADER_SIZE 40u
struct fdt_header {
 uint32_t magic,total,off_struct,off_strings,off_rsvmap,version,last_comp;
 uint32_t boot_cpuid,size_strings,size_struct;
};
static uint32_t be32(const void *p){const uint8_t*b=p;return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];}
static uint64_t be64(const void *p){return ((uint64_t)be32(p)<<32)|be32((const uint8_t*)p+4);}
static int bounded(size_t off,size_t len,size_t total){return off<=total&&len<=total-off;}
static int overlaps(size_t a,size_t an,size_t b,size_t bn)
{return an&&bn&&a<b+bn&&b<a+an;}
static size_t align4(size_t n){return (n+3u)&~(size_t)3u;}
static int name_eq(const char *a,const char *b){size_t i=0;while(a[i]&&b[i]&&a[i]==b[i])i++;return a[i]==b[i];}
static int memory_node_name(const char *s)
{return name_eq(s,"memory")||(s[0]=='m'&&s[1]=='e'&&s[2]=='m'&&s[3]=='o'&&s[4]=='r'&&s[5]=='y'&&s[6]=='@');}
static int cpu_node_name(const char *s)
{return name_eq(s,"cpu")||(s[0]=='c'&&s[1]=='p'&&s[2]=='u'&&s[3]=='@');}
static uint64_t read_cells(const uint8_t *p,uint32_t cells)
{return cells==1?be32(p):cells==2?be64(p):0;}
static int add_region(struct kira_fdt_region *r,size_t *n,size_t cap,uint64_t base,uint64_t size)
{if(!size||UINT64_MAX-base<size||*n>=cap)return -1;r[*n].base=base;r[*n].size=size;(*n)++;return 0;}
struct node_ctx {char name[96];int memory,reserved,cpus,cpu,chosen,soc;uint32_t ac,sc;};
int kira_fdt_parse(const void *blob,size_t available,struct kira_fdt_info *o)
{
 const uint8_t *b=blob,*s,*strings;struct fdt_header h;size_t pos,str_end;
 struct node_ctx stack[32];unsigned depth=0;uint32_t ac=2,sc=1;
 if(!blob||!o||available<FDT_HEADER_SIZE)return -1;
 /* The caller may pass an uninitialized handoff structure. Initialize every
    output field before the first append or parse operation. */
 for(size_t i=0;i<sizeof(*o);i++)((uint8_t*)o)[i]=0;
 h.magic=be32(b);h.total=be32(b+4);h.off_struct=be32(b+8);h.off_strings=be32(b+12);
 h.off_rsvmap=be32(b+16);h.version=be32(b+20);h.last_comp=be32(b+24);
 h.size_strings=be32(b+32);h.size_struct=be32(b+36);
 if(h.magic!=FDT_MAGIC||h.total>available||h.total<FDT_HEADER_SIZE||
    h.version<17||h.last_comp>17||
    h.off_struct<FDT_HEADER_SIZE||h.off_strings<FDT_HEADER_SIZE||
    h.off_rsvmap<FDT_HEADER_SIZE||(h.off_struct&3u)||(h.off_rsvmap&7u)||
    !bounded(h.off_struct,h.size_struct,h.total)||
    !bounded(h.off_strings,h.size_strings,h.total)||
    h.off_rsvmap>=h.total||
    overlaps(h.off_struct,h.size_struct,h.off_strings,h.size_strings))return -2;
 s=b+h.off_struct;strings=b+h.off_strings;str_end=h.size_strings;pos=0;
 {
  size_t rp=h.off_rsvmap;unsigned n=0;int terminated=0;
  while(bounded(rp,16,h.total)&&n++<KIRA_FDT_MAX_RESERVED+1){
   uint64_t base=be64(b+rp),size=be64(b+rp+8);rp+=16;
   if(!base&&!size){terminated=1;break;}
   if(add_region(o->reserved,&o->reserved_count,KIRA_FDT_MAX_RESERVED,base,size))return -3;
  }
  if(!terminated||overlaps(h.off_rsvmap,rp-h.off_rsvmap,h.off_struct,h.size_struct)||
     overlaps(h.off_rsvmap,rp-h.off_rsvmap,h.off_strings,h.size_strings))return -3;
 }
 while(pos<h.size_struct){
  uint32_t token;
  if(!bounded(pos,4,h.size_struct))return -4;
  token=be32(s+pos);pos+=4;
  if(token==FDT_BEGIN_NODE){
   size_t start=pos,n=0;
   if(depth==32)return -5;
   while(pos<h.size_struct&&s[pos])pos++;
   if(pos==h.size_struct)return -4;
   n=pos-start;if(n>=sizeof(stack[0].name))return -5;
   for(size_t j=0;j<n;j++)stack[depth].name[j]=(char)s[start+j];
   stack[depth].name[n]=0;pos=align4(pos+1);
   stack[depth].memory=(depth&&stack[depth-1].memory)||memory_node_name(stack[depth].name);
   stack[depth].reserved=(depth&&stack[depth-1].reserved)||name_eq(stack[depth].name,"reserved-memory");
   stack[depth].cpus=(depth&&stack[depth-1].cpus)||name_eq(stack[depth].name,"cpus");
   stack[depth].cpu=stack[depth].cpus&&depth>0&&cpu_node_name(stack[depth].name);
   stack[depth].chosen=(depth&&stack[depth-1].chosen)||name_eq(stack[depth].name,"chosen");
   stack[depth].soc=(depth&&stack[depth-1].soc)||name_eq(stack[depth].name,"soc");
   stack[depth].ac=ac;stack[depth].sc=sc;depth++;continue;
  }
  if(token==FDT_END_NODE){
   if(!depth)return -4;
   /* #address-cells/#size-cells are scoped to the current bus node.
      Do not leak a child bus's cell widths into following siblings. */
   ac=stack[depth-1].ac;sc=stack[depth-1].sc;
   depth--;continue;
  }
  if(token==FDT_NOP)continue;
  if(token==FDT_END)return depth==0?0:-4;
  if(token==FDT_PROP){
   uint32_t len,no;const char *pn;const uint8_t *v;
   if(!depth||!bounded(pos,8,h.size_struct))return -4;
   len=be32(s+pos);no=be32(s+pos+4);pos+=8;
   if(no>=str_end||!bounded(pos,len,h.size_struct))return -4;
   pn=(const char *)(strings+no);
   {size_t k=0;while(no+k<str_end&&pn[k])k++;if(no+k>=str_end)return -4;}
   v=s+pos;pos=align4(pos+len);
   if(name_eq(pn,"#address-cells")&&len==4){ac=be32(v);if(ac<1||ac>2)return -6;}
   else if(name_eq(pn,"#size-cells")&&len==4){sc=be32(v);if(sc>2)return -6;}
   else if(stack[depth-1].chosen&&name_eq(pn,"bootargs")){
    size_t n=len<sizeof(o->bootargs)-1?len:sizeof(o->bootargs)-1;
    for(size_t j=0;j<n&&v[j];j++)o->bootargs[j]=(char)v[j];o->bootargs[n]=0;
   }else if(stack[depth-1].chosen&&name_eq(pn,"linux,initrd-start")){
    if(len==4)o->initrd_start=be32(v);else if(len==8)o->initrd_start=be64(v);
   }else if(stack[depth-1].chosen&&name_eq(pn,"linux,initrd-end")){
    if(len==4)o->initrd_end=be32(v);else if(len==8)o->initrd_end=be64(v);
   }else if(stack[depth-1].memory&&name_eq(pn,"reg")){
    uint32_t a=stack[depth-1].ac,z=stack[depth-1].sc;size_t stride=(a+z)*4;
    if(!stride||len%stride)return -6;
    for(size_t q=0;q<len;q+=stride)
     if(add_region(o->memory,&o->memory_count,KIRA_FDT_MAX_MEMORY,read_cells(v+q,a),read_cells(v+q+a*4,z)))return -3;
   }else if(stack[depth-1].reserved&&name_eq(pn,"reg")){
    uint32_t a=stack[depth-1].ac,z=stack[depth-1].sc;size_t stride=(a+z)*4;
    if(!stride||len%stride)return -6;
    for(size_t q=0;q<len;q+=stride)
     if(add_region(o->reserved,&o->reserved_count,KIRA_FDT_MAX_RESERVED,read_cells(v+q,a),read_cells(v+q+a*4,z)))return -3;
   }else if(stack[depth-1].cpu&&name_eq(pn,"reg")){
    uint64_t id=len==4?be32(v):len==8?be64(v):UINT64_MAX;
    if(id<=UINT32_MAX&&o->cpu_count<KIRA_FDT_MAX_CPUS)o->cpu_ids[o->cpu_count++]=(uint32_t)id;
   }else if(stack[depth-1].soc&&name_eq(pn,"compatible")&&len>=7){
    /* GIC registers are extracted from compatible node reg below by a later
       binding layer; retain no guessed address from unrelated SoC devices. */
   }
   continue;
  }
  return -4;
 }
 return -4;
}
