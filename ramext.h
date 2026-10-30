#ifndef RAMEXT_H
#define RAMEXT_H

#include <stdint.h>

// Отображение ОЗУ выше 1 ГБ в identity-карту ядра (PML4[0]), чтобы PMM мог её выдавать.
// Чистая часть: кладёт [start, end) в PDPT страницами по 2 МБ (границы сужаются внутрь до 2 МБ),
// RW + NX. Страницы под PD берёт у alloc (возвращает физический = виртуальный адрес, 0 - нет памяти).
// Возвращает верхнюю границу реально отображённого, *mapped_start - нижнюю; равны, если ничего не отображено.
// Занятая запись PD (там уже что-то лежит) останавливает отображение: чужое не затираем.
uint64_t ramext_map(uint64_t* pdpt, uint64_t start, uint64_t end,
                    uint64_t (*alloc)(void), uint64_t* mapped_start);

// Ядро: отображает все диапазоны, которые pmm_high_ranges считает ОЗУ, и включает их в выдачу PMM.
// Возвращает число добавленных МБ (в лог идут и потери на краях).
uint64_t ramext_init(void);

// Делает [phys, phys+len) читаемым (только чтение, NX), если он лежит целиком в области ACPI reclaimable/NVS из карты памяти.
// Для таблиц ACPI выше 1 ГБ. Возвращает 1 - можно читать. Чистая часть: ramext_map_ro.
int ramext_map_read(uint64_t phys, uint64_t len);
int ramext_map_ro(uint64_t* pdpt, uint64_t start, uint64_t end, uint64_t (*alloc)(void));

#endif
