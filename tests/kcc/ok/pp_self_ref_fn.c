int f(int y) { return y; }
#define f(x) f(x + 1)
int g(void) { return f(2); }
