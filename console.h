#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
/* Связь с оболочкой рабочего стола (уровень 8): консоль (уровень 6) её не знает, оболочка подключает себя сама (wire.c). Нет оболочки - нет и действий. */
typedef struct {
    void (*print_info)(void);                                  /* команда fb */
    void (*request_files)(const char* left, const char* right); /* команда files */
    int  (*kbd_owner)(void);                                   /* 1 - клавиатуру читает Проводник */
} console_shell_t;
void console_set_shell(const console_shell_t* sh);
void console_poll(void);               // обработать накопившийся ввод с UART (COM1)
// Команда от оболочки (Проводник и т.п.), из любого потока: выполнится в основном потоке ядра при следующем
// console_poll так, будто её набрали в терминале (с эхом после приглашения). 0 - принята, -1 - очередь полна.
int  console_request(const char* line);

#endif
