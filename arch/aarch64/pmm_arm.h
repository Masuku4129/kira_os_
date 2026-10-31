#ifndef PMM_ARM_H
#define PMM_ARM_H
#include <stdint.h>
typedef struct { uint64_t base, size; } pmm_arm_region_t;
uint64_t pmm_arm_map_bytes(uint64_t lo, uint64_t hi);       /* размер битовой карты для адресов [lo, hi) */
/* Участки ОЗУ, общий охват [lo, hi), память под карту, занятое при старте [res_lo, res_hi). */
void pmm_arm_init(const pmm_arm_region_t* r, unsigned n, uint64_t lo, uint64_t hi, void* map,
                  uint64_t res_lo, uint64_t res_hi);
void pmm_arm_reserve(uint64_t lo, uint64_t hi);              /* ещё один занятый участок */
#endif
