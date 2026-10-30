#ifndef COREFAULT_H
#define COREFAULT_H

#include <stdint.h>

// Решение о судьбе сбоя в режиме ядра (чистая функция, проверяется на хосте).
//   KILL_THREAD - погибает только виновный поток, ядро продолжает работать;
//   FAIL_CORE   - отказ самого ядра (машинная проверка): оно выходит из планирования, остальные работают;
//   PANIC       - сбой нельзя изолировать: аварийная остановка всей системы.
// Изолировать можно, только если после сбоя общие данные ядра заведомо целы: ядро не держит спин-замков и
// мьютексов, не в обработчике прерывания, сбой первый, виновный - не служебный поток ядра.
typedef enum { COREFAULT_PANIC = 0, COREFAULT_KILL_THREAD = 1, COREFAULT_FAIL_CORE = 2 } corefault_action_t;

typedef struct {
    unsigned vector;
    unsigned locks_held;      // спин-замков у ядра
    unsigned irq_depth;       // >0: сбой в обработчике прерывания
    unsigned in_fault;        // ядро уже разбирало сбой
    unsigned mutexes_held;    // мьютексов ядра у виновного потока
    int have_thread;          // планировщик на этом ядре работает и есть текущий поток
    int service;              // виновный - служебный поток ядра
    unsigned other_cores;     // сколько других ядер продолжают планирование
    int irq_move_ok;          // прерывания устройств можно переселить с этого ядра (или они на нём не сидят)
} corefault_input_t;

corefault_action_t corefault_decide(const corefault_input_t* in);
const char* corefault_why_panic(const corefault_input_t* in);   // причина отказа в изоляции (для журнала)

#endif
