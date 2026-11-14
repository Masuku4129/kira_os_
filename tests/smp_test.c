#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "smp.h"
#include "tlb_shootdown.h"
#include "percpu.h"
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c); fails++;} }while(0)
static int fails;
int main(void) {
    uint32_t out[8];
    uint32_t ids[] = { 0, 1, 2, 3 };
    CHECK(smp_plan(ids, 4, 0, out, 8) == 3 && out[0] == 1 && out[1] == 2 && out[2] == 3);
    CHECK(smp_plan(ids, 4, 2, out, 8) == 3 && out[0] == 0 && out[1] == 1 && out[2] == 3);   /* загрузочный не в начале списка */
    uint32_t dup[] = { 4, 4, 6, 4, 6, 8 };
    CHECK(smp_plan(dup, 6, 4, out, 8) == 2 && out[0] == 6 && out[1] == 8);                   /* повторы и сам загрузочный */
    CHECK(smp_plan(ids, 4, 0, out, 2) == 2);                                                 /* предел */
    CHECK(smp_plan(ids, 1, 0, out, 8) == 0);                                                 /* один процессор */
    CHECK(smp_plan(ids, 0, 0, out, 8) == 0);
    uint32_t big[] = { 0, 300, 70000 };
    CHECK(smp_plan(big, 3, 0, out, 8) == 2 && out[0] == 300 && out[1] == 70000);           /* 32-битные ID x2APIC */

    /* команды ICR: режим доставки в битах 8-10 */
    CHECK(((ICR_INIT_ASSERT >> 8) & 7) == 5 && (ICR_INIT_ASSERT & 0x4000) && (ICR_INIT_ASSERT & 0x8000));
    CHECK(((ICR_INIT_DEASSERT >> 8) & 7) == 5 && !(ICR_INIT_DEASSERT & 0x4000) && (ICR_INIT_DEASSERT & 0x8000));
    CHECK(((ICR_SIPI(SMP_TRAMP_BASE >> 12) >> 8) & 7) == 6 && (ICR_SIPI(SMP_TRAMP_BASE >> 12) & 0xFF) == 0x08);
    CHECK(((ICR_FIXED(0xF0) >> 8) & 7) == 0 && (ICR_FIXED(0xF0) & 0xFF) == 0xF0 && (ICR_FIXED(0xF0) & 0x4000));

    /* TLB acknowledgements belong to one generation: a stale/late ack cannot complete a newer request. */
    cpumask_t targets = cm_none();
    cm_set(&targets, 1); cm_set(&targets, 65); cm_set(&targets, 255);
    volatile uint64_t ack[CPU_MAX] = {0};
    CHECK(!tlb_shootdown_all_acked(&targets, ack, 7));
    ack[1] = 7; ack[65] = 7; ack[255] = 6;
    CHECK(!tlb_shootdown_all_acked(&targets, ack, 7)); /* один CPU всё ещё не подтвердил */
    ack[255] = 7;
    CHECK(tlb_shootdown_all_acked(&targets, ack, 7));
    ack[1] = 8;
    CHECK(!tlb_shootdown_all_acked(&targets, ack, 8)); /* старые ack других CPU не принимаются */
    ack[65] = 8; ack[255] = 8;
    CHECK(tlb_shootdown_all_acked(&targets, ack, 8));
    CHECK(!tlb_shootdown_all_acked(&targets, ack, 0)); /* нулевое поколение недопустимо */
    cpumask_t no_targets = cm_none();
    CHECK(tlb_shootdown_all_acked(&no_targets, ack, 9));
    CHECK((SMP_TRAMP_BASE & 0xFFF) == 0 && SMP_TRAMP_BASE < 0x100000 && (SMP_TRAMP_BASE >> 12) <= 0xFF);   /* вектор SIPI - номер страницы ниже 1 МБ */

    /* таблица percpu */
    percpu_init_bsp(7);
    CHECK(percpu_count() == 1 && percpu_table[0].apic_id == 7 && percpu_table[0].cpu_id == 0 && percpu_table[0].self == &percpu_table[0]);
    percpu_t* a = percpu_alloc(9);
    CHECK(a && a->cpu_id == 1 && percpu_count() == 2 && a->self == a && this_cpu() == &percpu_table[0]);
    for (unsigned i = 2; i < PERCPU_MAX_CPUS; i++) CHECK(percpu_alloc(100 + i) != 0);
    CHECK(percpu_alloc(1) == 0);                                                              /* таблица полна */
    CHECK(smp_work_checksum(SMP_WORK_ITERS) == SMP_WORK_EXPECTED);                          /* константа проверки на ядрах верна */
    CHECK(smp_work_checksum(1) != smp_work_checksum(2));
    printf("%s\n", fails ? "FAILED" : "smp: AP selection (BSP excluded, duplicates, limit), ICR commands, SIPI vector, percpu table, work checksum passed");
    return fails != 0;
}
