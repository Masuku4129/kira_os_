struct S { const int k; int v; };
int f(struct S *s) { return s->k + s->v; }
