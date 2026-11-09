#include "chain.h"
long k_cb(long x) { return (x * x) % 1000003; }
long k_f(long x) {
    P p = { x, HALF(x) };
    P r = g_f(p, 3, 2.5);
    return r.a + (long)r.b;
}
