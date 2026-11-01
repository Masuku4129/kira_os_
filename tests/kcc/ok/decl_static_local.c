int f(void) { static int n = 5; static char buf[4]; n++; buf[0] = 1; return n; }
