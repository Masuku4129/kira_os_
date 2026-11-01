struct A; struct B { struct A *a; }; struct A { struct B *b; int v; };
int f(struct B *b) { return b->a->v; }
