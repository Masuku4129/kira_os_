#include "ioapic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int main(void){
    acpi_info_t a; memset(&a,0,sizeof a);
    irq_line_t l = ioapic_resolve(&a, 1);
    CHECK(l.gsi==1 && !l.active_low && !l.level);              /* ISA по умолчанию */
    a.iso_valid[0]=1; a.iso_gsi[0]=2; a.iso_flags[0]=0;         /* типичный IRQ0 -> GSI2 */
    l = ioapic_resolve(&a, 0); CHECK(l.gsi==2 && !l.active_low && !l.level);
    a.iso_valid[9]=1; a.iso_gsi[9]=9; a.iso_flags[9]=0x0F;      /* SCI: активный низкий, по уровню */
    l = ioapic_resolve(&a, 9); CHECK(l.gsi==9 && l.active_low && l.level);
    a.iso_valid[5]=1; a.iso_gsi[5]=5; a.iso_flags[5]=0x05;      /* явно: высокий, фронт */
    l = ioapic_resolve(&a, 5); CHECK(!l.active_low && !l.level);
    uint32_t lo, hi;
    ioapic_encode(0x21, 0, 0, 0, 3, &lo, &hi);
    CHECK(lo == 0x21 && hi == 0x03000000);
    ioapic_encode(0x2C, 1, 1, 1, 0, &lo, &hi);
    CHECK(lo == (0x2C | (1u<<13) | (1u<<15) | (1u<<16)) && hi == 0);
    printf("ioapic: ISO resolution and redirection-entry encoding passed\n");
    return 0;
}
