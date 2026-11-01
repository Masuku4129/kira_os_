#ifndef KIRA_PLATFORM_H
#define KIRA_PLATFORM_H
/* Сведения о платформе для общего кода (экран, настройки): у каждой архитектуры - своя реализация. */
unsigned    platform_cpus(void);        /* ядер, найденных на машине (ACPI / дерево устройств), - ещё до их запуска */
const char* platform_irq_name(void);    /* контроллер прерываний: "IOAPIC"/"PIC" (x86-64), "GICv3"/"GICv2" (ARM64) */
void        platform_status(void (*out)(const char*));   /* строка состояния: контроллер прерываний и таймер */
void        platform_hw(void (*out)(const char*));       /* описание процессора и машины (команда hw) */
int         platform_console_cmd(const char* cmd, const char* arg);   /* команды только этой архитектуры: 1 - выполнена */
void        platform_poweroff(void);   /* выключить машину (ACPI S5 / PSCI); возврат - не получилось */
void        platform_reboot(void);     /* перезагрузить (регистр сброса ACPI, 0xCF9, 8042 / PSCI); возврат - не получилось */
int         platform_fb_probe(void);   /* экран платформы без загрузочного и virtio-gpu (ARM: ramfb, simple-framebuffer); 0 - есть */
#endif
