#ifndef APIC_H
#define APIC_H

#include <stdint.h>

// Определяет и включает Local APIC. xAPIC: регистры в окне MMIO ядра. x2APIC (регистры - MSR, 32-битные ID)
// выбирается, если его уже включила прошивка, если есть APIC ID больше 254 (max_apic_id из MADT) или если задан
// want_x2 (параметр загрузки x2apic) и процессор его умеет. 0 - успех, -1 - APIC недоступен (система идёт на PIT).
int apic_init(int want_x2, uint32_t max_apic_id);
int apic_is_x2(void);

// Частота счётчика таймера LAPIC (делитель 16) по откалиброванному TSC, счётчиков в секунду; 0 - ошибка.
uint64_t apic_calibrate_timer_tsc(uint64_t tsc_khz);

uint32_t apic_get_id(void);
uint32_t apic_get_version(void);
int apic_is_enabled(void);
void apic_send_eoi(void);

// Межъядерные прерывания (ICR; в x2APIC - одна запись MSR). dest - APIC ID получателя; icr_low - команда (см. smp.h).
// 0 - доставлено в приёмник (бит занятости сброшен), -1 - не дождались.
int  apic_send_ipi(uint32_t dest_apic_id, uint32_t icr_low);
// Включает Local APIC на процессоре, который это вызывает (регистры уже отображены загрузочным процессором).
int  apic_enable_this_cpu(void);

// Периодический таймер LAPIC: прерывание с заданным вектором каждые initial_count
// счётчиков (делитель 16, как при калибровке). Останов гасит и маскирует.
void apic_timer_start_periodic(uint8_t vector, uint32_t initial_count);
void apic_timer_stop(void);
void apic_timer_mask_local(void);   /* только этого ядра, общий счёт не трогает */
uint32_t apic_timer_initial_count(void);   /* 0, если таймер LAPIC не работает (тики идут от PIT) */

#endif
