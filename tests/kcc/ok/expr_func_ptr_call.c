int add(int a, int b) { return a + b; }
int f(int (*op)(int, int)) { return op(1, 2) + (*op)(3, 4) + add(5, 6); }
