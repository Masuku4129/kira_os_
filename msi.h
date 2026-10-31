#ifndef KIRA_MSI_H
#define KIRA_MSI_H
#include <stdint.h>
#include "pci.h"
#include "spinlock.h"

/* Прерывания устройств сообщениями (MSI / MSI-X): устройство само пишет «сообщение» по адресу контроллера
   прерываний, линии и таблицы маршрутов ACPI не нужны. Архитектура выдаёт вектор и пару адрес/данные. */
typedef void (*msi_fn)(void* ctx);
int  msi_alloc(msi_fn fn, void* ctx, uint64_t* addr, uint32_t* data);   /* 0 - вектор выдан; -1 - нет (опрос) */
int  pci_msix_setup(const pci_dev_t* d, unsigned entry, uint64_t addr, uint32_t data);   /* 0 - MSI-X включён */
int  pci_msi_setup(const pci_dev_t* d, uint64_t addr, uint32_t data);                    /* 0 - MSI включён */

/* Ожидание события устройства сном: обработчик прерывания зовёт irqwait_fire, поток ждёт в irqwait_wait.
   Метка (irqwait_mark) берётся ДО запуска операции: прерывание, пришедшее раньше сна, не теряется. */
typedef struct irqwait { spinlock_t lk; void* waiter; volatile uint32_t fired; uint64_t wakeups; } irqwait_t;
#define IRQWAIT_INIT { SPINLOCK_INIT, 0, 0, 0 }
static inline uint32_t irqwait_mark(irqwait_t* w) { return __atomic_load_n(&w->fired, __ATOMIC_ACQUIRE); }
void irqwait_fire(irqwait_t* w);
void irqwait_stats(uint64_t* fired, uint64_t* woken);   /* всего прерываний устройств и пробуждений ими */
void irqwait_wait(irqwait_t* w, uint32_t mark, uint64_t ticks);           /* до события после mark или ticks тиков */
#endif
