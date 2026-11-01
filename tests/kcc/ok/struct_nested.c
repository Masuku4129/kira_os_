struct In { int a; }; struct Out { struct In i; struct In arr[2]; };
int f(struct Out *o) { return o->i.a + o->arr[1].a; }
