struct S { int a; }; struct S arr[3];
int f(int i) { arr[i].a = i; return arr[0].a + arr[2].a; }
