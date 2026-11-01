#include <kira.h>

static int twice(int a) {
    return a * 2;
}

int main(void) {
    int x = twice(21)
    k_puts("unreachable");
    return x;
}
