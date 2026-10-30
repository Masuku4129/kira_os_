#ifndef HWPROFILE_H
#define HWPROFILE_H

#include <stdint.h>

/* Профиль железа: что ядро узнало о процессоре и памяти в начале загрузки. От него зависят пределы
   (политика в uheap.c и далее): одна и та же сборка должна работать на 128 МБ и на 64 ГБ.
   Разбор CPUID отделён от выполнения команды CPUID, чтобы проверять его на синтетических значениях
   разных процессоров без самого железа. */

enum { HW_L0, HW_L1, HW_L7, HW_LD, HW_L15, HW_E0, HW_E1, HW_E7, HW_E8, HW_LEAVES };

typedef struct {
    uint32_t r[HW_LEAVES][4];       /* eax, ebx, ecx, edx каждого листа; недоступный лист - нули */
} hw_cpuid_t;

typedef struct {
    char     vendor[13];
    uint32_t family, model, stepping;
    uint32_t cpus;                  /* включённые процессоры по ACPI MADT, не меньше 1 */
    uint64_t ram_kb;                /* вся ОЗУ, которую видит менеджер страниц */
    uint32_t xsave_max;             /* байт под все компоненты XSAVE, которые умеет процессор (0 - XSAVE нет) */
    uint32_t tsc_khz;               /* измерено по PIT; 0 - ещё не измерено */
    uint32_t tsc_khz_leaf;          /* заявлено процессором (CPUID 0x15); 0 - не сообщает */
    uint8_t  phys_bits, virt_bits;
    unsigned has_sse2 : 1, has_nx : 1, has_page1g : 1, has_xsave : 1, has_avx : 1, has_avx2 : 1, has_avx512f : 1,
             has_pcid : 1, has_invpcid : 1, has_smep : 1, has_smap : 1, has_fsgsbase : 1, has_rdrand : 1,
             has_rdseed : 1, has_inv_tsc : 1, has_tsc_deadline : 1, has_x2apic : 1, in_vm : 1;
} hw_profile_t;

/* Чистая функция: из сырых листов CPUID заполняет поля процессора (cpus, ram_kb, tsc_khz не трогает). */
void hw_decode(const hw_cpuid_t* raw, hw_profile_t* out);
/* Выполняет CPUID на этом процессоре. */
void hw_collect(hw_cpuid_t* raw);

void hw_init(uint32_t cpus, uint64_t ram_kb);        /* collect + decode + занести ядра и память */
void hw_set_tsc_khz(uint32_t khz);                   /* после калибровки по таймеру */
const hw_profile_t* hw_get(void);

/* Человекочитаемое описание построчно через out (в ядре - uart_print). */
void hw_describe(const hw_profile_t* p, void (*out)(const char*));

#endif
