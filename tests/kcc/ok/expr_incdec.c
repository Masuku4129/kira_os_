int f(int *p) { int a = (*p)++; int b = --(*p); p++; --p; return a + b + p[0]--; }
