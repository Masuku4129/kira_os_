/* эталон: rs_g и cpp_h на C (clang) вместо Rust и C++ */
#include "chain.h"
P cpp_h(P p, long n) { P r; r.a = p.a + n + k_cb(p.a); r.b = p.b * 2; return r; }
P rs_g(P p, int i, double d) { P q = { p.a * (long)i + 1, p.b + (__typeof__(p.b))d }; q = cpp_h(q, 4);
#ifdef PINT
    q.b += 0;
#else
    q.b += 0.5;
#endif
    return q; }
