#include "ktest.h"
static int calls;
static int next(void) { return ++calls; }
static const char *name(int c) {
    switch (c) {
    case 0: return "zero";
    case 1 ... 9: return "digit";
    case 10 ... 15: return "ten-ish";
    case -20 ... -10: return "neg";
    case 100: return "hundred";
    default: return "other";
    }
}
static int uclass(unsigned c) {
    switch (c) {
    case 'a' ... 'z': return 1;
    case 'A' ... 'Z': return 2;
    case 0xfffffff0u ... 0xffffffffu: return 3;
    default: return 0;
    }
}
static int cclass(char c) {
    switch (c) { case 'a' ... 'f': case 'A' ... 'F': return 16; case '0' ... '9': return c - '0'; default: return -1; }
}
int main(void) {
    int xs[] = { 0, 1, 5, 9, 10, 15, 16, -9, -10, -15, -20, -21, 100, 99 };
    for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) { ps(name(xs[k])); ps(" "); }
    ps("\n");
    pn(uclass('m')); pn(uclass('Q')); pn(uclass(0xfffffff5u)); pn(uclass(0x7fffffffu)); pn(uclass(65)); pn(uclass(0));
    pn(cclass('c')); pn(cclass('7')); pn(cclass('z')); pn(cclass('F'));
    /* a ?: b - первый операнд вычисляется один раз */
    int a = next() ?: 99; pn(a); pn(calls);
    int z = 0; int b = z ?: next(); pn(b); pn(calls);
    long l = 0; unsigned char uc = 7; pn(l ?: uc); pn(uc ?: l);
    char *p = 0; char buf[4]; pn((p ?: buf) == buf); p = buf; pn((p ?: 0) == buf);
    pn(0 ?: 5); pn(3 ?: 5);
    int n = 5; int m = (n--) ?: 7; pn(m); pn(n);
    return 0;
}
