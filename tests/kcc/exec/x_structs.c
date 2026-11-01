#include "ktest.h"
struct B { unsigned a : 3; int b : 5; unsigned c : 7; int d : 20; unsigned char e; };
union U { unsigned long l; unsigned char b[8]; short s[4]; };
struct N { struct N *next; int v; };
struct M { char c; struct { short a; long b; } in; int arr[3]; };
int main(void) {
    struct B s = {0};
    s.a = 9; s.b = -3; s.c = 100; s.d = -1000; s.e = 200;
    pn(s.a); pn(s.b); pn(s.c); pn(s.d); pn(s.e);
    s.a++; s.b--; s.c += 40; s.d *= 2; s.e = (unsigned char)(s.e + 100);
    pn(s.a); pn(s.b); pn(s.c); pn(s.d); pn(s.e);
    union U u; u.l = 0x0102030405060708UL;
    pn(u.b[0]); pn(u.b[7]); pn(u.s[1]); u.b[1] = 0xff; px(u.l); ps("\n");
    struct N n3 = {0, 3}, n2 = {&n3, 2}, n1 = {&n2, 1};
    int sum = 0; for (struct N *p = &n1; p; p = p->next) sum = sum * 10 + p->v; pn(sum);
    struct M m = { 'x', { 5, 77 }, { 1, 2, 3 } };
    struct M m2 = m; m2.in.b++; m2.arr[2] = 9;
    pn(m.in.b + m2.in.b); pn(m2.arr[2] + m.arr[2]); pn((long)sizeof(struct M)); pn((long)sizeof(struct B));
    struct M *pm = &m2; pm->arr[1] += 5; pn(m2.arr[1]);
    struct M z; __builtin_memset(&z, 0, sizeof z); pn(z.in.b);
    __builtin_memcpy(&z, &m, sizeof z); pn(z.in.a + z.c);
    pn(__builtin_memcmp(&z, &m, sizeof z)); z.arr[0] = 5; pn(__builtin_memcmp(&z, &m, sizeof z) > 0);
    pn((long)__builtin_strlen("kira os"));
    return 0;
}
