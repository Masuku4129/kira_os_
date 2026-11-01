struct S { int k; union { int i; char c; }; struct { int x, y; }; };
int f(struct S *s) { return s->i + s->x + s->y + s->k; }
