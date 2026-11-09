#include "abi_t.h"
extern int printf(const char *, ...);
static void L(const char *n, long v) { printf("%s=%ld\n", n, v); }
static long dq(double d) { return (long)(d * 1000); }
int main(void) {
    S8 s8 = { 7, -3 }; S8 r8 = f_s8(s8, 5); L("s8", r8.a * 1000 + r8.b);
    S16 s16 = { 10, 20 }; S16 r16 = f_s16(s16, 3); L("s16", r16.a * 1000 + r16.b);
    S40 s40 = { 1, 2, 3, 4, 5 }; S40 r40 = f_s40(s40, 10); L("s40", r40.a + r40.b * 10 + r40.c * 100 + r40.d * 1000 + r40.e * 10000);
    D16 d16 = { 1.5, 2.5 }; D16 rd = f_d16(d16, 2.0); L("d16", dq(rd.x) * 100000 + dq(rd.y));
    M16 m16 = { 9, 1.25 }; M16 rm = f_m16(m16, 4); L("m16", rm.a * 100000 + dq(rm.b));
    F12 f12 = { 1.0f, 2.0f, 3.0f }; F12 rf = f_f12(f12, 0.5f); L("f12", dq(rf.x) * 1000000 + dq(rf.y) * 1000 + dq(rf.z));
    Cm cm = { 3, 300, 70000 }; Cm rc = f_cm(cm, 4); L("cm", rc.c * 100000000L + rc.s * 100000L + rc.i);
    L("many_d", dq(f_many_d(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)));
    L("many_i", f_many_i(1, 2, 3, 4, 5, 6, 7, 8, 9));
    L("mix", f_mix(1, 2.5, 3, 4.5f, 5, 6, 7.5, 200));
    L("ext", f_ext(-5, 250, -300, 60000, 7));
    int big = 0x12345680; L("ret_sc", f_ret_sc(big) + 1000);       /* вызывающий обязан расширить знаково из al */
    L("ret_us", f_ret_us(0x1234ffff) + 1);
    L("vsum", f_vsum(3, 10, 20, 30, 1000L));
    L("vd", dq(f_vd(3, 1.5, 2.25, 3.125, 7)));
    L("cb", f_cb(f_cbtarget, 5));
    return 0;
}
