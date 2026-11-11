#ifndef PIC_H
#define PIC_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

/* Динамические векторы для PCI INTx через IOAPIC. 0x50..0x5F зарезервированы MSI,
   0x60..0x7F — только для зарегистрированных обработчиков линий GSI. */
#define IRQ_VECTOR_BASE  0x60u
#define IRQ_VECTOR_COUNT 32u

void pic_init(void);
void pic_disable(void);
void pic_send_end_of_interrupt(unsigned char irq);
void pic_unmask_irq(unsigned char irq);
void pic_mask_irq(unsigned char irq);

// Регистрация обработчика аппаратного IRQ (0..15). Возвращает 0 или -1.
// EOI отправляет сам диспетчер - обработчик его НЕ шлёт.
int irq_register_handler(unsigned char irq, irq_handler_t handler);

// Подтверждение конца прерывания через Local APIC (режим IOAPIC) вместо PIC.
void irq_set_eoi_apic(unsigned irq, int on);
void irq_set_eoi_apic_all(void);
int  irq_has_handler(unsigned irq);
void irq_unregister_handler(unsigned irq);

/* Выдать/освободить вектор для IOAPIC GSI. Выдача допустима только после включения
   Local APIC. Перед освобождением вызывающий обязан замаскировать GSI и отключить
   источник; release дожидается уже вошедших обработчиков. */
int irq_vector_alloc(irq_handler_t handler, uint8_t *vector);
int irq_vector_is_allocated(uint8_t vector);
int irq_vector_release(uint8_t vector);

// Вызывается из ассемблерной заглушки с номером вектора (32..47)
void irq_dispatch(uint64_t vector, uint64_t cs, uint64_t rip, uint64_t rbp);   /* cs - сегмент прерванного кода (Ring 3: проверка завершения процесса) */

#endif
