struct S { int a; char b; long c; };
int f(struct S *s) { s->a = 1; s->b = 2; return s->c; }
