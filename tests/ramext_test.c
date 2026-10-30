#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ramext.h"
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c); fails++;} }while(0)
static int fails;
static int budget;
static uint64_t alloc_pd(void){ if(budget<=0) return 0; budget--; void* p=aligned_alloc(4096,4096); return (uint64_t)(uintptr_t)p; }
#define MB (1ULL<<20)
#define GB (1ULL<<30)
#define NX (1ULL<<63)
static uint64_t* pd_of(uint64_t* pdpt,unsigned g){ return (uint64_t*)(uintptr_t)(pdpt[g]&0x000FFFFFFFFFF000ULL); }
int main(void){
    uint64_t* pdpt=aligned_alloc(4096,4096); memset(pdpt,0,4096);
    uint64_t ms,me;
    // 4..5 ГБ целиком
    budget=10;
    me=ramext_map(pdpt,4*GB,5*GB,alloc_pd,&ms);
    CHECK(ms==4*GB && me==5*GB && budget==9);
    CHECK((pdpt[4]&1) && (pdpt[4]&2));
    for(unsigned i=0;i<512;i++){ uint64_t e=pd_of(pdpt,4)[i]; CHECK(e==((4*GB+i*2*MB)|1|2|(1<<7)|NX)); }
    CHECK(pdpt[3]==0 && pdpt[5]==0);
    // края: 1 ГБ + 1 МБ .. 2 ГБ + 3 МБ -> 1ГБ+2МБ .. 2ГБ+2МБ, два PD
    me=ramext_map(pdpt,GB+MB,2*GB+3*MB,alloc_pd,&ms);
    CHECK(ms==GB+2*MB && me==2*GB+2*MB);
    CHECK(pd_of(pdpt,1)[0]==0 && pd_of(pdpt,1)[1]!=0 && pd_of(pdpt,2)[0]!=0 && pd_of(pdpt,2)[1]==0);
    // слишком короткий (меньше 2 МБ после сужения)
    me=ramext_map(pdpt,6*GB+MB,6*GB+3*MB-4096,alloc_pd,&ms);
    CHECK(me==0 && ms==0 && pdpt[6]==0);
    // ниже 1 ГБ и выше 512 ГБ отвергаются
    me=ramext_map(pdpt,0,GB,alloc_pd,&ms); CHECK(me==0);
    me=ramext_map(pdpt,511*GB,513*GB,alloc_pd,&ms); CHECK(me==0);
    // нет памяти под PD: часть отображена, остальное нет
    budget=1;
    me=ramext_map(pdpt,8*GB,10*GB,alloc_pd,&ms);
    CHECK(ms==8*GB && me==9*GB && pdpt[9]==0);
    budget=0;
    me=ramext_map(pdpt,20*GB,21*GB,alloc_pd,&ms); CHECK(me==ms);
    // занятая запись PD останавливает отображение, чужое не затирается
    budget=3;
    me=ramext_map(pdpt,12*GB,13*GB,alloc_pd,&ms); CHECK(me==13*GB);
    pd_of(pdpt,12)[5]=0; pd_of(pdpt,12)[6]=0x12345|1;
    me=ramext_map(pdpt,12*GB+10*MB,12*GB+20*MB,alloc_pd,&ms);
    CHECK(ms==12*GB+10*MB && me==12*GB+12*MB);
    CHECK(pd_of(pdpt,12)[6]==(0x12345|1));
    // чтение ACPI: наружу до 2 МБ, только чтение + NX, существующее не трогаем
    budget=4;
    CHECK(ramext_map_ro(pdpt,20*GB+5*MB+123,20*GB+5*MB+4096,alloc_pd)==1);
    CHECK(pd_of(pdpt,20)[2]==((20*GB+4*MB)|1|(1<<7)|NX) && pd_of(pdpt,20)[3]==0);
    uint64_t before=pd_of(pdpt,4)[7];
    CHECK(ramext_map_ro(pdpt,4*GB+14*MB,4*GB+16*MB+1,alloc_pd)==1 && pd_of(pdpt,4)[7]==before && (pd_of(pdpt,4)[8]&1));
    budget=0;
    CHECK(ramext_map_ro(pdpt,30*GB,30*GB+4096,alloc_pd)==0);
    CHECK(ramext_map_ro(pdpt,600*GB,600*GB+4096,alloc_pd)==0);
    printf("%s\n",fails?"FAILED":"ALL RAMEXT TESTS PASSED");
    return fails!=0;
}
