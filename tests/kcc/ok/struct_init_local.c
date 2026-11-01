struct P { int x, y; };
int f(int v) { struct P p = {v, v + 1}; int a[3] = {v}; struct P q = p; return p.x + a[2] + q.y; }
