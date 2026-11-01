int f(void) { int a = 1; { int a = 2; { int a = 3; (void)a; } } return a; }
