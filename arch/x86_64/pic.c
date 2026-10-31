#include "pic.h"
#include "apic.h"
#include "uart.h"
#include "percpu.h"
#include "proc.h"
#include "sched.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    (PIC1_COMMAND + 1)
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    (PIC2_COMMAND + 1)
#define PIC_EOI      0x20
#define PIC_READ_ISR 0x0B

static irq_handler_t irq_handlers[16];

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void pic_init(void) {
    // Каскадирование и ремап векторов в безопасную зону (32-47)
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    // Все линии замаскированы; нужные включаются через pic_unmask_irq()
    // после регистрации обработчика.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_end_of_interrupt(unsigned char irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_unmask_irq(unsigned char irq) {
    if (irq >= 16) return;
    if (irq >= 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1u << 2)); // каскад IRQ2
        outb(PIC2_DATA, inb(PIC2_DATA) & (uint8_t)~(1u << (irq - 8)));
    } else {
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1u << irq));
    }
}

void pic_mask_irq(unsigned char irq) {
    if (irq >= 16) return;
    if (irq >= 8) {
        outb(PIC2_DATA, inb(PIC2_DATA) | (uint8_t)(1u << (irq - 8)));
    } else {
        outb(PIC1_DATA, inb(PIC1_DATA) | (uint8_t)(1u << irq));
    }
}

// Какие линии подтверждаются через Local APIC (а не через PIC). Заполняется при
// переходе на APIC-режим; до этого все линии - через PIC.
static volatile uint16_t eoi_apic_mask;

void irq_set_eoi_apic(unsigned irq, int on) {
    if (irq >= 16) return;
    if (on) eoi_apic_mask |= (uint16_t)(1u << irq); else eoi_apic_mask &= (uint16_t)~(1u << irq);
}
void irq_set_eoi_apic_all(void) { eoi_apic_mask = 0xFFFF; }
int irq_has_handler(unsigned irq) { return irq < 16 && irq_handlers[irq] != 0; }
void irq_unregister_handler(unsigned irq) { if (irq < 16) irq_handlers[irq] = 0; }

int irq_register_handler(unsigned char irq, irq_handler_t handler) {
    if (irq >= 16 || handler == 0 || irq_handlers[irq] != 0) {
        return -1;
    }
    irq_handlers[irq] = handler;
    return 0;
}

// Ложное (spurious) прерывание: линия 7 (или 15) пришла, а в ISR соответствующего
// контроллера бита нет. EOI для него слать нельзя (для IRQ15 - только мастеру).
static int is_spurious(unsigned char irq) {
    if (irq == 7) {
        outb(PIC1_COMMAND, PIC_READ_ISR);
        return (inb(PIC1_COMMAND) & 0x80) == 0;
    }
    if (irq == 15) {
        outb(PIC2_COMMAND, PIC_READ_ISR);
        if ((inb(PIC2_COMMAND) & 0x80) == 0) {
            outb(PIC1_COMMAND, PIC_EOI); // мастер видел каскадное IRQ2
            return 1;
        }
    }
    return 0;
}

int msi_dispatch(uint64_t vector);

void irq_dispatch(uint64_t vector, uint64_t cs, uint64_t rip, uint64_t rbp) {
    unsigned char irq = (unsigned char)(vector - 32);
    if (irq >= 16) {                                  /* сообщение устройства (MSI): только через Local APIC */
        percpu_t* me = this_cpu();
        me->irq_rip = rip; me->irq_rbp = (cs & 3) ? 0 : rbp; me->irq_depth++;
        msi_dispatch(vector);
        apic_send_eoi();
        this_cpu()->irq_depth--;
        if ((cs & 3) == 3) proc_return_check();
        sched_irq_exit();
        return;
    }

    int via_apic = (eoi_apic_mask >> irq) & 1;
    if (!via_apic && is_spurious(irq)) return;

    percpu_t* me = this_cpu();
    me->irq_rip = rip;                        // где стояло ядро: по этому дамп watchdog показывает место зависания
    me->irq_rbp = (cs & 3) ? 0 : rbp;
    me->irq_depth++;
    if (irq_handlers[irq]) {
        irq_handlers[irq]();
    } else {
        // Прерывание без обработчика: гасим линию, чтобы не зациклиться.
        pic_mask_irq(irq);
    }
    if (via_apic) apic_send_eoi(); else pic_send_end_of_interrupt(irq);
    this_cpu()->irq_depth--;
    if ((cs & 3) == 3) proc_return_check();   // прервана программа, процесс которой завершается: поток уходит

    // Вытеснение только ПОСЛЕ EOI: иначе линия IRQ остаётся "в обслуживании"
    // и контроллер не доставит следующее прерывание, пока поток не вернётся.
    sched_irq_exit();
}
