struct S { int (*fp)(int); };
int f(struct S s) { return s.nofp(1); }
