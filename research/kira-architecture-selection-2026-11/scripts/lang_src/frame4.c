/* общая C-рамка для Rust/C++/asm-вариантов 4 нагрузок: выбирает функцию по -DFN=имя; собирается clang -O2 (одна и та же для всех языков). */
#include <stdio.h>
extern int FN(void);
int main(void) { printf("%d\n", FN()); return 0; }
