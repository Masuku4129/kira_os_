#ifndef KSTACK_H
#define KSTACK_H
#include <stdint.h>
/* Стеки потоков ядра с пустотой ниже (переполнение - ошибка доступа, а не порча чужой памяти). Реализует vmm своей
   архитектуры. 0 - окно исчерпано: планировщик берёт стек из кучи с меткой на дне. */
void* vmm_kstack_alloc(uint32_t bytes);            // нижний адрес стека или 0
void  vmm_kstack_free(void* base);
int   vmm_kstack_guard_hit(uint64_t addr);         // 1 - адрес в пустоте под стеком
int   vmm_is_kstack(uint64_t addr);
uint32_t vmm_kstack_bad_frees(void);               // сколько освобождений отклонено (двойное / чужой адрес)
#endif
