#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
int  uart_present(void);                // 1 - COM1 есть и прошёл проверку петлёй
void uart_putchar(char c);
int uart_getchar(void);                // -1, если во входном буфере пусто
void uart_print(const char* str);
void uart_print_serial(const char* str); // только в последовательный порт, не в журнал экрана
void uart_tick_flush(void);            // раз в тик: вывести застрявший хвост строки этого ядра
void uart_switch_flush(void);          // смена потока: вывести хвост строки прежнего потока этого ядра
void uart_flush_all(void);             // авария: вывести накопленное всеми ядрами
void uart_print_hex(uint64_t value);   // "0x..." без ведущих нулей
void uart_print_dec(uint64_t value);   // беззнаковое десятичное

uint64_t uart_tx_dropped(void);   // байт, не ушедших в порт: он не успевал (вывод при этом есть в журнале)
#endif
