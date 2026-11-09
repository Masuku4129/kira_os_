#ifndef IRQMODE_H
#define IRQMODE_H
#include <stdint.h>

/* Переход с PIC+PIT на современную схему: IOAPIC для устройств и таймер LAPIC (100 Гц)
   как источник тиков. Каждый шаг проверяется на живом железе; при любом сбое система остаётся
   на PIC+PIT и работает как раньше. Возвращает 0 - APIC-режим включён, <0 - остались на PIC. */
int irqmode_enable_apic(uint64_t tsc_khz, uint64_t lapic_counts_per_s, int have_lapic, uint32_t tick_hz);
int irqmode_is_apic(void);
/* APIC ID ядра, на которое сейчас направлены прерывания устройств. */
uint32_t irqmode_target(void);
/* Ядро с таким APIC ID отказало: все линии устройств переносятся на new_apic_id. Возвращает число перенесённых
   линий, <0 - перенос невозможен (нет APIC-режима). Если отказавшее ядро не было целью, ничего не делает. */
int irqmode_core_failed(uint32_t failed_apic_id, uint32_t new_apic_id);

/* Линия PCI INTx для драйвера Ring 3: on=1 - направить (уровень, активный низкий) на текущее ядро и подтверждать через LAPIC; on=0 - замаскировать.
   0 - готово, <0 - нет APIC-режима или линии. */
int irqmode_pci_line(unsigned irq, int on);

#endif
