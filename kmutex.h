#ifndef KMUTEX_H
#define KMUTEX_H

#include <stdint.h>
#include "spinlock.h"

struct thread;

// Блокирующий мьютекс ядра. Ожидающий поток засыпает (ядро свободно для других потоков), владелец передаёт
// мьютекс первому в очереди (без «обгона»), так что очередь честная. Можно держать долго (операции с диском),
// можно спать под мьютексом. Нельзя брать из обработчиков прерываний и с выключенными прерываниями.
// Не рекурсивный: повторный захват тем же потоком - взаимная блокировка (kmutex_lock_check ловит её).
typedef struct kmutex {
    spinlock_t lk;                 // защищает поля ниже
    struct thread* owner;          // 0 - свободен
    struct thread* head;           // очередь ожидающих (по порядку прихода)
    struct thread* tail;
} kmutex_t;

#define KMUTEX_INIT { SPINLOCK_INIT, 0, 0, 0 }

void kmutex_lock(kmutex_t* m);
int  kmutex_trylock(kmutex_t* m);          // 1 - взят
void kmutex_unlock(kmutex_t* m);
int  kmutex_held(const kmutex_t* m);       // 1, если мьютекс держит вызывающий поток

#endif
