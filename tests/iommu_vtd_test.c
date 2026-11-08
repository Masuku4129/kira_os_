#include "iommu_vtd.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void w64(uint8_t*p,uint64_t v){for(int i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static void fix(uint8_t*p,uint32_t n){p[9]=0;uint8_t s=0;for(uint32_t i=0;i<n;i++)s=(uint8_t)(s+p[i]);p[9]=(uint8_t)(0-s);}
int main(void){
 uint8_t b[104]; memset(b,0,sizeof b); memcpy(b,"DMAR",4); w32(b+4,sizeof b); b[8]=1; b[36]=47; b[37]=1;
 /* DRHD: include-all, segment 0, aligned MMIO base, one endpoint scope (00:1f.0). */
 w16(b+48,0);w16(b+50,24);b[52]=1;w16(b+54,0);w64(b+56,0xFED90000);
 b[64]=1;b[65]=8;b[68]=0;b[69]=0;b[70]=31;b[71]=0;
 /* RMRR: 0x100000..0x100fff, one endpoint scope (00:02.0). */
 w16(b+72,1);w16(b+74,32);w16(b+78,0);w64(b+80,0x100000);w64(b+88,0x100fff);
 b[96]=1;b[97]=8;b[100]=0;b[101]=0;b[102]=2;b[103]=0;
 fix(b,sizeof b);
 vtd_dmar_info_t info;
 CHECK(vtd_dmar_parse(b,sizeof b,&info)==0);
 CHECK(info.host_address_width==47 && info.flags==1 && info.unit_count==1 && info.rmrr_count==1);
 CHECK(info.units[0].include_all && info.units[0].register_base==0xFED90000 && info.units[0].scope_count==1);
 CHECK(info.scopes[0].path[0].device==31 && info.scopes[0].path[0].function==0);
 CHECK(info.rmrr[0].base==0x100000 && info.rmrr[0].limit==0x100fff && info.rmrr[0].scope_count==1);
 CHECK(vtd_dmar_parse(b,47,&info)<0);
 b[0]='X'; CHECK(vtd_dmar_parse(b,sizeof b,&info)==-3); b[0]='D';
 b[9]^=1; CHECK(vtd_dmar_parse(b,sizeof b,&info)==-5); b[9]^=1;
 w32(b+4,0xffffffffu); CHECK(vtd_dmar_parse(b,sizeof b,&info)==-4); w32(b+4,sizeof b); fix(b,sizeof b);
 w16(b+50,3); fix(b,sizeof b); CHECK(vtd_dmar_parse(b,sizeof b,&info)==-7); w16(b+50,24); fix(b,sizeof b);
 b[70]=32; fix(b,sizeof b); CHECK(vtd_dmar_parse(b,sizeof b,&info)<0);
 /* Fuzz every structure byte with valid checksum: parser must reject or safely accept. */
 for(unsigned i=0;i<50000;i++){uint8_t c[sizeof b];memcpy(c,b,sizeof b);unsigned p=48+(i*37u)%56;c[p]^=(uint8_t)(1u+(i%255));fix(c,sizeof c);(void)vtd_dmar_parse(c,sizeof c,&info);}
 /* Device-to-unit and device-to-RMRR lookup (include-all fallback, explicit scope wins, bridge scopes). */
 b[70]=31; fix(b,sizeof b);
 CHECK(vtd_dmar_parse(b,sizeof b,&info)==0 && info.units[0].register_bytes==4096);
 CHECK(vtd_dmar_unit_of(&info,0,0,31,0,0,0)==0 && vtd_dmar_unit_of(&info,0,5,1,0,0,0)==0);   /* include-all covers everything on the segment */
 CHECK(vtd_dmar_unit_of(&info,1,0,31,0,0,0)==-1);                                           /* other segment: no unit */
 CHECK(vtd_dmar_rmrr_of(&info,0,0,2,0,0,0)==0 && vtd_dmar_rmrr_of(&info,0,0,3,0,0,0)==-1);
 {
  /* Two units: unit 0 names 00:03.0 and everything behind bridge 00:1c.0 (buses 2..4); unit 1 is include-all. */
  uint8_t t[160]; memset(t,0,sizeof t); memcpy(t,"DMAR",4); t[8]=1; t[36]=47;
  w16(t+48,0); w16(t+50,16+8+8); t[52]=0; w64(t+56,0xFED90000);                 /* DRHD 0, two scopes */
  t[64]=1; t[65]=8; t[69]=0; t[70]=3; t[71]=0;                                    /* endpoint 00:03.0 */
  t[72]=2; t[73]=8; t[77]=0; t[78]=0x1c; t[79]=0;                                  /* bridge 00:1c.0 */
  w16(t+80,0); w16(t+82,16); t[84]=1; w64(t+88,0xFED91000);                      /* DRHD 1, include-all */
  w32(t+4,96); fix(t,96);
  vtd_dmar_info_t m; CHECK(vtd_dmar_parse(t,96,&m)==0 && m.unit_count==2);
  CHECK(vtd_dmar_unit_of(&m,0,0,3,0,0,0)==0);
  CHECK(vtd_dmar_unit_of(&m,0,0,4,0,0,0)==1);
  CHECK(vtd_dmar_unit_of(&m,0,0,0x1c,0,0,0)==0);                                  /* the bridge itself */
  CHECK(vtd_dmar_unit_of(&m,0,3,0,0,0,0)==1);                                     /* behind the bridge, but no topology callback: not matched */
  /* second include-all in the same segment is malformed */
  w16(t+48,0); t[52]=1; fix(t,96); CHECK(vtd_dmar_parse(t,96,&m)<0);
  /* DRHD size field above the supported window is refused */
  t[52]=0; t[53]=9; fix(t,96); CHECK(vtd_dmar_parse(t,96,&m)<0);
 }
 puts("VT-d DMAR parser: topology, bounds, checksums and malformed-input fuzz passed");
 return 0;
}
