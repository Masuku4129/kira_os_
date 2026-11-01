struct S { int a; };
int f(struct S s, int c) { return c ? s : 1; }
