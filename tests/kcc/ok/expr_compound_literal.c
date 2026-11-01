struct P { int x, y; };
int f(void) { struct P p = (struct P){1, 2}; int *q = (int[]){1, 2, 3}; return p.x + q[2] + (&(struct P){5, 6})->y; }
