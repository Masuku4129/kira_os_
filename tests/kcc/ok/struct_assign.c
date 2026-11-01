struct S { int a, b; };
void f(struct S *x, struct S *y) { *x = *y; struct S t = *x; *y = t; }
