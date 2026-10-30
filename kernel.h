#ifndef KERNEL_H
#define KERNEL_H

void kernel_main(unsigned long magic, unsigned long addr);
void kernel_panic(const char* reason) __attribute__((noreturn));

#endif
