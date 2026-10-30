#ifndef IOAPIC_H
#define IOAPIC_H
#include <stdint.h>
#include "acpi.h"

/* Описание линии: полярность/триггер из переопределений ACPI (MADT type 2). */
typedef struct { uint32_t gsi; int active_low; int level; } irq_line_t;

/* Чистая логика (проверяется на хосте): ISA IRQ -> GSI + полярность + триггер. */
irq_line_t ioapic_resolve(const acpi_info_t* a, unsigned isa_irq);
/* Кодирование записи таблицы перенаправления IOAPIC. */
void ioapic_encode(uint8_t vector, int active_low, int level, int masked, uint32_t dest_apic_id,
                   uint32_t* lo, uint32_t* hi);

/* Отображает IOAPIC из MADT и проверяет регистры. 0 - успех. */
int ioapic_init(const acpi_info_t* a);
/* Направляет ISA IRQ на вектор 32+irq (размаскированным) на процессор dest. 0 - успех. */
int ioapic_route_isa(const acpi_info_t* a, unsigned isa_irq, uint32_t dest_apic_id);
void ioapic_mask_isa(const acpi_info_t* a, unsigned isa_irq, int masked);
unsigned ioapic_entry_count(void);

#endif
