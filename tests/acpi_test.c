#include "acpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
void uart_print(const char*s){(void)s;}
void uart_print_dec(uint64_t v){(void)v;}
void uart_print_hex(uint64_t v){(void)v;}
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
static void fix(uint8_t* t, uint32_t len, uint32_t csum_off){ t[csum_off]=0; uint8_t s=0; for(uint32_t i=0;i<len;i++) s+=t[i]; t[csum_off]=(uint8_t)(0-s); }
static void hdr(uint8_t* t, const char* sig, uint32_t len){ memset(t,0,len); memcpy(t,sig,4); memcpy(t+4,&len,4); t[8]=1; }
static uint64_t hook_base; static uint8_t* hook_buf; static int hook_calls;
static const void* hook(uint64_t phys, uint64_t len){ hook_calls++; if(!hook_buf || phys < hook_base || phys + len > hook_base + 0x700) return 0; return hook_buf + (phys - hook_base); }
int main(void){
    uint8_t* m = mmap((void*)0x10000000, 1<<16, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    CHECK(m == (uint8_t*)0x10000000);
    uint64_t base = 0x10000000;
    uint8_t *rsdp=m, *xsdt=m+0x100, *madt=m+0x200, *hpet=m+0x400, *mcfg=m+0x500;
    /* MADT: 2 CPU (один отключён), 1 IOAPIC, 1 override */
    uint32_t mlen=44+8+8+12+10+16+16; hdr(madt,"APIC",mlen);
    uint32_t la=0xFEE00000; memcpy(madt+36,&la,4);
    uint8_t* e=madt+44; e[0]=0;e[1]=8;e[2]=0;e[3]=0;e[4]=1;           /* CPU0 enabled */
    e+=8; e[0]=0;e[1]=8;e[2]=1;e[3]=1;e[4]=0;                         /* CPU1 disabled */
    e+=8; e[0]=1;e[1]=12;e[2]=2; uint32_t ia=0xFEC00000; memcpy(e+4,&ia,4);
    e+=12; e[0]=2;e[1]=10;e[2]=0;e[3]=0;{uint32_t g=2;memcpy(e+4,&g,4);} e[8]=0;e[9]=0;
    e+=10; e[0]=9;e[1]=16;{uint32_t x=300,fl=1;memcpy(e+4,&x,4);memcpy(e+8,&fl,4);}   /* x2APIC, ID 300 */
    e+=16; e[0]=9;e[1]=16;{uint32_t x=0,fl=1;memcpy(e+4,&x,4);memcpy(e+8,&fl,4);}     /* тот же CPU0 ещё раз - не считать */
    fix(madt,mlen,9);
    hdr(hpet,"HPET",56); fix(hpet,56,9);
    hdr(mcfg,"MCFG",60); uint64_t ecam=0xB0000000ull; memcpy(mcfg+44,&ecam,8); mcfg[44+10]=0; mcfg[44+11]=255; fix(mcfg,60,9);
    uint32_t xlen=36+3*8; hdr(xsdt,"XSDT",xlen);
    uint64_t a1=base+0x200,a2=base+0x400,a3=base+0x500; memcpy(xsdt+36,&a1,8);memcpy(xsdt+44,&a2,8);memcpy(xsdt+52,&a3,8); fix(xsdt,xlen,9);
    memset(rsdp,0,36); memcpy(rsdp,"RSD PTR ",8); rsdp[15]=2; uint32_t rl=36; memcpy(rsdp+20,&rl,4); uint64_t xa=base+0x100; memcpy(rsdp+24,&xa,8);
    fix(rsdp,20,8); fix(rsdp,36,32);
    acpi_info_t i;
    CHECK(acpi_parse(base, base+(1<<16), &i)==0);
    /* RSDP length and root entry array must stay within the supplied physical window. */
    { uint32_t save; memcpy(&save, rsdp+20, 4); uint32_t huge=0xFFFFFFFFu; memcpy(rsdp+20, &huge, 4);
      CHECK(acpi_parse(base, base+(1<<16), &i)==-4); memcpy(rsdp+20, &save, 4); }
    CHECK(acpi_parse(UINT64_MAX-7, base+(1<<16), &i)==-1);
    { uint32_t save; memcpy(&save, xsdt+4, 4); uint32_t bad=61; memcpy(xsdt+4, &bad, 4); fix(xsdt,bad,9);
      CHECK(acpi_parse(base, base+(1<<16), &i)==-6); memcpy(xsdt+4, &save, 4); fix(xsdt,save,9); }
    CHECK(acpi_parse(base, base+(1<<16), &i)==0);
    CHECK(i.cpu_count==2 && i.cpu_apic_id[0]==0 && i.cpu_apic_id[1]==300 && i.max_apic_id==300);
    CHECK(i.valid && i.revision==2 && i.ioapic_count==1 && i.ioapic[0].addr==0xFEC00000);
    CHECK(i.lapic_addr==0xFEE00000 && i.irq_override_count==1 && i.has_hpet && i.mcfg_base==ecam && i.mcfg_bus_end==255);
    CHECK(i.tables_seen==3 && i.tables_bad==0);
    CHECK(i.iso_valid[0]==1 && i.iso_gsi[0]==2 && !i.iso_valid[1]);
    madt[50]^=0xFF;                       /* порча MADT: должна быть отвергнута, остальное разобрано */
    CHECK(acpi_parse(base, base+(1<<16), &i)==0 && i.tables_bad==1 && i.cpu_count==0 && i.has_hpet);
    madt[50]^=0xFF;
    rsdp[3]='X'; CHECK(acpi_parse(base, base+(1<<16), &i)==-2); rsdp[3]=' ';
    rsdp[10]^=1; CHECK(acpi_parse(base, base+(1<<16), &i)==-3); rsdp[10]^=1;
    CHECK(acpi_parse(base, base+0x120, &i)==-5);   /* корень вне доступной памяти */
    /* корень и таблицы выше тождественной карты: читаются через хук ядра (там они в другом месте адресного пространства) */
    {
        static uint8_t hi[0x700];
        memcpy(hi, m, sizeof hi);                       /* те же таблицы «в высокой памяти» */
        hook_base = base; hook_buf = hi; hook_calls = 0;
        acpi_set_map_hook(hook);
        CHECK(acpi_parse(base, base + 0x40, &i) == 0 && i.cpu_count == 2 && i.tables_seen == 3 && hook_calls >= 4);
        hook_buf = 0;                                    /* ядро не смогло отобразить: таблица пропущена, не падение */
        CHECK(acpi_parse(base, base + 0x40, &i) == -5);
        acpi_set_map_hook(0);
    }
    CHECK(acpi_parse(0, 1<<30, &i)==-1);
    /* ---- таблицы Intel MP (как у SeaBIOS при выключенном ACPI) ---- */
    {
        uint8_t* fps = m + 0x800; uint8_t* ct = m + 0x900;
        memset(fps, 0, 16); memcpy(fps, "_MP_", 4); uint32_t ca = (uint32_t)(base + 0x900); memcpy(fps + 4, &ca, 4); fps[8] = 1; fps[9] = 4; fix(fps, 16, 10);
        memset(ct, 0, 0x200); memcpy(ct, "PCMP", 4); ct[6] = 4; memcpy(ct + 8, "KIRATEST", 8);
        uint32_t lap = 0xFEE00000; memcpy(ct + 36, &lap, 4);
        uint8_t* q = ct + 44; uint16_t cnt = 0;
        for (int c = 0; c < 4; c++) { q[0] = 0; q[1] = (uint8_t)c; q[2] = 0x14; q[3] = (uint8_t)(c == 2 ? 0 : (c == 0 ? 3 : 1)); q += 20; cnt++; }  /* CPU2 выключен */
        q[0] = 1; q[1] = 0; memcpy(q + 2, "PCI   ", 6); q += 8; cnt++;
        q[0] = 1; q[1] = 1; memcpy(q + 2, "ISA   ", 6); q += 8; cnt++;
        q[0] = 2; q[1] = 4; q[2] = 0x11; q[3] = 1; { uint32_t ioa = 0xFEC00000; memcpy(q + 4, &ioa, 4); } q += 8; cnt++;
        uint8_t ints[][8] = {
            { 3, 0, 0, 0, 1, 0, 4, 2 },                    /* ISA IRQ0 -> вход 2 (таймер) */
            { 3, 0, 0, 0, 1, 1, 4, 1 },                    /* IRQ1 -> 1 */
            { 3, 0, 0x0F, 0, 1, 9, 4, 9 },                 /* IRQ9: уровень, активный низкий */
            { 3, 3, 0, 0, 1, 0, 4, 0 },                    /* ExtINT - не прерывание устройства */
            { 3, 0, 0x0F, 0, 0, 0x0C, 4, 16 },             /* шина PCI - не ISA */
            { 4, 3, 0, 0, 1, 0, 0xFF, 0 },                 /* локальное - пропуск */
        };
        for (unsigned k = 0; k < sizeof ints / 8; k++) { memcpy(q, ints[k], 8); q += 8; cnt++; }
        uint16_t tl = (uint16_t)(q - ct); memcpy(ct + 4, &tl, 2); memcpy(ct + 34, &cnt, 2); fix(ct, tl, 7);
        CHECK(acpi_scan(base, base + 0x1000, "_MP_", 4, 0) == base + 0x800);
        CHECK(acpi_scan(base, base + 0x1000, "RSD PTR ", 8, 20) == base);
        acpi_info_t mp; memset(&mp, 0, sizeof mp);
        CHECK(mp_parse(base + 0x800, base + (1 << 16), &mp) == 0);
        CHECK(mp.legacy_isa);                              /* в таблице есть шина ISA */
        CHECK(mp.valid && mp.source == 1 && mp.cpu_count == 3 && mp.cpu_apic_id[2] == 3 && mp.max_apic_id == 3 && mp.lapic_addr == 0xFEE00000);
        CHECK(mp.ioapic_count == 1 && mp.ioapic[0].id == 4 && mp.ioapic[0].addr == 0xFEC00000 && mp.ioapic[0].gsi_base == 0);
        CHECK(mp.iso_valid[0] && mp.iso_gsi[0] == 2 && mp.iso_valid[1] && mp.iso_gsi[1] == 1 && mp.iso_flags[9] == 0x0F && mp.irq_override_count == 2);
        CHECK(!mp.iso_valid[12]);
        ct[50] ^= 1; CHECK(mp_parse(base + 0x800, base + (1 << 16), &mp) == -4); ct[50] ^= 1;     /* испорченная таблица */
        fps[9] ^= 1; CHECK(mp_parse(base + 0x800, base + (1 << 16), &mp) == -2);
        CHECK(acpi_scan(base, base + 0x1000, "_MP_", 4, 0) == 0); fps[9] ^= 1;                 /* сумма не сходится - не находим */
        fps[11] = 6; fix(fps, 16, 10);                     /* стандартная конфигурация без таблицы */
        CHECK(mp_parse(base + 0x800, base + (1 << 16), &mp) == 0 && mp.cpu_count == 2 && mp.ioapic_count == 1);
        srand(3);
        for (int t = 0; t < 20000; t++) {                  /* мусор в таблице не выводит за её пределы */
            fps[11] = 0; fix(fps, 16, 10);
            for (int k = 44; k < 0x100; k++) ct[k] = (uint8_t)rand();
            uint16_t c2 = (uint16_t)(rand() % 300); memcpy(ct + 34, &c2, 2); fix(ct, tl, 7);
            mp_parse(base + 0x800, base + (1 << 16), &mp);
            CHECK(mp.ioapic_count <= 64 && mp.cpu_count <= 300);
        }
    }
    {   /* FADT: бит LEGACY_DEVICES (IAPC_BOOT_ARCH, смещение 109) - платформа сообщает, что устройства ISA есть */
        uint8_t *rs = m + 0x3000, *xs = m + 0x3100, *fa = m + 0x3200;
        for (int ver = 0; ver < 4; ver++) {
            static const uint8_t arch[4] = { 0x03, 0x00, 0x02, 0x01 };            /* 0 и 3: бит 0 стоит; 2: другие флаги; 1: ревизия FADT 1 */
            hdr(fa, "FACP", 136); fa[8] = (uint8_t)(ver == 1 ? 1 : 3); fa[109] = arch[ver]; fix(fa, 136, 9);   /* ver 1: ревизия 1 - поля нет */
            uint32_t xl = 36 + 8; hdr(xs, "XSDT", xl); uint64_t fp = base + 0x3200; memcpy(xs + 36, &fp, 8); fix(xs, xl, 9);
            memset(rs, 0, 36); memcpy(rs, "RSD PTR ", 8); rs[15] = 2; uint32_t rl = 36; memcpy(rs + 20, &rl, 4);
            uint64_t xa = base + 0x3100; memcpy(rs + 24, &xa, 8); fix(rs, 20, 8); fix(rs, 36, 32);
            acpi_info_t fi;
            CHECK(acpi_parse(base + 0x3000, base + (1 << 16), &fi) == 0 && fi.tables_seen == 1);
            CHECK(fi.legacy_isa == (ver == 0 || ver == 3));
        }
    }
    {   /* \_S5: как у QEMU (Name(\_S5, Package(4){5, 5, 0, 0}) через BytePrefix), Zero/One, длина пакета в 2 байта, ложные */
        uint8_t a = 9, b = 9;
        static const uint8_t q[] = { 0x10, 0x20, 0x08, '_', 'S', '5', '_', 0x12, 0x0A, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x00, 0x00 };
        CHECK(acpi_find_s5(q, sizeof q, &a, &b) == 0 && a == 5 && b == 5);
        static const uint8_t r[] = { 0x08, '\\', '_', 'S', '5', '_', 0x12, 0x46, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00 };
        CHECK(acpi_find_s5(r, sizeof r, &a, &b) == 0 && a == 0 && b == 1);
        static const uint8_t m[] = { 0x5B, 0x80, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x07, 0x00, 0x00, 0x00, 0x00 };   /* не NameOp */
        CHECK(acpi_find_s5(m, sizeof m, &a, &b) != 0);
        static const uint8_t t[] = { 0x08, '_', 'S', '5', '_', 0x12 };                         /* обрезано */
        CHECK(acpi_find_s5(t, sizeof t, &a, &b) != 0);
    }
    printf("acpi: all parser checks passed (incl. Intel MP tables, BIOS scan)\n");
    return 0;
}
