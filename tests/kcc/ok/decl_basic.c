int a; static int b = 3; extern int c; const int d = 4; volatile int e;
int f(void) { return a + b + c + d + e; }
