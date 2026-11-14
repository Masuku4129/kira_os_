#ifndef KSTACK_H
#define KSTACK_H
#include <stdint.h>
/* Стеки потоков ядра с пустотой ниже (переполнение - ошибка доступа, а не порча чужой памяти). Реализует vmm своей
   архитектуры. 0 - guarded stack window exhausted or allocation failed; thread creation must fail closed. */
void* vmm_kstack_alloc(uint32_t bytes);            // нижний адрес стека или 0
void  vmm_kstack_free(void* base);
int   vmm_kstack_guard_hit(uint64_t addr);         // 1 - адрес в пустоте под стеком
int   vmm_is_kstack(uint64_t addr);
#endif
