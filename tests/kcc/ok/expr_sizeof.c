struct S { char c; int i; };
unsigned long f(int a[10]) { return sizeof(int) + sizeof a + sizeof(struct S) + sizeof(char[7]) + sizeof(a[0]); }
