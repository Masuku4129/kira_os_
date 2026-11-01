volatile int a; int volatile b; volatile int *c; int *volatile d; __volatile__ int e;
int f(void) { a = 1; return a + b + *c + *d + e; }
