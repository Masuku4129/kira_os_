#include "ktest.h"
static const char msg[] = "hello from kcc\n";
const char *names[] = { "alpha", "beta", "gamma" };
int main(void) {
    ps(msg);
    for (int i = 0; i < 3; i++) { ps(names[i]); ps(i < 2 ? ", " : "\n"); }
    pn(-12345678901L); pn(0); pn(9223372036854775807L);
    ps("0x"); px(0xdeadbeefcafeUL); ps("\n");
    return 7;
}
