#include "abi_t.h"
#define VA_START(ap, l) __builtin_va_start(ap, l)
#define VA_ARG(ap, t) __builtin_va_arg(ap, t)
#define VA_END(ap) __builtin_va_end(ap)
S8 f_s8(S8 s, int k) { S8 r = { s.a * k + 1, s.b - k }; return r; }
S16 f_s16(S16 s, long k) { S16 r = { s.a + k, s.b * k }; return r; }
S40 f_s40(S40 s, long k) { S40 r = { s.a + k, s.b + 2 * k, s.c + 3 * k, s.d + 4 * k, s.e + 5 * k }; return r; }
D16 f_d16(D16 d, double k) { D16 r = { d.x * k, d.y + k }; return r; }
M16 f_m16(M16 m, int k) { M16 r = { m.a + k, m.b * k }; return r; }
F12 f_f12(F12 f, float k) { F12 r = { f.x + k, f.y * k, f.z - k }; return r; }
Cm f_cm(Cm c, int k) { Cm r; r.c = (char)(c.c + k); r.s = (short)(c.s * k); r.i = c.i - k; return r; }
double f_many_d(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) { return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j; }
long f_many_i(long a, long b, long c, long d, long e, long f, long g, long h, long i) { return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i; }
long f_mix(int a, double b, long c, float d, char e, short f, double g, unsigned char h) { return a + (long)(b * 4) + c * 3 + (long)(d * 8) + e * 5 + f * 7 + (long)(g * 2) + h * 11; }
long f_ext(signed char a, unsigned char b, short c, unsigned short d, int e) { return a * 1000000L + b * 10000L + c * 100L + d + e; }
signed char f_ret_sc(int x) { return (signed char)x; }
unsigned short f_ret_us(int x) { return (unsigned short)x; }
long f_vsum(int n, ...) { va_list ap; VA_START(ap, n); long s = 0; for (int i = 0; i < n; i++) s += VA_ARG(ap, int) * (i + 1); long l = VA_ARG(ap, long); VA_END(ap); return s + l; }
double f_vd(int n, ...) { va_list ap; VA_START(ap, n); double s = 0; for (int i = 0; i < n; i++) { s += VA_ARG(ap, double); } int t = VA_ARG(ap, int); VA_END(ap); return s + t; }
long f_cb(long (*cb)(long, long), long x) { return cb(x, x + 1) * 3; }
long f_cbtarget(long a, long b) { return a * 100 + b; }
