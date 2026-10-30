#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_poll(void);               // обработать накопившийся ввод с UART (COM1)
void console_note_event(void);         // зарегистрировать внешнее событие (клавиатура и т.п.)
uint32_t console_take_events(void);
// Команда от оболочки (Проводник и т.п.), из любого потока: выполнится в основном потоке ядра при следующем
// console_poll так, будто её набрали в терминале (с эхом после приглашения). 0 - принята, -1 - очередь полна.
int  console_request(const char* line);
void console_set_apic_rate(uint64_t rate); // результат калибровки LAPIC для команды status    // события с прошлого вызова (и сброс счётчика)

#endif
