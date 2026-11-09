#ifdef PINT
struct P { long a; long b; };
#else
struct P { long a; double b; };
#endif
extern "C" long k_cb(long x);
extern "C" P cpp_h(P p, long n) { P r; r.a = p.a + n + k_cb(p.a); r.b = p.b * 2; return r; }
