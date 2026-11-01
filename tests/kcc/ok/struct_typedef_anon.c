typedef struct { int a; int b; } PT;
PT p = {1, 2}; int f(PT *q) { return q->a + p.b; }
