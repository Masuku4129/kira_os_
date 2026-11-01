#include "ktest.h"
static int sw(int x) {
    switch (x) {
    case 0: return 100;
    case 1: case 2: x += 10;
    case 3: x += 20; break;
    case 100: return -1;
    case -7: x = 77; break;
    default: x = -x;
    }
    return x;
}
int main(void) {
    for (int i = -8; i < 6; i++) { pd(sw(i)); ps(" "); }
    pd(sw(100)); ps(" "); pn(sw(1000));
    int n = 0, i = 0;
    while (i < 20) { i++; if (i % 3 == 0) continue; if (i > 15) break; n += i; }
    pn(n);
    do { n -= 7; } while (n > 0); pn(n);
    for (int a = 0, b = 10; a < b; a++, b--) n += a * b; pn(n);
    int k = 0;
loop:
    k++;
    if (k < 5) goto loop;
    pn(k);
    int t = 0;
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) { if (y == x) continue; if (y > 2) break; t += x * 10 + y; }
    pn(t);
    pn(({ int z = 5; z * z; }) + 1);
    return n & 127;
}
