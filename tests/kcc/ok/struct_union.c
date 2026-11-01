union U { int i; char c[4]; long l; };
union U u = {5};
int f(union U *p) { return p->i + p->c[1]; }
