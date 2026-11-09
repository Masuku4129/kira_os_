/* общие объявления цепочки main(clang) -> k_f(kcc) -> g_f(gcc|clang) -> rs_g(rustc) -> cpp_h(clang++) -> k_cb(kcc)
   P: по умолчанию {long; double} (SysV: INTEGER + SSE -> rdi + xmm0, возврат rax + xmm0). -DPINT: {long; long} (оба INTEGER) - вариант без FP-поля. */
#ifdef PINT
typedef struct { long a; long b; } P;
#define HALF(x) ((x) / 2)
#define BC(c) ((long)(c))
#else
typedef struct { long a; double b; } P;
#define HALF(x) ((x) * 0.5)
#define BC(c) (c)
#endif
P g_f(P p, int i, double d);
P rs_g(P p, int i, double d);
P cpp_h(P p, long n);
long k_cb(long x);
long k_f(long x);
