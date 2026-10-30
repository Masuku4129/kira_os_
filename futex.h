#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

// Futex: ожидание на 32-битном слове памяти программы. Ключ - физический адрес слова, поэтому одно и то же слово
// в разделяемой памяти разных процессов - один ключ. Проверка значения и постановка в очередь неделимы
// относительно futex_wake (замок корзины), пробуждение не теряется.
// wait: 0 - разбужен, KE_AGAIN - значение уже другое, KE_TIMEOUT, KE_INTR (stop() != 0). ticks: ~0 - без срока.
/* word - адрес слова, load - как его прочитать (для программ - безопасное чтение памяти процесса; -1 - адрес пропал). */
int      futex_wait(uint64_t key, uint64_t word, int (*load)(uint64_t word, uint32_t* v), uint32_t expected, uint64_t ticks, int (*stop)(void));
unsigned futex_wake(uint64_t key, unsigned count);       // сколько разбужено

#endif
