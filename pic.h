#ifndef PIC_H
#define PIC_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

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

// Вызывается из ассемблерной заглушки с номером вектора (32..47)
void irq_dispatch(uint64_t vector, uint64_t cs);   /* cs - сегмент прерванного кода (Ring 3: проверка завершения процесса) */

#endif
