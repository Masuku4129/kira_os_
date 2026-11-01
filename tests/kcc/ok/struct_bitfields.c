struct B { unsigned a : 3; int b : 5; unsigned : 0; unsigned c : 8; };
int f(struct B *p) { p->a = 5; p->b = -3; p->c++; return p->a + p->b + p->c; }
