int x = 1;
int f(void) { int x = 2; { int x = 3; x++; } return x; }
