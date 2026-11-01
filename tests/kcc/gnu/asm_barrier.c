void f(void) { __asm__ volatile("" ::: "memory"); __asm__ __volatile__("mfence" ::: "memory"); asm volatile("pause"); }
