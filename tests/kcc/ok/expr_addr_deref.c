int f(int x) { int *p = &x; int **q = &p; return **q + *&x; }
