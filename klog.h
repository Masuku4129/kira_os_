#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

// Кольцевой журнал всего, что ядро выводит в UART. Нужен, чтобы экран мог показать
// сообщения, напечатанные до его инициализации, и чтобы вывод из прерываний
// не рисовался сразу (рисует поток дисплея).
#define KLOG_SIZE 32768

void klog_putc(char c);
uint64_t klog_total(void);                                  // всего записано байт
// Читает с позиции *cursor до max байт. Если нужные данные уже затёрты, курсор
// сдвигается к самым старым сохранившимся (возвращает флаг через *lost).
unsigned klog_read(uint64_t* cursor, char* out, unsigned max, unsigned* lost);

#endif
