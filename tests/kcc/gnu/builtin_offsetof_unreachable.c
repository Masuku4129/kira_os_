struct S { int a; long b; };
unsigned long f(int x) { if (x) return __builtin_offsetof(struct S, b); __builtin_unreachable(); }
