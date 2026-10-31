#ifndef KIRA_PLATFORM_H
#define KIRA_PLATFORM_H
/* Сведения о платформе для общего кода (экран, настройки): у каждой архитектуры - своя реализация. */
const char* platform_irq_name(void);    /* контроллер прерываний: "IOAPIC"/"PIC" (x86-64), "GICv3"/"GICv2" (ARM64) */
#endif
