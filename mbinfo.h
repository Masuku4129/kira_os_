#ifndef MBINFO_H
#define MBINFO_H

#include <stdint.h>

#define MBINFO_MAX 65536

// Загрузчик может положить Multiboot2-структуру выше 1 ГБ (типично при UEFI), а ядро
// отображает 1:1 только первый ГБ. Если структура вне карты, она отображается через
// bootmap и копируется в статический буфер ядра; возвращается адрес копии.
// В остальных случаях (нормальная структура, неверный magic, повреждённый размер)
// возвращается addr без изменений - дальше их проверяют и отвергают PMM и остальные модули.
unsigned long mbinfo_relocate(unsigned long magic, unsigned long addr);

// Ищет тег заданного типа в (уже доступной) структуре. NULL - нет или структура повреждена.
const void* mbinfo_find_tag(unsigned long mbi, uint32_t type);
// Есть ли слово в командной строке загрузки (тег 1), например "x2apic". 0 - нет или строки нет.
int mbinfo_cmdline_has(unsigned long mbi, const char* word);

#endif
