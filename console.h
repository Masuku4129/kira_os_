#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_poll(void);               // обработать накопившийся ввод с UART (COM1)
// Команда от оболочки (Проводник и т.п.), из любого потока: выполнится в основном потоке ядра при следующем
// console_poll так, будто её набрали в терминале (с эхом после приглашения). 0 - принята, -1 - очередь полна.
int  console_request(const char* line);

#endif
