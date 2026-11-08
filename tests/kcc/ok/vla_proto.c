void p1(int n, int a[*]);
void p2(int n, int m, int a[n][m]);
void p3(int n, int a[static n]);
int p4(int n, int (*f)(int m, int b[m]), int c[n]);
void p1(int n, int a[n]) { a[0] = n; }
