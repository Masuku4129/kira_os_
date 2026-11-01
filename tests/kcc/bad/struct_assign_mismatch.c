struct A { int a; }; struct B { int a; };
void f(struct A *a, struct B *b) { *a = *b; }
