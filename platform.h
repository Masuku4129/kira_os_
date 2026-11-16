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
/* Батарея для верхней панели. 0 - батарея есть, поля заполнены; -1 - батареи нет или данных нет (тогда индикатор не показывается, ничего не выдумывается).
   percent 0..100 или -1 (заряд неизвестен); charging: 1 заряжается; ac: 1 сеть подключена, 0 нет, -1 неизвестно; version растёт при каждом изменении. */
typedef struct { int percent, charging, ac, minutes_left; unsigned version; } platform_battery_t;
int         platform_battery(platform_battery_t* b);
void        platform_acpi_cmd(const char* args);   /* команда acpi: x86-64 - интерпретатор AML и таблицы; ARM64 - сообщение, что AML там не используется */
int         platform_fb_probe(void);   /* экран платформы без загрузочного и virtio-gpu (ARM: ramfb, simple-framebuffer); 0 - есть */
#endif
