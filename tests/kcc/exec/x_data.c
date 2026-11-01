#include "ktest.h"
struct P { int x; char tag; long y; const char *name; };
struct P tab[] = { {1, 'a', 100, "one"}, {2, 'b', -200, "two"}, {3, 'c', 300000000000L, "three"} };
static int counter = 5;
int zero_bss[100];
static int *ptr_to_counter = &counter;
int *ptr_into = &zero_bss[7];
int (*fp)(int) = 0;
char buf[16] = "xyz";
const int lookup[8] = { 1, 1, 2, 3, 5, 8, 13, 21 };
int next(int v) { return v + counter++; }
int twice(int v) { return v * 2; }
int main(void) {
    for (int i = 0; i < 3; i++) { ps(tab[i].name); ps(" "); pd(tab[i].x); ps(" "); pd(tab[i].tag); ps(" "); pn(tab[i].y); }
    *ptr_to_counter += 10; pn(counter); pn(next(1)); pn(next(1)); pn(counter);
    *ptr_into = 42; pn(zero_bss[7]); pn(zero_bss[8]);
    fp = twice; pn(fp(21));
    buf[3] = '!'; buf[4] = 0; ps(buf); ps("\n"); pn(buf[10]);
    long sum = 0; for (int i = 0; i < 8; i++) sum += lookup[i]; pn(sum);
    static int local_static[4] = {9, 8, 7, 6}; local_static[2]++; pn(local_static[0] * 1000 + local_static[2]);
    return (int)(sizeof(tab) + sizeof(zero_bss));
}
