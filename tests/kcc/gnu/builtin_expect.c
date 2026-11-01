int f(int x) { if (__builtin_expect(x > 1, 1)) return 1; return __builtin_expect(!!x, 0); }
