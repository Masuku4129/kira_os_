/* _Generic: выбор по типу управляющего выражения без вычисления; квалификаторы указуемого типа важны, верхние отбрасываются. */
#define KIND(x) _Generic((x), char: 1, unsigned char: 3, short: 4, unsigned short: 5, int: 6, unsigned: 7, \
                         long: 8, unsigned long: 9, long long: 10, unsigned long long: 11, const char *: 12, char *: 13, \
                         void *: 14, int *: 15, default: 99)
static int f(int x) { return x + 1; }
struct S { int a; };
int main(void) {
    int i = 0; const int ci = 1; char c = 'a'; unsigned short us = 3; long l = 5; unsigned long long ull = 9;
    char arr[4]; const char *cp = "x"; void *vp = 0; int *ip = &i; struct S s = {1}; int k = 0;
    int r = KIND(i) * 1 + KIND(ci) * 2 + KIND(c) * 3 + KIND(us) * 4 + KIND(l) * 5 + KIND(ull) * 6;
    r += KIND(arr) + KIND(cp) * 2 + KIND(vp) * 3 + KIND(ip) * 4 + KIND(1u) + KIND(2L) + KIND('a');
    r += _Generic(s, struct S: 1000, default: 0);
    r += _Generic(f, int (*)(int): 20, default: 0);
    r += _Generic(k++, int: 1, default: 0) * 0 + k;          /* управляющее выражение не вычисляется */
    return r & 0xffffff;
}
