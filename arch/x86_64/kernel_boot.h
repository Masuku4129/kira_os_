#ifndef KIRA_X86_KERNEL_BOOT_H
#define KIRA_X86_KERNEL_BOOT_H

/* x86-64-only boot composition entry; ARM64 has its own platform boot path. */
void kernel_boot_init(unsigned long magic, unsigned long addr);

#endif
