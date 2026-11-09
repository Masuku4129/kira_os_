#include "chain.h"
P g_f(P p, int i, double d) { P r = rs_g(p, i, d); r.a += 100; r.b -= BC(1.25); return r; }
