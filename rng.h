#ifndef KIRA_RNG_H
#define KIRA_RNG_H
#include <stdint.h>
#include <stddef.h>
/* Генератор случайных чисел ядра: пул SHA-256, в который подмешиваются аппаратные источники (RDRAND/RDSEED на
   x86-64, RNDR на ARM64, устройство virtio-rng) и времена прерываний; выход - ChaCha20 с заменой ключа после
   каждой выдачи (прошлые выдачи по нынешнему состоянию не восстановить). */
void rng_init(void);                          /* после PCI: аппаратные источники, первое засевание */
void rng_mix(const void* data, size_t n);     /* подмешать данные (не оценивая их случайность) */
void rng_event(void);                         /* событие (прерывание): подмешать счётчик тактов */
int  rng_get(void* out, size_t n);            /* 1 - засеяно аппаратным источником, 0 - только временем событий */
const char* rng_sources(void);                /* «RDRAND, virtio-rng» и т.п. - для status/hw */

/* Архитектура: счётчик тактов и аппаратное случайное слово (1 - получено). */
uint64_t arch_cycles(void);
int      arch_hw_random(uint64_t* v);
const char* arch_hw_random_name(void);        /* имя инструкции или 0 - процессор не умеет */

int virtio_rng_read(void* out, uint32_t n);   /* virtio-rng: байт получено, <0 - устройства нет */
#endif
