#ifndef KIRA_ACPIRT_H
#define KIRA_ACPIRT_H
/* x86-64: запуск ACPI-интерпретатора на реальной машине. Уровень 2 (arch): порты, память, PCI, отдельный стек, SCI.
   Логика событий, батареи и _PRT - в acpidev.c (уровень 1). */
#include <stdint.h>

int  acpirt_init(void);                  /* 0 - работает; иначе таблицы не приняты (выключение через \_S5-разбор ядра остаётся) */
int  acpirt_up(void);
void acpirt_poll(void);                  /* из основного цикла ядра: события, питание */
int  acpirt_take_poweroff(void);         /* 1 - нажата кнопка питания (запрос снимается: повтор только при новом нажатии) */
int  acpirt_s5(uint8_t* a, uint8_t* b);  /* \_S5 от интерпретатора, 0 - получено */
void acpirt_prepare_sleep(int state);    /* \_PTS/\_GTS перед выключением */
void acpirt_cmd(const char* args);       /* команда консоли acpi (arch/x86_64/acpirt_cmd.c) */
void acpirt_dump(void);                  /* печать состояния в консоль/UART */
#endif
