/* _Alignas с числом и типом, _Alignof. */
#include <stdint.h>
_Alignas(64) static char buf[10];
_Alignas(long long) static char b2[3];
struct T { char c; _Alignas(16) int x; };
int main(void) {
    _Alignas(16) char loc[5];
    int r = (int)(((uintptr_t)buf) % 64) + (int)(((uintptr_t)loc) % 16);
    r += (int)_Alignof(struct T) + (int)sizeof(struct T) + (int)_Alignof(long long);
    loc[0] = 1; buf[0] = 2; b2[0] = 3;
    return r + loc[0] + buf[0] + b2[0];
}
