void stop(void) __attribute__((noreturn));
static inline __attribute__((always_inline)) int sq(int x) { return x * x; }
int unused_fn(int a __attribute__((unused))) { return 1; }
__attribute__((noinline)) int g(void) { return sq(2); }
