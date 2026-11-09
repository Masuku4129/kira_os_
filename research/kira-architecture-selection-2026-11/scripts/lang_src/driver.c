/* общая рамка: вызывает bench_main() (в нагрузках main переименован -Dmain=bench_main), печатает полный результат */
#include <stdio.h>
extern int bench_main(void);
int main(void) { printf("%d\n", bench_main()); return 0; }
