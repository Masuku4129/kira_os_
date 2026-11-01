int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
int main(void) { return fib(15) + fact(6) % 100; }
