int foo = 5;
#define foo (foo + 1)
int f(void) { return foo; }
