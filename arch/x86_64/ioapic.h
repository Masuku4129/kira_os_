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
/* Линия PCI INTx на ISA-номер irq: уровень, активный низкий (если MADT не говорит иное), вектор 32+irq, размаскирована. 0 - успех. */
int ioapic_route_pci(const acpi_info_t* a, unsigned isa_irq, uint32_t dest_apic_id);
int ioapic_route_gsi(const acpi_info_t* a, uint32_t gsi, uint8_t vector, uint32_t dest);   /* линия PCI по номеру GSI на заданный вектор */
void ioapic_mask_gsi(const acpi_info_t* a, uint32_t gsi, int masked);
void ioapic_mask_isa(const acpi_info_t* a, unsigned isa_irq, int masked);
unsigned ioapic_entry_count(void);
/* Сон S3: снимок и возврат всех записей перенаправления и номеров IOAPIC. save: 0 - готово; restore: 0 - все записи легли как были. */
int ioapic_save(void);
int ioapic_restore(void);

#endif
