#define f(x) (x)
int (f)(int a) { return a; }
int g(void) { return (f)(1) + f(2); }
