#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "pmm.h"

static char outbuf[1<<16]; static size_t outlen;
void uart_print(const char*s){ size_t n=strlen(s); if(outlen+n<sizeof outbuf){memcpy(outbuf+outlen,s,n);outlen+=n;} }
void uart_print_dec(uint64_t v){ char b[32]; snprintf(b,32,"%llu",(unsigned long long)v); uart_print(b);}
void uart_print_hex(uint64_t v){ char b[32]; snprintf(b,32,"0x%llX",(unsigned long long)v); uart_print(b);}

#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c); fails++;} }while(0)
static int fails;
extern void* pmm_test_store; extern uint64_t pmm_test_store_bytes;
static uint32_t store_buf[1<<18];

static uint8_t* mk_mbi(uint64_t at, int with_mmap){
    uint8_t* m = mmap((void*)at, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    if(m==MAP_FAILED){perror("mmap");exit(2);}
    memset(m,0,4096);
    uint8_t* p=m+8;
    // tag type 1 (cmdline) "x"
    *(uint32_t*)p=1; *(uint32_t*)(p+4)=9; p[8]='x'; p+=16;
    if(with_mmap){
        // Embedded boot modules: one below 1 GB and one in RAMEXT's high range.
        *(uint32_t*)p=3; *(uint32_t*)(p+4)=16; *(uint32_t*)(p+8)=0x18000000; *(uint32_t*)(p+12)=0x18400000; p+=16;
        *(uint32_t*)p=3; *(uint32_t*)(p+4)=16; *(uint32_t*)(p+8)=0x100200000ULL; *(uint32_t*)(p+12)=0x100400000ULL; p+=16;
        struct E{uint64_t a,l;uint32_t t,r;} ents[]={
            {0,0x9FC00,1,0},{0x9FC00,0x60400,2,0},{0x100000,0x1FF00000ULL,1,0},
            {0x20000000ULL,0x100000,2,0},{0x100000000ULL,0x40000000ULL,1,0}};
        uint32_t sz=16+sizeof ents;
        *(uint32_t*)p=6; *(uint32_t*)(p+4)=sz; *(uint32_t*)(p+8)=24; *(uint32_t*)(p+12)=0;
        memcpy(p+16,ents,sizeof ents); p+=(sz+7)&~7u;
    }
    *(uint32_t*)p=0; *(uint32_t*)(p+4)=8; p+=8;
    *(uint32_t*)m=(uint32_t)(p-m);
    return m;
}

int main(void){
    pmm_test_store=0; pmm_test_store_bytes=0;
    uint64_t mbi_addr=0x10000000ULL;
    // negative cases
    CHECK(pmm_init(0x1234,mbi_addr)==-1);
    CHECK(pmm_init(0x36D76289UL,0)==-2);
    CHECK(pmm_init(0x36D76289UL,0x2000000000ULL)==-2);
    uint8_t* nm=mk_mbi(0x20000000ULL+0x1000000,0);  // 0x21000000, no mmap
    CHECK(pmm_init(0x36D76289UL,0x21000000ULL)==-4);
    munmap(nm,4096);

    uint8_t* m=mk_mbi(mbi_addr,1);
    uint32_t info_size=*(uint32_t*)m;
    CHECK(pmm_init(0x36D76289UL,mbi_addr)==-5);                // нет места под карты
    pmm_test_store=store_buf; pmm_test_store_bytes=sizeof store_buf;
    CHECK(pmm_init(0x36D76289UL,mbi_addr)==0);
    uint64_t expect_pages=0x1FF00000ULL/4096 - (0x320000-0x100000)/4096 - 1 - (0x400000ULL/4096);
    printf("free kb = %u, expected %llu (info_size=%u)\n",pmm_get_free_memory_kb(),(unsigned long long)expect_pages*4,info_size);
    CHECK(pmm_get_free_memory_kb()==expect_pages*4);

    // непрерывные блоки
    {
        uint32_t kb0 = pmm_get_free_memory_kb();
        uint64_t blk = pmm_alloc_contiguous(768);                 // 3 МБ, как буфер кадра 1024x768x32
        CHECK(blk != 0 && blk % 4096 == 0 && blk >= 0x320000 && blk + 768 * 4096 <= (1ULL << 30));
        CHECK(pmm_get_free_memory_kb() == kb0 - 768 * 4);
        uint64_t one = pmm_alloc_page();                          // одиночная страница не пересекается с блоком
        CHECK(one != 0 && (one < blk || one >= blk + 768 * 4096));
        CHECK(pmm_free_page(blk + 5 * 4096) == 0);               // можно вернуть и по одной странице
        CHECK(pmm_alloc_contiguous(768) != blk);                  // дыра в блоке слишком мала -> другое место
        CHECK(pmm_alloc_contiguous(0) == 0);
        CHECK(pmm_alloc_contiguous(1u << 20) == 0);               // 4 ГБ - больше, чем есть
        CHECK(pmm_free_page(one) == 0);
        printf("contiguous 3 MB block at 0x%llX\n", (unsigned long long)blk);
    }
    // перезапуск PMM для чистого состояния дальнейших проверок
    CHECK(pmm_init(0x36D76289UL,mbi_addr)==0);

    // alloc everything, check invariants
    uint32_t n=0; uint64_t first=0, prev=0;
    static uint8_t seen[1<<18];
    for(;;){
        uint64_t p=pmm_alloc_page(); if(!p)break;
        CHECK(p%4096==0); CHECK(p>=0x320000); CHECK(p<(1ULL<<30));
        CHECK(!(p>=mbi_addr && p<mbi_addr+4096));
        CHECK(!seen[p>>12]); seen[p>>12]=1;
        if(!n) first=p;
        prev=p; n++;
    }
    printf("allocated %u pages, first=0x%llX last=0x%llX\n",n,(unsigned long long)first,(unsigned long long)prev);
    CHECK(n==expect_pages);
    CHECK(pmm_get_free_memory_kb()==0);
    // frees
    CHECK(pmm_free_page(first)==0);
    CHECK(pmm_free_page(first)==-1);          // double free
    CHECK(pmm_free_page(0x200000)==-1);       // kernel image
    CHECK(pmm_free_page(mbi_addr)==-1);       // mbi
    CHECK(pmm_free_page(0x18000000ULL)==-1); // embedded module below 1 GB
    CHECK(pmm_free_page(0x1000)==-1);         // low memory
    CHECK(pmm_free_page(first+1)==-1);        // unaligned
    CHECK(pmm_free_page(1ULL<<30)==-1);       // out of range
    CHECK(pmm_alloc_page()==first);           // reuse
    CHECK(pmm_alloc_page()==0);

    // память выше 1 ГБ: известна, но не выдаётся, пока её не отобразили
    {
        pmm_range_t r[PMM_MAX_HIGH];
        unsigned k = pmm_high_ranges(r, PMM_MAX_HIGH);
        CHECK(k == 1 && r[0].start == 0x100000000ULL && r[0].end == 0x140000000ULL);
        CHECK(pmm_managed_top() == 0x140000000ULL);
        CHECK(pmm_free_page(0x100000000ULL) == -1);            // ещё не добавлена
        CHECK(pmm_add_high(0x100000000ULL, 0x100000000ULL + (8ULL<<20)) == 1536);
        CHECK(pmm_add_high(0x100000000ULL, 0x100000000ULL + (8ULL<<20)) == 0);   // повтор ничего не меняет
        CHECK(pmm_add_high(0x13FF00000ULL, 0x140100000ULL) == -1);               // выходит за диапазон
        CHECK(pmm_add_high(0x50000000ULL, 0x50200000ULL) == -1);                 // не ОЗУ выше 1 ГБ
        CHECK(pmm_add_high(0x100000001ULL, 0x100002000ULL) == -1);               // не выровнен
        CHECK(pmm_get_free_memory_kb() == 1536 * 4);
        CHECK(pmm_free_page(0x100200000ULL) == -1); // embedded module above 1 GB
        uint64_t a = pmm_alloc_page();
        CHECK(a >= 0x100000000ULL && a < 0x100000000ULL + (8ULL<<20));
        CHECK(a < 0x100200000ULL || a >= 0x100400000ULL);
        uint64_t blk = pmm_alloc_contiguous(512);
        CHECK(blk >= 0x100000000ULL && blk + 512*4096 <= 0x100000000ULL + (8ULL<<20));
        CHECK(pmm_free_contiguous(blk, 512) == 0);
        CHECK(pmm_free_page(a) == 0);
        CHECK(pmm_free_page(0x100000000ULL + (9ULL<<20)) == -1);                 // за добавленной частью
        CHECK(pmm_free_page(0x140000000ULL) == -1);
    }
    printf("%s\n--- log ---\n%.*s", fails?"FAILED":"ALL PMM TESTS PASSED",(int)outlen,outbuf);
    return fails!=0;
}
