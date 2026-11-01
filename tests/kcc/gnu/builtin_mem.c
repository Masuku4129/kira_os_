void f(char *d, const char *s, unsigned long n) { __builtin_memcpy(d, s, n); __builtin_memset(d, 0, n); if (__builtin_memcmp(d, s, n)) d[0] = 1; }
