struct N { struct N *next; int v; };
int f(struct N *n) { int s = 0; while (n) { s += n->v; n = n->next; } return s; }
