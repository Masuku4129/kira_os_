struct F { int n; char data[]; };
int f(struct F *p) { return p->n + p->data[0]; }
