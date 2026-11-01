int g(int a, int b) { return a * b; }
int f(int x) { return g(g(x, 2), g(3, g(x, x))); }
