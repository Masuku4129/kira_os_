#ifndef ARCH_VMM_H
#define ARCH_VMM_H
/* x86-64: программы живут в нижней половине адресов (PML4[1..255]), сдвига нет. */
#define VMM_USER_SHIFT 0ULL
#define USER_VA(x) ((uint64_t)(x) + VMM_USER_SHIFT)   /* адрес раскладки программы для этой архитектуры */
#endif
