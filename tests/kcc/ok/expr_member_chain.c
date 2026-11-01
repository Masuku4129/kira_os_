struct A { int x; } ; struct B { struct A a; struct A *p; };
int f(struct B *b) { return b->a.x + b->p->x + (*b).a.x; }
