#ifndef ACPI_H
#define ACPI_H
#include <stdint.h>

#define ACPI_MAX_IOAPIC 64
#define ACPI_MAX_CPU    256

typedef struct {
    int      valid;            /* RSDP и корневая таблица прошли проверку контрольных сумм */
    uint8_t  revision;         /* ревизия ACPI (0 = 1.0, >=2 = XSDT) */
    uint32_t cpu_count;        /* включённые процессоры из MADT */
    uint32_t cpu_apic_id[ACPI_MAX_CPU];   /* MADT type 0 (xAPIC, 8 бит) и type 9 (x2APIC, 32 бита), без повторов */
    uint32_t max_apic_id;
    uint32_t lapic_addr;
    uint32_t ioapic_count;
    struct { uint8_t id; uint32_t addr; uint32_t gsi_base; } ioapic[ACPI_MAX_IOAPIC];
    uint32_t irq_override_count;   /* MADT type 2: ISA IRQ -> GSI */
    uint8_t  iso_valid[16];        /* для ISA IRQ 0..15: есть ли переопределение */
    uint32_t iso_gsi[16];
    uint16_t iso_flags[16];        /* бит0-1 полярность (3 = активный низкий), бит2-3 триггер (3 = по уровню) */
    int      has_hpet;
    uint64_t mcfg_base;            /* база PCIe ECAM (0 = нет) */
    uint8_t  mcfg_bus_start, mcfg_bus_end;
    uint32_t tables_seen, tables_bad, tables_skipped_high;
    uint8_t  source;               /* 0 - ACPI (MADT), 1 - таблицы Intel MP (ACPI нет или в нём нет процессоров) */
} acpi_info_t;

/* Разбор по физическому адресу RSDP. Таблицы выше max_phys пропускаются (ещё не отображены). */
int acpi_parse(uint64_t rsdp_phys, uint64_t max_phys, acpi_info_t* out);
/* Таблицы Intel MultiProcessor (старые ПК, эмуляторы с выключенным ACPI - например Limbo): по физическому адресу
   плавающего указателя "_MP_". Заполняет процессоры, IOAPIC и назначения ISA IRQ так же, как MADT. 0 - успех. */
int mp_parse(uint64_t fps_phys, uint64_t max_phys, acpi_info_t* out);
/* Поиск сигнатуры (по 16 байт) с проверкой контрольной суммы sum_len байт (0 - длина из FPS: байт 8 x 16). */
uint64_t acpi_scan(uint64_t from, uint64_t to, const char* sig, unsigned sig_len, unsigned sum_len);
/* Найти RSDP в теге Multiboot2 (тип 14/15) или в памяти BIOS и разобрать; без ACPI-процессоров - таблицы MP. */
int acpi_init(uint64_t mbi_phys);

// Ядро ставит хук, который делает таблицу выше max_phys читаемой (0 - не удалось); без хука такие таблицы пропускаются.
void acpi_set_map_hook(int (*hook)(uint64_t phys, uint64_t len));
const acpi_info_t* acpi_get(void);

#endif
