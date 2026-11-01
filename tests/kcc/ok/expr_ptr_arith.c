int f(int *p, int *q) { int *r = p + 3; r = r - 1; return (int)(r - q) + *(p + 1) + 2[p]; }
