#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

void syscall_init(void);
void syscall_init_cpu(void);
void syscall_proc_gone(uint32_t pid);   /* вывести незаконченную строку завершившегося процесса */    /* MSR SYSCALL на текущем процессоре (каждый настраивает свои) */
uint64_t syscall_handler(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3);

#endif
