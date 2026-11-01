int a; typeof(a) b; __typeof__(a + 1L) c; typeof(int *) d;
int f(void) { typeof(b) x = 3; return x + sizeof(typeof(c)); }
